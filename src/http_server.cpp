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

void handle_conn(sock_t s) {
    std::string first_line, body;
    if (!read_request(s, &first_line, &body)) { close_sock(s); return; }

    std::string method, path;
    {
        std::istringstream iss(first_line);
        iss >> method >> path;
    }
    // strip query from path
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (path == "/health") {
        Json r = Json::object();
        r.set("ok", Json::boolean(true));
        std::string ver = g_bridge ? g_bridge->version() : "";
        r.set("version", Json::s(ver));
        r.set("loaded", Json::boolean(g_bridge && g_bridge->is_loaded()));
        send_response(s, 200, "application/json", json_dump(r));
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
    // Register a sink that broadcasts to SSE clients.
    static HttpEventSink sink;
    bridge.set_event_sink(&sink);

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
