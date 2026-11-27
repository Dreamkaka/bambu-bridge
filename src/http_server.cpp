#include "http_server.hpp"
#include "http_web_api.hpp"
#include "http_websocket.hpp"
#include "rpc_handler.hpp"
#include "json.hpp"
#include "camera_stream.hpp"
#include "mcp_stdio.hpp"
#include <charconv>
#include <map>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <deque>
#include <condition_variable>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <chrono>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#  include <poll.h>
#  include <sys/time.h>
#  include <fcntl.h>
#endif

namespace bambu {

namespace {

std::atomic<bool> g_running{false};

// ---- platform socket wrappers ----------------------------------------------
sock_t make_listen(int port, bool loopback_only, std::string* err) {
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { if (err) *err = "socket failed"; return 0; }
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { if (err) *err = "bind failed"; ::closesocket(s); return 0; }
    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) { if (err) *err = "listen failed"; ::closesocket(s); return 0; }
    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        if (err) *err = "setting listener nonblocking failed";
        ::closesocket(s);
        return 0;
    }
    return (sock_t)s;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { if (err) *err = "socket failed"; return 0; }
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) { if (err) *err = "bind failed"; ::close(s); return 0; }
    if (::listen(s, SOMAXCONN) < 0) { if (err) *err = "listen failed"; ::close(s); return 0; }
    if (::fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
        if (err) *err = "setting listener nonblocking failed";
        ::close(s);
        return 0;
    }
    return (sock_t)s;
#endif
}

void close_sock(sock_t s) {
#if defined(_WIN32)
    ::closesocket((SOCKET)s);
#else
    ::close((int)s);
#endif
}

int recv_some(sock_t s, char* out, size_t capacity) {
#if defined(_WIN32)
    return ::recv((SOCKET)s, out, static_cast<int>(capacity), 0);
#else
    return static_cast<int>(::recv(s, out, capacity, 0));
#endif
}

int wait_readable(sock_t s, int timeout_ms) {
#if defined(_WIN32)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET((SOCKET)s, &fds);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return ::select(0, &fds, nullptr, nullptr, &timeout);
#else
    pollfd fd{s, POLLIN, 0};
    const int rc = ::poll(&fd, 1, timeout_ms);
    return rc < 0 && errno == EINTR ? 0 : rc;
#endif
}

