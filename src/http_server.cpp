#include "http_server.hpp"
#include "rpc_handler.hpp"
#include "json.hpp"

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

bool read_request(sock_t s, std::string* first_line, std::string* body) {
    std::string buf;
    char tmp[4096];
#if defined(_WIN32)
    int n = ::recv((SOCKET)s, tmp, sizeof(tmp), 0);
#else
    int n = ::recv((int)s, tmp, sizeof(tmp), 0);
#endif
    if (n <= 0) return false;
    buf.assign(tmp, n);
    // Look for \r\n\r\n separating headers from body.
    size_t hdr_end = buf.find("\r\n\r\n");
    size_t content_len = 0;
    if (hdr_end != std::string::npos) {
        std::string hdr = buf.substr(0, hdr_end);
        std::istringstream iss(hdr);
        bool first = true;
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first) { *first_line = line; first = false; }
            else {
                std::string lower = line;
                for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
                if (lower.rfind("content-length:", 0) == 0) {
                    content_len = (size_t)atoll(line.substr(line.find(':') + 1).c_str());
                }
            }
        }
    } else {
        *first_line = buf;
        return true;
    }
    // collect body
    size_t body_start = hdr_end + 4;
    size_t got = buf.size() - body_start;
    while (got < content_len) {
#if defined(_WIN32)
        int n = ::recv((SOCKET)s, tmp, sizeof(tmp), 0);
#else
        int n = ::recv((int)s, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) break;
        buf.append(tmp, n);
        got += n;
    }
    if (body_start < buf.size())
        *body = buf.substr(body_start, content_len);
    return true;
}

void send_all(sock_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#if defined(_WIN32)
        int n = ::send((SOCKET)s, data.data() + off, (int)(data.size() - off), 0);
#else
        int n = ::send((int)s, data.data() + off, data.size() - off, 0);
#endif
        if (n <= 0) break;
        off += n;
    }
}

void send_response(sock_t s, int status, const std::string& ct, const std::string& body) {
    std::string hdr = "HTTP/1.1 " + std::to_string(status) + " OK\r\n"
                      "Content-Type: " + ct + "\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n";
    send_all(s, hdr + body);
}

// ---- runtime state ---------------------------------------------------------
std::atomic<bool> g_running{false};
sock_t g_listen = 0;
int    g_http_port = 0;
std::thread g_accept_thread;
BambuBridge* g_bridge = nullptr;

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

// Extract a query parameter value (first match) from a query string.
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

void handle_conn(sock_t s) {
    std::string first_line, body;
    if (!read_request(s, &first_line, &body)) { close_sock(s); return; }

    std::string method, path, query;
    {
        std::istringstream iss(first_line);
        iss >> method >> path;
    }
    {
        size_t q = path.find('?');
        if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    }

    if (path == "/health") {
        Json r = Json::object();
        r.set("ok", Json::boolean(true));
        std::string ver = g_bridge ? g_bridge->version() : "";
        r.set("version", Json::s(ver));
        r.set("loaded", Json::boolean(g_bridge && g_bridge->is_loaded()));
        r.set("agent_ready", Json::boolean(g_bridge && g_bridge->agent_ready()));
        r.set("logged_in", Json::boolean(g_bridge && g_bridge->is_user_login()));
        send_response(s, 200, "application/json", json_dump(r));
        close_sock(s);
        return;
    }

    // ---- OAuth login (Bambu Studio style) --------------------------------
    if (path == "/login-url") {
        std::string host = g_bridge ? g_bridge->get_host_for_login() : "";
        std::string redirect = std::string("http://127.0.0.1:") + std::to_string(g_http_port) + "/sign-in/callback";
        std::string login_url = host + "/sign-in?from=studio&source=portal&slicerLoginType=ticket&to=" + url_encode(redirect);
        Json r = Json::object();
        r.set("ok", Json::boolean(true));
        r.set("host", Json::s(host));
        r.set("redirect_url", Json::s(redirect));
        r.set("url", Json::s(login_url));
        send_response(s, 200, "application/json", json_dump(r));
        close_sock(s);
        return;
    }

    if (path == "/login") {
        std::string host = g_bridge ? g_bridge->get_host_for_login() : "";
        std::string redirect = std::string("http://127.0.0.1:") + std::to_string(g_http_port) + "/sign-in/callback";
        std::string login_url = host + "/sign-in?from=studio&source=portal&slicerLoginType=ticket&to=" + url_encode(redirect);
        std::string hdr = "HTTP/1.1 302 Found\r\nLocation: " + login_url + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(s, hdr);
        close_sock(s);
        return;
    }

    if (path == "/sign-in/callback") {
        std::string ticket = url_query_param(query, "ticket");
        if (ticket.empty()) {
            send_response(s, 200, "text/html", "<html><body><h2>Authentication failed</h2><p>No ticket in callback.</p></body></html>");
            close_sock(s);
            return;
        }
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

bool start_http_server(BambuBridge& bridge, int port) {
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
