#include "stdio_transport.hpp"
#include "rpc_handler.hpp"
#include "json.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

namespace bambu {

// Serialize access to stdout so event lines and response lines do not interleave.
static std::mutex g_stdout_mtx;

namespace {

void emit_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_stdout_mtx);
    std::cout << line << std::endl;
    std::cout.flush();
}

class StdioEventSink : public EventSink {
public:
    void emit(const std::string& event, const Json& payload) override {
        Json j = Json::object();
        j.set("event", Json::s(event));
        j.set("payload", payload);
        emit_line(json_dump(j));
    }
    void on_state(const std::string& state) override {
        Json j = Json::object();
        j.set("event", Json::s("state"));
        j.set("payload", Json::object().set("state", Json::s(state)));
        emit_line(json_dump(j));
    }
};

} // namespace

void run_stdio(BambuBridge& bridge) {
    StdioEventSink sink;
    bridge.add_event_sink(&sink);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json req;
        if (!json_parse(line, &req)) {
            Json r = Json::object();
            r.set("ok", Json::boolean(false));
            r.set("error", Json::s("bad JSON request"));
            emit_line(json_dump(r));
            continue;
        }
        Json resp = handle_request(bridge, req);
        emit_line(json_dump(resp));
    }

    // EOF; nothing to do. The caller may be joining a shutdown.
}

} // namespace bambu