bool set_stream_timeout(sock_t s) {
#if defined(_WIN32)
    const DWORD timeout = 2000;
    return ::setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    const timeval timeout{2, 0};
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

void shutdown_sock(sock_t s) {
#if defined(_WIN32)
    ::shutdown((SOCKET)s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

using Headers = std::map<std::string, std::string>;

std::string_view trim_http(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

char ascii_lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

std::string_view header_value(const Headers& headers, const char* name) {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string_view{} : std::string_view(it->second);
}

bool read_request(sock_t s, std::string* first_line, Headers& headers, std::string* body, std::string* unread) {
    std::string buf;
    char tmp[4096];
    auto receive = [&] {
        // Also observe stop while waiting for partial HTTP requests on Winsock.
        while (g_running) {
            const int ready = wait_readable(s, 100);
            if (ready < 0) return false;
            if (ready > 0) break;
        }
        if (!g_running) return false;
        const int n = recv_some(s, tmp, sizeof(tmp));
        if (n <= 0) return false;
        buf.append(tmp, n);
        return true;
    };
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() >= 32768 || !receive()) return false;
    }
    const auto first = buf.find("\r\n");
    first_line->assign(buf, 0, first);
    for (size_t pos = first + 2; pos < end;) {
        const auto next = buf.find("\r\n", pos);
        const std::string_view line(buf.data() + pos, next - pos);
        const auto colon = line.find(':');
        if (!colon || colon == std::string_view::npos) return false;
        std::string key(line.substr(0, colon));
        for (auto& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        const auto value = trim_http(line.substr(colon + 1));
        auto [it, inserted] = headers.try_emplace(key, value);
        if (!inserted) {
            if (key != "accept") return false;
            it->second.append(",").append(value);
        }
        pos = next + 2;
    }
    if (headers.count("transfer-encoding")) return false;
    size_t length = 0;
    if (const auto it = headers.find("content-length"); it != headers.end()) {
        const auto& value = it->second;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length > 16 * 1024 * 1024)
            return false;
    }
    const size_t offset = end + 4;
    while (buf.size() - offset < length) if (!receive()) return false;
    body->assign(buf, offset, length);
    unread->assign(buf, offset + length, std::string::npos);
    return true;
}

bool send_all(sock_t s, std::string_view data) {
    size_t off = 0;
    while (off < data.size()) {
#if defined(_WIN32)
        int n = ::send((SOCKET)s, data.data() + off, (int)(data.size() - off), 0);
#else
#  ifdef MSG_NOSIGNAL
        int n = ::send((int)s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#  else
        int n = ::send((int)s, data.data() + off, data.size() - off, 0);
#  endif
#endif
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

void send_response(sock_t s, int status, const std::string& ct, const std::string& body,
                   bool no_store = false, const char* extra = "") {
    const char* reason = status == 202 ? "Accepted" : status == 400 ? "Bad Request"
                       : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed" : status == 406 ? "Not Acceptable"
                       : status == 415 ? "Unsupported Media Type" : status == 502 ? "Bad Gateway"
                       : status == 409 ? "Conflict" : status == 413 ? "Payload Too Large"
                       : status == 426 ? "Upgrade Required" : status == 500 ? "Internal Server Error"
                       : status == 503 ? "Service Unavailable" : status == 504 ? "Gateway Timeout" : "OK";
    std::string hdr = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
                      "Content-Type: " + ct + "\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (no_store) hdr += "Cache-Control: no-store\r\n";
    hdr += extra;
    hdr += "Connection: close\r\n\r\n";
    if (send_all(s, hdr)) send_all(s, body);
}

// ---- runtime state ---------------------------------------------------------
sock_t g_listen = 0;
int    g_http_port = 0;
std::thread g_accept_thread;
BambuBridge* g_bridge = nullptr;
bool g_mcp_control = false;
std::atomic<bool> g_web_api{false};
std::filesystem::path g_web_root;

enum class ConnMode { http, stream };
std::mutex g_conn_mtx;
std::condition_variable g_conn_done;
std::map<sock_t, ConnMode> g_connections;

struct ConnectionOwner {
    sock_t socket;
    ~ConnectionOwner() {
        std::lock_guard<std::mutex> lock(g_conn_mtx);
        close_sock(socket);
        g_connections.erase(socket);
        if (g_connections.empty()) g_conn_done.notify_all();
    }
};

// Workers copy a shared wire buffer under this lock, then write without it.
std::mutex g_event_mtx;
struct EventQueue {
    static constexpr size_t max_message = 1024 * 1024;
    using Message = std::shared_ptr<const std::string>;
    std::deque<Message> messages;
    size_t bytes = 0;
    uint64_t next = 0;

    void push(Message wire) {
        if (!wire) return;
        const size_t size = wire->size();
        messages.push_back(std::move(wire));
        bytes += size;
        ++next;
        while (messages.size() > 128 || bytes > 4 * 1024 * 1024) {
            bytes -= messages.front()->size();
            messages.pop_front();
        }
    }

    Message take(uint64_t& cursor, bool& fell_behind) const {
        std::lock_guard<std::mutex> lock(g_event_mtx);
        const auto first = next - messages.size();
        fell_behind = cursor < first;
        if (fell_behind || cursor == next) return {};
        return messages[cursor++ - first];
    }
};
EventQueue g_sse_events, g_ws_events;
size_t g_ws_count = 0;

enum class StreamRegistration { ok, stopping, full };
StreamRegistration register_stream(sock_t s, bool ws, uint64_t& cursor) {
    std::scoped_lock lock(g_conn_mtx, g_event_mtx);
    if (!g_running) return StreamRegistration::stopping;
    if (ws && g_ws_count >= 16) return StreamRegistration::full;
    if (ws) ++g_ws_count;
    cursor = (ws ? g_ws_events : g_sse_events).next;
    g_connections.at(s) = ConnMode::stream;
    return StreamRegistration::ok;
}

struct WsSlot {
    ~WsSlot() {
        std::lock_guard<std::mutex> lock(g_event_mtx);
        --g_ws_count;
    }
};

Json event_envelope(const std::string& event, Json payload) {
    Json envelope = Json::object();
    envelope.set("event", Json::s(event));
    envelope.set("payload", std::move(payload));
    return envelope;
}

bool finite_json(const Json& value) {
    if (value.kind == Json::Number) return std::isfinite(value.num);
    for (const auto& item : value.obj) if (!finite_json(item.second)) return false;
    for (const auto& item : value.arr) if (!finite_json(item)) return false;
    return true;
}

EventQueue::Message event_wire(std::string wire) {
    if (wire.size() > EventQueue::max_message) return {};
    return std::make_shared<const std::string>(std::move(wire));
}

EventQueue::Message project_ws_event(const std::string& event, const Json& payload) {
    const auto field = [&](const char* name, Json::Kind kind) -> const Json* {
        const auto* value = payload.find(name);
        return value && value->kind == kind ? value : nullptr;
    };
    auto projected = Json::object();
    const char* name;
    const auto copy_int = [&](const char* key) {
        const auto* value = field(key, Json::Number);
        if (!value || !std::isfinite(value->num) || std::trunc(value->num) != value->num ||
            value->num < (std::numeric_limits<int>::min)() || value->num > (std::numeric_limits<int>::max)()) return false;
        projected.set(key, *value);
        return true;
    };
    if (event == "on_message" || event == "on_local_message") {
        const auto* id = field("dev_id", Json::String);
        const auto* msg = field("msg", Json::String);
        Json parsed;
        if (!id || !is_web_device_id(id->str) || !msg || !json_parse(msg->str, &parsed, true) ||
            parsed.kind != Json::Object || !finite_json(parsed)) return {};
        auto report = std::find_if(parsed.obj.begin(), parsed.obj.end(), [](const auto& entry) { return entry.first == "print"; });
        if (report == parsed.obj.end() || report->second.kind != Json::Object) return {};
        const auto* command = report->second.find("command");
        if (!command || command->kind != Json::String || command->str != "push_status") return {};
        projected.set("dev_id", *id);
        projected.set("source", Json::s(event == "on_message" ? "cloud" : "lan"));
        projected.set("report", std::move(report->second));
        name = "printer_status";
    } else if (event == "user_login" || event == "on_user_login") {
        const auto* login = field(event == "user_login" ? "logged_in" : "login", Json::Bool);
        if (!login) return {};
        projected.set("logged_in", *login);
        name = "account_changed";
    } else if (event == "login_result") {
        const auto* success = field("success", Json::Bool);
        if (!success) return {};
        projected.set("success", *success);
        name = "login_result";
    } else if (event == "on_server_connected") {
        if (!copy_int("return_code") || !copy_int("reason_code")) return {};
        name = "cloud_connection";
    } else if (event == "on_local_connect") {
        const auto* id = field("dev_id", Json::String);
        if (!id || !is_web_device_id(id->str) || !copy_int("status")) return {};
        projected.set("dev_id", *id);
        name = "lan_connection";
    } else return {};

    auto text = json_dump(event_envelope(name, std::move(projected)));
    if (text.size() > EventQueue::max_message || !websocket_utf8_valid(text)) return {};
    return event_wire(websocket_frame(0x1, text));
}

void publish_event(const std::string& event, const Json& payload) {
    if (!g_running) return;
    auto sse = event_wire("event: " + event + "\ndata: " + json_dump(event_envelope(event, payload)) + "\n\n");
    auto ws = g_web_api ? project_ws_event(event, payload) : nullptr;
    std::lock_guard<std::mutex> lock(g_event_mtx);
    if (!g_running) return;
    g_sse_events.push(std::move(sse));
    g_ws_events.push(std::move(ws));
}

class HttpEventSink : public EventSink {
public:
    void emit(const std::string& event, const Json& payload) override { publish_event(event, payload); }
    void on_state(const std::string& state) override {
        publish_event("state", Json::object().set("state", Json::s(state)));
    }
};

// ---- URL/query helpers ------------------------------------------------------
std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i+1]), lo = hex(in[i+2]);
            if (hi >= 0 && lo >= 0) { out.push_back((char)((hi << 4) | lo)); i += 2; }
            else out.push_back(in[i]);
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string url_query_param(const std::string& query, const std::string& key) {
    std::string needle = key + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string tok = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (tok.compare(0, needle.size(), needle) == 0)
            return url_decode(tok.substr(needle.size()));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

bool is_loopback_peer(sock_t s) {
    sockaddr_in peer{};
#ifdef _WIN32
    int len = sizeof(peer);
    const int rc = getpeername(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&peer), &len);
#else
    socklen_t len = sizeof(peer);
    const int rc = getpeername(s, reinterpret_cast<sockaddr*>(&peer), &len);
#endif
    return rc == 0 && peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
}

bool admits_web_request(sock_t s, const Headers& headers, bool websocket, bool oauth_callback) {
    if (!is_loopback_peer(s)) return false;
    std::string host(header_value(headers, "host"));
    for (char& ch : host) ch = ascii_lower(ch);
    const auto port = ":" + std::to_string(g_http_port);
    const bool localhost = host == "localhost" + port;
    if (!localhost && host != "127.0.0.1" + port) return false;
    if (headers.count("origin")) {
        if (header_value(headers, "origin") != "http://" + host) return false;
    } else if (websocket) return false;

    const auto site = header_value(headers, "sec-fetch-site");
    const bool has_site = headers.count("sec-fetch-site") != 0;
    const bool has_mode = headers.count("sec-fetch-mode") != 0;
    const bool has_dest = headers.count("sec-fetch-dest") != 0;
    if (oauth_callback) {
        return localhost && (!(has_site || has_mode || has_dest || headers.count("sec-fetch-user")) ||
               (header_value(headers, "sec-fetch-mode") == "navigate" &&
                header_value(headers, "sec-fetch-dest") == "document"));
    }
    return !has_site || site == "same-origin" || site == "none";
}

bool accepts_json(std::string_view accept) {
    while (!accept.empty()) {
        const auto comma = accept.find(',');
        auto item = accept.substr(0, comma);
        const auto semi = item.find(';');
        const auto type = trim_http(item.substr(0, semi));
        if (type == "application/json" || type == "application/*" || type == "*/*") {
            auto quality = semi == std::string_view::npos ? std::string_view{} : trim_http(item.substr(semi + 1));
            if (quality.rfind("q=", 0) != 0 || quality.substr(2).find_first_not_of("0.") != std::string_view::npos)
                return true;
        }
        if (comma == std::string_view::npos) break;
        accept.remove_prefix(comma + 1);
    }
    return false;
}

void handle_mcp(sock_t s, const std::string& method, const Headers& headers, const std::string& body) {
    const auto header = [&](const char* name) -> std::string_view {
        const auto it = headers.find(name);
        return it == headers.end() ? std::string_view{} : std::string_view(it->second);
    };
    const auto port = ":" + std::to_string(g_http_port);
    const auto ip = "127.0.0.1" + port, localhost = "localhost" + port;
    const auto host = header("host"), origin = header("origin");
    const auto reject = [&](int status, const char* message, const char* extra = "") {
        send_response(s, status, "text/plain; charset=utf-8", message, true, extra);
    };
    if (!is_loopback_peer(s) || (host != ip && host != localhost) ||
        (headers.count("origin") && origin != "http://" + ip && origin != "http://" + localhost))
        return reject(403, "MCP requires a local peer, Host and Origin");
    if (method != "POST") return reject(405, "MCP accepts POST only", "Allow: POST\r\n");
    const auto version = header("mcp-protocol-version");
    if (headers.count("mcp-protocol-version") && !supports_mcp_http_version(version))
        return reject(400, "Unsupported MCP-Protocol-Version");
    auto content_type = header("content-type");
    if (trim_http(content_type.substr(0, content_type.find(';'))) != "application/json")
        return reject(415, "MCP expects application/json");
    if (headers.count("accept") && !accepts_json(header("accept")))
        return reject(406, "MCP responses use application/json");
    const auto response = dispatch_mcp_http(*g_bridge, body, g_mcp_control);
    send_response(s, response.empty() ? 202 : 200, "application/json", response, true);
}

void send_web_response(sock_t s, const WebApiResponse& response) {
    send_response(s, response.status, "application/json; charset=utf-8", json_dump(response.body),
                  true, response.extra_headers.c_str());
}

bool ascii_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return ascii_lower(x) == ascii_lower(y);
    });
}

