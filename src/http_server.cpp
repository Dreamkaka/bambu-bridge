#include "http_server.hpp"
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
#include <deque>

#include <chrono>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#endif

namespace bambu {

namespace {

// ---- platform socket wrappers ----------------------------------------------
sock_t make_listen(int port, std::string* err) {
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { if (err) *err = "socket failed"; return 0; }
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { if (err) *err = "bind failed"; ::closesocket(s); return 0; }
    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) { if (err) *err = "listen failed"; ::closesocket(s); return 0; }
    return (sock_t)s;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { if (err) *err = "socket failed"; return 0; }
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) { if (err) *err = "bind failed"; ::close(s); return 0; }
    if (::listen(s, SOMAXCONN) < 0) { if (err) *err = "listen failed"; ::close(s); return 0; }
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

using Headers = std::map<std::string, std::string>;

std::string_view trim_http(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

bool read_request(sock_t s, std::string* first_line, Headers& headers, std::string* body) {
    std::string buf;
    char tmp[4096];
    auto receive = [&] {
#if defined(_WIN32)
        int n = ::recv((SOCKET)s, tmp, sizeof(tmp), 0);
#else
        int n = ::recv((int)s, tmp, sizeof(tmp), 0);
#endif
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
    return true;
}

bool send_all(sock_t s, const std::string& data) {
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
std::atomic<bool> g_running{false};
sock_t g_listen = 0;
int    g_http_port = 0;
std::thread g_accept_thread;
BambuBridge* g_bridge = nullptr;
bool g_mcp_control = false;

// Event broadcast for /events (SSE).
std::mutex g_event_mtx;
std::deque<std::string> g_events;         // pending unsent events
std::vector<sock_t> g_sse_clients;

class HttpEventSink : public EventSink {
public:
    void emit(const std::string& event, const Json& payload) override {
        Json j = Json::object();
        j.set("event", Json::s(event));
        j.set("payload", payload);
        std::string line = "event: " + event + "\ndata: " + json_dump(j) + "\n\n";
        std::lock_guard<std::mutex> lk(g_event_mtx);
        g_events.push_back(line);
        for (auto& s : g_sse_clients) send_all(s, line);
    }
    void on_state(const std::string& state) override {
        Json j = Json::object();
        j.set("event", Json::s("state"));
        j.set("payload", Json::object().set("state", Json::s(state)));
        std::string line = "event: state\ndata: " + json_dump(j) + "\n\n";
        std::lock_guard<std::mutex> lk(g_event_mtx);
        g_events.push_back(line);
        for (auto& s : g_sse_clients) send_all(s, line);
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

std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) out.push_back((char)c);
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
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

bool mcp_loopback_peer(sock_t s) {
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
    if (!mcp_loopback_peer(s) || (host != ip && host != localhost) ||
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

void handle_conn(sock_t s) {
    std::string first_line, body;
    Headers headers;
    if (!read_request(s, &first_line, headers, &body)) {
        send_response(s, 400, "text/plain", "Malformed HTTP request");
        close_sock(s);
        return;
    }

    std::string method, path, query;
    {
        std::istringstream iss(first_line);
        iss >> method >> path;
    }
    {
        size_t q = path.find('?');
        if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    }

    if (path == "/mcp") {
        handle_mcp(s, method, headers, body);
        close_sock(s);
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
        close_sock(s);
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
        close_sock(s);
        return;
    }

    if (path == "/login") {
        std::string host = g_bridge ? g_bridge->get_host_for_login() : "";
        std::string login_url = build_login_url(host, g_http_port, locale());
        std::string hdr = "HTTP/1.1 302 Found\r\nLocation: " + login_url + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(s, hdr);
        close_sock(s);
        return;
    }

    std::string ticket = url_query_param(query, "ticket");
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
        // Tell the /events SSE about the login result.
        if (g_bridge) {
            Json ev = Json::object();
            ev.set("event", Json::s("login_result"));
            ev.set("payload", Json::object().set("success", Json::boolean(success)));
            std::lock_guard<std::mutex> lk(g_event_mtx);
            g_events.push_back("event: login_result\ndata: " + json_dump(ev) + "\n\n");
        }
        std::string page = success
            ? "<html><head><meta charset=\"utf-8\"><style>body{font-family:Arial,sans-serif;margin:32px;}a.button{display:inline-block;padding:10px 16px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}</style></head><body><h2>Authentication complete</h2><p>You can return to your app. This window will close.</p><a class=\"button\" href=\"/health\">OK</a></body></html>"
            : "<html><head><meta charset=\"utf-8\"><style>body{font-family:Arial,sans-serif;margin:32px;}</style></head><body><h2>Authentication failed</h2><p>Something went wrong. Please try again.</p></body></html>";
        send_response(s, 200, "text/html", page);
        close_sock(s);
        return;
    }

    if (path == "/sign-in/callback") {
        send_response(s, 200, "text/html", "<html><body><h2>Authentication failed</h2><p>No ticket in callback.</p></body></html>");
        close_sock(s);
        return;
    }

    if (path == "/rpc" && method == "POST") {
        Json req;
        if (!json_parse(body, &req)) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s("bad JSON body"));
            send_response(s, 200, "application/json", json_dump(r));
            close_sock(s);
            return;
        }
        Json resp = handle_request(*g_bridge, req);
        send_response(s, 200, "application/json", json_dump(resp));
        close_sock(s);
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
            close_sock(s);
            return;
        }
        if (params.kind != Json::Object) params = Json::object();
        std::string qdev = url_query_param(query, "dev_id");
        if (!qdev.empty() && !params.find("dev_id")) params.set("dev_id", Json::s(qdev));
        req.set("params", params);
        Json resp = handle_request(*g_bridge, req);
        send_response(s, 200, "application/json", json_dump(resp));
        close_sock(s);
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
        close_sock(s);
        return;
    }

    const bool snapshot = path == "/camera.jpg";
    if ((snapshot || path == "/camera.ts" || path == "/camera") && method == "GET" && g_bridge) {
        auto camera_error = [&](int status, const std::string& message) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s(message));
            send_response(s, snapshot ? status : 200, "application/json", json_dump(r), snapshot);
            close_sock(s);
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
            close_sock(s);
            return;
        }
        if (snapshot) {
            std::string jpeg;
            auto status = capture_camera_jpeg(url, jpeg, cam_err);
            if (status == SnapshotStatus::ok) {
                send_response(s, 200, "image/jpeg", jpeg, true);
                close_sock(s);
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
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n";
        send_all(s, hdr);
        std::atomic<bool> run{true};
        stream_camera_mpegts(url, run, [s](const unsigned char* d, size_t n) {
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
        close_sock(s);
        return;
    }

    if (path == "/events" && method == "GET") {
        // Server-Sent Events: keep the connection open and stream events.
        std::string hdr = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: keep-alive\r\n\r\n";
        send_all(s, hdr);
        {
            std::lock_guard<std::mutex> lk(g_event_mtx);
            g_sse_clients.push_back(s);
        }
        // drain any buffered events
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            // Keep-alive comment keeps some proxies happy.
        }
        {
            std::lock_guard<std::mutex> lk(g_event_mtx);
            for (size_t i = 0; i < g_sse_clients.size(); ++i)
                if (g_sse_clients[i] == s) { g_sse_clients.erase(g_sse_clients.begin() + i); break; }
        }
        close_sock(s);
        return;
    }

    send_response(s, 404, "text/plain", "Not Found");
    close_sock(s);
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

bool start_http_server(BambuBridge& bridge, int port, bool mcp_control) {
    g_mcp_control = mcp_control;
    g_bridge = &bridge;
    g_http_port = port;
    // Register a sink that broadcasts to SSE clients.
    static HttpEventSink sink;
    bridge.add_event_sink(&sink);

    std::string err;
    g_listen = make_listen(port, &err);
    if (!g_listen) { std::fprintf(stderr, "[http] %s\n", err.c_str()); return false; }

    g_running = true;
    g_accept_thread = std::thread([port]() {
        while (g_running) {
#if defined(_WIN32)
            SOCKET c = ::accept((SOCKET)g_listen, nullptr, nullptr);
            if (c == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
#else
            int c = ::accept((int)g_listen, nullptr, nullptr);
            if (c < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
#  ifdef SO_NOSIGPIPE
            int no_sigpipe = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#  endif
#endif
            std::thread(handle_conn, (sock_t)c).detach();
        }
    });
    std::fprintf(stdout, "[http] listening on 0.0.0.0:%d\n", port);
    std::fflush(stdout);
    return true;
}

void stop_http_server() {
    g_running = false;
    if (g_listen) { close_sock(g_listen); g_listen = 0; }
    if (g_accept_thread.joinable()) g_accept_thread.join();
}

} // namespace bambu
