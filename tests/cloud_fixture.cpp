// In-process test peer for the Open Bamboo publish/report contract. No sockets.
#include "bambu_abi.hpp"

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

struct Peer {
    OnMessageFn report;
    std::string subscribed;
    bool connected = false;
    unsigned sends = 0;
};

EXPORT void* bambu_network_create_agent(std::string) { return new Peer(); }
EXPORT int bambu_network_start(void*) { return 0; }
EXPORT int bambu_network_destroy_agent(void* p) { delete static_cast<Peer*>(p); return 0; }
EXPORT bool bambu_network_is_user_login(void*) { return true; }
EXPORT bool bambu_network_is_server_connected(void* p) { return static_cast<Peer*>(p)->connected; }
EXPORT int bambu_network_refresh_connection(void* p) { static_cast<Peer*>(p)->connected = true; return 0; }
EXPORT int bambu_network_set_on_message_fn(void* p, OnMessageFn fn) {
    static_cast<Peer*>(p)->report = std::move(fn);
    return 0;
}
EXPORT int bambu_network_add_subscribe(void* p, std::vector<std::string> ids) {
    auto& peer = *static_cast<Peer*>(p);
    if (ids.size() != 1 || !peer.connected || ids[0] == "denied") return -1;
    peer.subscribed = ids[0];
    // Like the native plugin, do not promise a printer-connected callback on subscribe.
    return 0;
}
EXPORT int bambu_network_send_message(void* p, std::string id, std::string, int, int) {
    auto& peer = *static_cast<Peer*>(p);
    if (id != peer.subscribed) return -4;
    if (id == "uncertain") return peer.sends++ == 0 ? -2 : 0;
    peer.report(id, R"({"print":{"command":"push_status"}})");
    peer.report(id, R"({"info":{"command":"get_version","sequence_id":"6"}})");
    if (id == "online") peer.report(id, R"({"info":{"command":"get_version","sequence_id":7,"module":[]}})");
    return 0;
}