bool has_http_token(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (ascii_equal(trim_http(list.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

void ws_close(sock_t s, uint16_t code) {
    const char bytes[] = {static_cast<char>(code >> 8), static_cast<char>(code)};
    send_all(s, websocket_frame(0x8, std::string_view(bytes, sizeof(bytes))));
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void run_websocket(sock_t s, std::string& pending, uint64_t cursor) {
    if (!send_all(s, websocket_frame(0x1, R"({"event":"ready","payload":{}})"))) return;
    auto next_ping = Clock::now() + 30s;
    auto pong_deadline = Clock::time_point::max();
    int wait_ms = 0;
    while (g_running) {
        const int readable = wait_readable(s, wait_ms);
        if (!g_running) break;
        if (readable < 0) return;
        if (readable > 0) {
            char buf[4096];
            const int n = recv_some(s, buf, sizeof(buf));
            if (n <= 0) return;
            pending.append(buf, n);
        }
        WsFrame frame;
        while (true) {
            const auto result = websocket_consume(pending, frame);
            if (result == WsRead::incomplete) break;
            if (result != WsRead::frame) {
                ws_close(s, result == WsRead::invalid_utf8 ? 1007 : result == WsRead::unsupported_data ? 1003 : 1002);
                return;
            }
            if (frame.opcode == 0x8) { send_all(s, websocket_frame(0x8, frame.payload)); return; }
            if (frame.opcode == 0x9 && !send_all(s, websocket_frame(0xa, frame.payload))) return;
            if (frame.opcode == 0xa && frame.payload == "bridge" && Clock::now() < pong_deadline)
                pong_deadline = Clock::time_point::max();
        }
        const auto now = Clock::now();
        if (now >= pong_deadline) return;
        if (now >= next_ping) {
            if (!send_all(s, websocket_frame(0x9, "bridge"))) return;
            pong_deadline = now + 10s;
            next_ping = now + 30s;
        }
        bool fell_behind;
        auto wire = g_ws_events.take(cursor, fell_behind);
        if (fell_behind) { ws_close(s, 1013); return; }
        if (wire && !send_all(s, *wire)) return;
        wait_ms = wire ? 0 : 50;
    }
    ws_close(s, 1001);
}

void handle_websocket(sock_t s, const std::string& method, std::string_view version, std::string_view query,
                      const Headers& headers, const std::string& body, std::string& unread) {
    const auto reject = [&](int status, const char* code, const char* extra = "") {
        auto response = web_api_error(status, code);
        response.extra_headers = extra;
        send_web_response(s, response);
    };
    if (method != "GET") return reject(405, "method_not_allowed", "Allow: GET\r\n");
    if (!query.empty()) return reject(400, "invalid_query");
    if (!body.empty()) return reject(400, "invalid_body");
    if (version != "HTTP/1.1" || !ascii_equal(header_value(headers, "upgrade"), "websocket") ||
        !has_http_token(header_value(headers, "connection"), "upgrade"))
        return reject(400, "invalid_websocket_handshake");
    if (header_value(headers, "sec-websocket-version") != "13")
        return reject(426, "unsupported_websocket_version", "Sec-WebSocket-Version: 13\r\n");
    auto accept = websocket_accept(header_value(headers, "sec-websocket-key"));
    if (!accept) return reject(400, "invalid_websocket_handshake");
    if (!set_stream_timeout(s)) return;
    uint64_t cursor;
    const auto registration = register_stream(s, true, cursor);
    if (registration == StreamRegistration::full) return reject(503, "stream_capacity");
    if (registration != StreamRegistration::ok) return;
    const WsSlot slot;
    if (!send_all(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " + *accept + "\r\n\r\n")) return;
    run_websocket(s, unread, cursor);
}

void run_sse(sock_t s) {
    uint64_t cursor;
    if (!set_stream_timeout(s) || register_stream(s, false, cursor) != StreamRegistration::ok) return;
    if (!send_all(s, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n")) return;
    auto heartbeat = Clock::now() + 15s;
    int wait_ms = 0;
    while (g_running) {
        const int readable = wait_readable(s, wait_ms);
        if (!g_running || readable < 0) return;
        if (readable > 0) {
            char buf[512];
            if (recv_some(s, buf, sizeof(buf)) <= 0) return;
        }
        bool fell_behind;
        auto wire = g_sse_events.take(cursor, fell_behind);
        if (fell_behind) return;
        if (wire) {
            if (!send_all(s, *wire)) return;
            heartbeat = Clock::now() + 15s;
        } else if (Clock::now() >= heartbeat) {
            if (!send_all(s, ": keep-alive\n\n")) return;
            heartbeat = Clock::now() + 15s;
        }
        wait_ms = wire ? 0 : 50;
    }
}

bool safe_asset_name(std::string_view name) {
    if (name.empty() || name.size() > 180 || name == "." || name == "..") return false;
    for (unsigned char ch : name) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

std::string ui_mime(std::string_view rel) {
    const auto dot = rel.rfind('.');
    if (dot == std::string_view::npos) return {};
    const auto ext = rel.substr(dot);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    return {};
}

bool path_within(const std::filesystem::path& root, const std::filesystem::path& full) {
    const auto relative = full.lexically_relative(root);
    if (relative.empty()) return false;
    for (const auto& part : relative) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

std::filesystem::path executable_dir() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
#elif defined(__linux__)
    std::string buf(4096, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    buf.resize(static_cast<size_t>(n));
    return std::filesystem::path(buf).parent_path();
#else
    return {};
#endif
}

bool directory_has_index(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "index.html", ec);
}

void locate_web_root() {
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) candidates.push_back(cwd / "web" / "dist");
    const auto exe = executable_dir();
    if (!exe.empty()) {
        candidates.push_back(exe / "web" / "dist");
        candidates.push_back(exe.parent_path() / "web" / "dist");
    }
    for (const auto& candidate : candidates) {
        if (!directory_has_index(candidate)) continue;
        const auto canon = std::filesystem::weakly_canonical(candidate, ec);
        if (ec) continue;
        g_web_root = canon;
        std::fprintf(stdout, "[http] web ui: %s\n", g_web_root.string().c_str());
        return;
    }
}

bool load_ui_file(const std::filesystem::path& rel, std::string& body, std::string& type) {
    type = ui_mime(rel.generic_string());
    if (type.empty()) return false;
    std::error_code ec;
    const auto full = std::filesystem::weakly_canonical(g_web_root / rel, ec);
    if (ec || !path_within(g_web_root, full) || !std::filesystem::is_regular_file(full, ec) || ec) return false;
    const auto size = std::filesystem::file_size(full, ec);
    if (ec || size > 16 * 1024 * 1024) return false;
    std::ifstream in(full, std::ios::binary);
    if (!in) return false;
    body.assign(static_cast<size_t>(size), '\0');
    return size == 0 || static_cast<bool>(in.read(&body[0], static_cast<std::streamsize>(size)));
}

bool serve_web_ui(sock_t s, const std::string& path) {
    if (g_web_root.empty()) return false;
    std::filesystem::path rel;
    bool immutable = false;
    if (path == "/" || path == "/index.html") rel = "index.html";
    else if (path.rfind("/assets/", 0) == 0) {
        const auto name = std::string_view(path).substr(std::string_view("/assets/").size());
        if (!safe_asset_name(name)) return false;
        rel = std::filesystem::path("assets") / std::string(name);
        immutable = true;
    } else return false;

    std::string body, type;
    if (!load_ui_file(rel, body, type)) {
        send_response(s, 404, "text/plain; charset=utf-8", "Not Found", true);
        return true;
    }
    const char* cache = immutable ? "Cache-Control: public, max-age=31536000, immutable\r\n" : "";
    send_response(s, 200, type, body, !immutable, cache);
    return true;
}

void handle_conn(sock_t s) {
    std::string first_line, body, unread;
    Headers headers;
    if (!read_request(s, &first_line, headers, &body, &unread)) {
        send_response(s, 400, "text/plain", "Malformed HTTP request");
        return;
    }

    std::string method, path, query, version;
    {
        std::istringstream iss(first_line);
        iss >> method >> path >> version;
        if (std::string extra; iss >> extra) version.clear();
    }
    {
        size_t q = path.find('?');
        if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    }

    const bool v1 = path == "/api/v1" || path.rfind("/api/v1/", 0) == 0;
    if (v1) {
        const bool ws = path == "/api/v1/ws";
        if (!g_web_api) send_web_response(s, web_api_error(404, "not_found"));
        else if (!admits_web_request(s, headers, ws, false)) send_web_response(s, web_api_error(403, "forbidden_origin"));
        else if (ws) handle_websocket(s, method, version, query, headers, body, unread);
        else {
            bool downloading = false;
            const auto response = dispatch_web_api(*g_bridge, method, path, query, header_value(headers, "content-type"), body,
                g_running, [&](std::string_view name, std::string_view bytes) {
                    if (!downloading) {
                        if (!set_stream_timeout(s)) return false;
                        downloading = true;
                        constexpr char hex[] = "0123456789ABCDEF";
                        std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                          "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                                          "Transfer-Encoding: chunked\r\nConnection: close\r\n"
                                          "Content-Disposition: attachment; filename=\"printer-file\"; filename*=UTF-8''";
                        for (unsigned char c : name) { hdr += '%'; hdr += hex[c >> 4]; hdr += hex[c & 15]; }
                        hdr += "\r\n\r\n";
                        if (!send_all(s, hdr)) return false;
                    }
                    if (bytes.empty()) return true;
                    char prefix[32];
                    const int length = std::snprintf(prefix, sizeof(prefix), "%zx\r\n", bytes.size());
                    return send_all(s, std::string_view(prefix, static_cast<size_t>(length))) &&
                           send_all(s, bytes) && send_all(s, "\r\n");
                });
            if (!downloading) send_web_response(s, response);
            // Missing terminal chunk tells clients that a partially sent file failed.
            else if (response.status == 200) send_all(s, "0\r\n\r\n");
        }
        return;
    }

    const bool callback_path = method == "GET" && (path == "/" || path == "/sign-in/callback");
    const std::string ticket = (!g_web_api || callback_path) ? url_query_param(query, "ticket") : "";
    if (g_web_api && !admits_web_request(s, headers, false, callback_path && !ticket.empty())) {
        send_response(s, 403, "text/plain; charset=utf-8", "Forbidden origin", true);
        return;
    }

    if (path == "/mcp") {
        handle_mcp(s, method, headers, body);
        return;
    }

    if (path == "/health") {
        Json r = Json::object();
        r.set("ok", Json::boolean(true));
        std::string ver = g_bridge ? g_bridge->version() : "";
        r.set("version", Json::s(ver));
        r.set("client_version", Json::s(g_bridge ? g_bridge->client_version() : ""));
        r.set("loaded", Json::boolean(g_bridge && g_bridge->is_loaded()));
        r.set("agent_ready", Json::boolean(g_bridge && g_bridge->agent_ready()));
        r.set("logged_in", Json::boolean(g_bridge && g_bridge->is_user_login()));
        send_response(s, 200, "application/json", json_dump(r));
        return;
    }

    // ---- OAuth login (Bambu Studio style) --------------------------------
    auto locale = []() -> std::string {
        return (g_bridge && g_bridge->country_code() == "CN") ? "zh-CN" : "en";
    };

    if (path == "/login-url") {
        std::string host = g_bridge ? g_bridge->get_host_for_login() : "";
        std::string login_url = build_login_url(host, g_http_port, locale());
        Json r = Json::object();
        r.set("ok", Json::boolean(true));
        r.set("host", Json::s(host));
        r.set("redirect_url", Json::s("http://localhost:" + std::to_string(g_http_port)));
        r.set("url", Json::s(login_url));
        send_response(s, 200, "application/json", json_dump(r));
        return;
    }

    if (path == "/login") {
        std::string host = g_bridge ? g_bridge->get_host_for_login() : "";
        std::string login_url = build_login_url(host, g_http_port, locale());
        std::string hdr = "HTTP/1.1 302 Found\r\nLocation: " + login_url + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(s, hdr);
        return;
    }

    if (!ticket.empty()) {
        // Build a login_with_ticket RPC request and run it through the bridge.
        Json req = Json::object();
        req.set("method", Json::s("login_with_ticket"));
        Json prm = Json::object();
        prm.set("ticket", Json::s(ticket));
        req.set("params", prm);
        Json resp = handle_request(*g_bridge, req);
        bool success = false;
        if (const Json* ok = resp.find("ok"); ok && ok->kind == Json::Bool && ok->b) {
            const Json* result = resp.find("result");
            const Json* code = result ? result->find("code") : nullptr;
            success = code && code->kind == Json::Number && code->num == 0.0;
            if (!success && g_bridge) success = g_bridge->is_user_login();
        }
        publish_event("login_result", Json::object().set("success", Json::boolean(success)));
        std::string page = success
            ? "<html><head><meta charset=\"utf-8\"><style>body{font-family:Arial,sans-serif;margin:32px;}a.button{display:inline-block;padding:10px 16px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}</style></head><body><h2>Authentication complete</h2><p>You can return to your app. This window will close.</p><a class=\"button\" href=\"/health\">OK</a></body></html>"
            : "<html><head><meta charset=\"utf-8\"><style>body{font-family:Arial,sans-serif;margin:32px;}</style></head><body><h2>Authentication failed</h2><p>Something went wrong. Please try again.</p></body></html>";
        send_response(s, 200, "text/html", page);
        return;
    }

    if (path == "/sign-in/callback") {
        send_response(s, 200, "text/html", "<html><body><h2>Authentication failed</h2><p>No ticket in callback.</p></body></html>");
        return;
    }

    if (path == "/rpc" && method == "POST") {
        Json req;
        if (!json_parse(body, &req)) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s("bad JSON body"));
            send_response(s, 200, "application/json", json_dump(r));
            return;
        }
        Json resp = handle_request(*g_bridge, req);
        send_response(s, 200, "application/json", json_dump(resp));
        return;
    }

    if (method == "POST" && path.rfind("/api/", 0) == 0 && g_bridge) {
        std::string rpc_method = path.substr(5);
        Json req = Json::object();
        req.set("method", Json::s(rpc_method));
        Json params;
        if (body.empty()) params = Json::object();
        else if (!json_parse(body, &params)) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s("bad JSON body"));
            send_response(s, 200, "application/json", json_dump(r));
            return;
        }
        if (params.kind != Json::Object) params = Json::object();
        std::string qdev = url_query_param(query, "dev_id");
        if (!qdev.empty() && !params.find("dev_id")) params.set("dev_id", Json::s(qdev));
        req.set("params", params);
        Json resp = handle_request(*g_bridge, req);
        send_response(s, 200, "application/json", json_dump(resp));
        return;
    }

    if (path == "/camera.html" && method == "GET") {
        std::string html =
            "<!doctype html><meta charset=utf-8><title>bambu camera</title>"
            "<style>body{font-family:sans-serif;margin:24px;background:#111;color:#eee}video{width:100%;max-width:960px;background:#000}</style>"
            "<h2>printer camera</h2><p>needs <a href=https://github.com/video-dev/mpegts.js>mpegts.js</a> in browser, or open the ts URL in VLC.</p>"
            "<video id=v controls autoplay muted></video>"
            "<script src=https://cdn.jsdelivr.net/npm/mpegts.js@1.7.3/dist/mpegts.min.js></script>"
            "<script>const q=location.search||('?dev_id='+prompt('dev_id'));"
            "const url='/camera.ts'+q;"
            "if(mpegts.isSupported()){const p=mpegts.createPlayer({type:'mse',isLive:true,url:url});p.attachMediaElement(document.getElementById('v'));p.load();p.play();}"
            "else{document.body.insertAdjacentHTML('beforeend','<p>open in VLC: '+location.origin+url+'</p>');}</script>";
        send_response(s, 200, "text/html; charset=utf-8", html);
        return;
    }

    const bool snapshot = path == "/camera.jpg";
    if ((snapshot || path == "/camera.ts" || path == "/camera") && method == "GET" && g_bridge) {
        auto camera_error = [&](int status, const std::string& message) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s(message));
            send_response(s, snapshot ? status : 200, "application/json", json_dump(r), snapshot);
        };
        std::string dev = url_query_param(query, "dev_id");
        if (dev.empty()) {
            camera_error(400, "missing dev_id");
            return;
        }
        std::string cam_err;
        if (!camera_source_ready() && !load_camera_source(g_bridge->plugin_dir(), &cam_err) && snapshot) {
            camera_error(503, "BambuSource not loaded");
            return;
        }
        std::string cam_kind;
        std::string url = resolve_camera_url(*g_bridge, dev,
                                            url_query_param(query, "protocols"),
                                            url_query_param(query, "ip"),
                                            url_query_param(query, "user"),
                                            url_query_param(query, "password"),
                                            url_query_param(query, "proto"),
                                            url_query_param(query, "dev_ver"),
                                            &cam_err, &cam_kind);
        if (url.empty()) {
            camera_error(502, snapshot || cam_err.empty() ? "resolve camera URL failed" : cam_err);
            return;
        }
        if (url.rfind("bambu:///", 0) != 0) {
            if (snapshot) {
                camera_error(502, "BRTC/WebRTC URL is not supported for snapshots; try LAN");
                return;
            }
            Json r = Json::object();
            r.set("ok", Json::boolean(true));
            r.set("kind", Json::s(cam_kind.empty() ? "brtc" : cam_kind));
            r.set("url", Json::s(url));
            r.set("error", Json::s("cloud liveview is BRTC/WebRTC; this URL is not MPEG-TS. Use LAN proto=local or a WebRTC client"));
            send_response(s, 200, "application/json", json_dump(r));
            return;
        }
        if (snapshot) {
            std::string jpeg;
            auto status = capture_camera_jpeg(url, jpeg, cam_err);
            if (status == SnapshotStatus::ok) {
                send_response(s, 200, "image/jpeg", jpeg, true);
            } else {
                camera_error(status == SnapshotStatus::unavailable ? 503 :
                             status == SnapshotStatus::timeout ? 504 : 502, cam_err);
            }
            return;
        }
        std::string hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp2t\r\n"
            "Cache-Control: no-cache, no-store\r\n"
            "Connection: close\r\n";
        if (!g_web_api) hdr += "Access-Control-Allow-Origin: *\r\n";
        if (!send_all(s, hdr + "\r\n")) return;
        stream_camera_mpegts(url, g_running, [s](const unsigned char* d, size_t n) {
            size_t off = 0;
            while (off < n) {
#if defined(_WIN32)
                int w = ::send((SOCKET)s, (const char*)d + off, (int)(n - off), 0);
#else
                int w = ::send((int)s, (const char*)d + off, (int)(n - off), 0);
#endif
                if (w <= 0) return false;
                off += (size_t)w;
            }
            return true;
        }, &cam_err);
        return;
    }

    if (path == "/events" && method == "GET") return run_sse(s);

    if (g_web_api && method == "GET" && query.empty() && serve_web_ui(s, path)) return;

    send_response(s, 404, "text/plain", "Not Found");
}

} // namespace

bool init_sockets() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    return true;
}

int http_listen_port() { return g_http_port; }

std::string build_login_url(const std::string& host_in, int port, const std::string& locale) {
    std::string host = host_in;
    while (!host.empty() && host.back() == '/') host.pop_back();
    if (host.rfind("http://", 0) != 0 && host.rfind("https://", 0) != 0)
        host = "https://bambulab.com";
    std::string lang = locale.empty() ? "en" : locale;
    auto enc = [](const std::string& in) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : in) {
            bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved) out.push_back((char)c);
            else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        }
        return out;
    };
    std::string localhost = "http://localhost:" + std::to_string(port);
    std::string callback = host + "/sign-in/callback?source=portal&locale=" + enc(lang)
        + "&redirect_url=" + enc(localhost)
        + "&openBy=suite&from=studio&slicerLoginType=ticket";
    return host + "/sign-in?&from=studio&source=portal&to=" + enc(callback);
}

