// In-process test peer for the Open Bamboo publish/report contract. No sockets.
#include "bambu_abi.hpp"
#include <mutex>
#include <string_view>

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

struct Peer {
    std::mutex mutex;
    OnMessageFn report, local_report;
    OnLocalConnectedFn local_connect;
    std::string subscribed, lan_device;
    bool logged_in = true;
    bool connected = false;
    unsigned sends = 0;
    unsigned commands = 0;
};

constexpr auto temperature = R"({"print":{"command":"push_status","nozzle_temper":215},"system":{"command":"ignored"}})";

EXPORT void* bambu_network_create_agent(std::string) { return new Peer(); }
EXPORT int bambu_network_start(void*) { return 0; }
EXPORT int bambu_network_destroy_agent(void* p) { delete static_cast<Peer*>(p); return 0; }
EXPORT int bambu_network_set_country_code(void* p, std::string country) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.logged_in = country != "US";
    return 0;
}
EXPORT bool bambu_network_is_user_login(void* p) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    return peer.logged_in;
}
EXPORT bool bambu_network_is_server_connected(void* p) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    return peer.connected;
}
EXPORT int bambu_network_refresh_connection(void* p) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.connected = true;
    return 0;
}
EXPORT int bambu_network_set_on_message_fn(void* p, OnMessageFn fn) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.report = std::move(fn);
    return 0;
}
EXPORT int bambu_network_add_subscribe(void* p, std::vector<std::string> ids) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    if (ids.size() != 1 || !peer.connected || ids[0] == "denied") return -1;
    peer.subscribed = ids[0];
    // Like the native plugin, do not promise a printer-connected callback on subscribe.
    return 0;
}
EXPORT int bambu_network_send_message(void* p, std::string id, std::string command, int, int) {
    auto& peer = *static_cast<Peer*>(p);
    OnMessageFn report;
    {
        std::lock_guard<std::mutex> lock(peer.mutex);
        ++peer.commands;
        if (id != peer.subscribed) return -4;
        if (id == "uncertain") return peer.sends++ == 0 ? -2 : 0;
        report = peer.report;
    }
    if (command.find(R"("command":"pushall")") != std::string::npos) {
        report(id, temperature);
        report(id, R"({"print":{"command":"push_status","malformed":01}})");
        report(id, R"({"print":{"command":"push_status","nonfinite":1e999}})");
        report(id, "{\"print\":{\"command\":\"push_status\",\"invalid_utf8\":\"\xff\"}}");
        report(id, R"({"print":{"command":"pause","result":"success"}})");
    } else if (command.find(R"("command":"pause")") != std::string::npos)
        report(id, R"({"print":{"command":"push_status","gcode_state":"PAUSE"}})");
    else if (command.find(R"("command":"resume")") != std::string::npos)
        report(id, R"({"print":{"command":"push_status","gcode_state":"RUNNING"}})");
    else if (command.find(R"("command":"stop")") != std::string::npos)
        report(id, R"({"print":{"command":"push_status","gcode_state":"IDLE"}})");
    else if (command.find("get_version") != std::string::npos) {
        report(id, R"({"print":{"command":"push_status"}})");
        report(id, R"({"info":{"command":"get_version","sequence_id":"6"}})");
        if (id == "online") report(id, R"({"info":{"command":"get_version","sequence_id":7,"module":[]}})");
        if (id == "storage-fixture") {
            constexpr std::string_view prefix = "\"sequence_id\":\"";
            const auto pos = command.find(prefix);
            if (pos == std::string::npos) return -1;
            const auto start = pos + prefix.size();
            const auto seq = command.substr(start, command.find('"', start) - start);
            report(id, "{\"info\":{\"command\":\"get_version\",\"sequence_id\":\"" + seq +
                       "\",\"module\":[{\"name\":\"ota\",\"sw_ver\":\"01.02.03.04\"}]}}");
        }
    }
    return 0;
}

EXPORT int bambu_network_get_camera_url(void*, std::string key, std::function<void(std::string)> callback) {
    const auto split = key.find('|');
    const auto id = key.substr(0, split);
    if (key == "storage-fixture|01.02.03.04|\"tutk\"" || id == "online")
        callback("bambu:///tutk?uid=fixture");
    else
        callback("bambu:///local/192.0.2.10?port=6000");
    return 0;
}

EXPORT int bambu_network_get_my_token(void*, std::string ticket, unsigned* status, std::string* body) {
    if (ticket != "web-ticket") return -1;
    *status = 200;
    *body = R"({"accessToken":"fixture-token","refreshToken":"fixture-refresh"})";
    return 0;
}
EXPORT int bambu_network_get_my_profile(void*, std::string token, unsigned* status, std::string* body) {
    if (token != "fixture-token") return -1;
    *status = 200;
    *body = R"({"uidStr":"fixture-user","name":"Fixture User"})";
    return 0;
}
EXPORT int bambu_network_change_user(void* p, std::string profile) {
    if (profile.find(R"("token":"fixture-token")") == std::string::npos ||
        profile.find(R"("user_id":"fixture-user")") == std::string::npos) return -1;
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.logged_in = true;
    return 0;
}
EXPORT int bambu_network_get_user_print_info(void*, unsigned* status, std::string* body) {
    *status = 200;
    *body = R"({"devices":[{"dev_id":"online","dev_name":"Fixture","dev_online":true,"dev_access_code":"private-fixture"},{"dev_name":"No ID"}]})";
    return 0;
}

EXPORT int bambu_network_set_on_local_connect_fn(void* p, OnLocalConnectedFn fn) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.local_connect = std::move(fn);
    return 0;
}
EXPORT int bambu_network_set_on_local_message_fn(void* p, OnMessageFn fn) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.local_report = std::move(fn);
    return 0;
}
EXPORT int bambu_network_connect_printer(void* p, std::string id, std::string ip,
                                        std::string username, std::string password, bool ssl) {
    if (id != "lan-fixture" || ip != "192.0.2.10" || username != "bblp" || password != "fixture-pass" || !ssl) return -1;
    auto& peer = *static_cast<Peer*>(p);
    OnLocalConnectedFn connect;
    {
        std::lock_guard<std::mutex> lock(peer.mutex);
        peer.lan_device = id;
        connect = peer.local_connect;
    }
    connect(0, id, "fixture local message"); // Bridge reenters send_message_to_printer synchronously.
    return 0;
}
EXPORT int bambu_network_send_message_to_printer(void* p, std::string id, std::string, int, int) {
    auto& peer = *static_cast<Peer*>(p);
    OnMessageFn report;
    {
        std::lock_guard<std::mutex> lock(peer.mutex);
        ++peer.commands;
        if (id != peer.lan_device) return -1;
        report = peer.local_report;
    }
    report(id, temperature);
    return 0;
}
EXPORT int bambu_network_disconnect_printer(void* p) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    peer.lan_device.clear();
    return 0;
}

// The test reads this counter via legacy status; the Web API must not expose it.
EXPORT std::string bambu_network_get_user_id(void* p) {
    auto& peer = *static_cast<Peer*>(p);
    std::lock_guard<std::mutex> lock(peer.mutex);
    return std::to_string(peer.commands);
}