bool start_http_server(BambuBridge& bridge, int port, bool mcp_control, bool loopback_only, bool web_api) {
    std::string err;
    g_listen = make_listen(port, loopback_only || web_api, &err);
    if (!g_listen) { std::fprintf(stderr, "[http] %s\n", err.c_str()); return false; }

    g_web_api = web_api;
    g_web_root.clear();
    if (web_api) locate_web_root();
    g_mcp_control = mcp_control;
    g_bridge = &bridge;
    g_http_port = port;
    static HttpEventSink sink;
    try {
        bridge.add_event_sink(&sink);
        g_running = true;
        g_accept_thread = std::thread([] {
            while (g_running) {
                const int ready = wait_readable(g_listen, 100);
                if (ready < 0) break;
                if (!ready || !g_running) continue;
#if defined(_WIN32)
                SOCKET c = ::accept((SOCKET)g_listen, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                u_long nonblocking = 0;
                if (::ioctlsocket(c, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                    ::closesocket(c);
                    continue;
                }
#else
                int c = ::accept(g_listen, nullptr, nullptr);
                if (c < 0) continue;
#  ifdef SO_NOSIGPIPE
                int no_sigpipe = 1;
                ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#  endif
#endif
                const auto client = static_cast<sock_t>(c);
                try {
                    std::lock_guard<std::mutex> lock(g_conn_mtx);
                    g_connections.emplace(client, ConnMode::http);
                } catch (...) {
                    close_sock(client);
                    continue;
                }
                try {
                    std::thread([client] {
                        const ConnectionOwner owner{client};
                        try {
                            if (g_running) handle_conn(client);
                        } catch (...) {
                            std::fprintf(stderr, "[http] connection handler failed\n");
                        }
                    }).detach();
                } catch (...) {
                    const ConnectionOwner owner{client};
                }
            }
        });
    } catch (...) {
        stop_http_server();
        return false;
    }
    std::fprintf(stdout, "[http] listening on %s:%d\n", (loopback_only || web_api) ? "127.0.0.1" : "0.0.0.0", port);
    std::fprintf(stdout, "[mcp] Streamable HTTP: http://127.0.0.1:%d/mcp\n", port);
    std::fflush(stdout);
    return true;
}

void stop_http_server() {
    {
        std::lock_guard<std::mutex> lock(g_event_mtx);
        g_running = false;
    }
    if (g_accept_thread.joinable()) g_accept_thread.join();
    if (g_listen) { close_sock(g_listen); g_listen = 0; }
    {
        std::unique_lock<std::mutex> lock(g_conn_mtx);
        for (const auto& entry : g_connections)
            if (entry.second == ConnMode::http) shutdown_sock(entry.first);
        g_conn_done.wait(lock, [] { return g_connections.empty(); });
    }
    {
        std::lock_guard<std::mutex> lock(g_event_mtx);
        g_ws_events = {};
        g_sse_events = {};
    }
    g_bridge = nullptr;
}

} // namespace bambu
