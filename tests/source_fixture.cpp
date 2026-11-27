// Synthetic BambuSource companion. The bridge still dispatches HTTP and consumes the real CTRL ABI.
#include "bambu_tunnel.hpp"
#include <deque>
#include <string>
#include <string_view>

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C"
#endif

struct Tunnel {
    bool started = false;
    bool listed = false;
    bool internal = false;
    std::deque<std::string> replies;
    std::string data;
};

EXPORT int Bambu_Create(Bambu_Tunnel* out, const char* url) {
    const std::string_view path(url);
    if (path.rfind("bambu:///local/192.0.2.10.?port=6000&user=bblp&passwd=a%26b%2Bc&device=lan-fixture", 0) != 0 &&
        path.rfind("bambu:///tutk?uid=fixture&device=", 0) != 0) return -1;
    *out = new Tunnel();
    return Bambu_success;
}
EXPORT int Bambu_Open(Bambu_Tunnel) { return Bambu_success; }
EXPORT int Bambu_StartStream(Bambu_Tunnel, bool) { return -1; }
EXPORT int Bambu_StartStreamEx(Bambu_Tunnel h, int type) {
    static_cast<Tunnel*>(h)->started = type == 0x3001;
    return type == 0x3001 ? Bambu_success : -1;
}
EXPORT void Bambu_Close(Bambu_Tunnel) {}
EXPORT void Bambu_Destroy(Bambu_Tunnel h) { delete static_cast<Tunnel*>(h); }

EXPORT int Bambu_SendMessage(Bambu_Tunnel h, int type, const char* p, int size) {
    auto& t = *static_cast<Tunnel*>(h);
    if (!t.started || type != 0x3001) return -1;
    const std::string_view req(p, size);
    auto has = [&](std::string_view token) { return req.find(token) != req.npos; };
    if (has(R"("cmdtype":1)") && has(R"("sequence":1)")) {
        t.listed = true;
        t.internal = has(R"("storage":"internal")");
        t.replies.push_back(R"({"cmdtype":256,"sequence":1,"result":0})");
        if (has(R"("type":"video")")) {
            t.replies.push_back(R"({"cmdtype":1,"sequence":1,"result":17})");
        } else if (has(R"("type":"timelapse")")) {
            t.replies.push_back(R"({"cmdtype":1,"sequence":1,"result":0,"reply":{"file_lists":[{"name":"legacy.mp4","size":0}]}})");
        } else if (t.internal) {
            t.replies.push_back(R"({"cmdtype":1,"sequence":1,"result":0,"reply":{"file_lists":[{"name":"broken.3mf","path":"/userdata/model/broken.3mf","size":6}]}})");
        } else {
            t.replies.push_back(R"({"cmdtype":1,"sequence":1,"result":1,"reply":{"file_lists":[{"name":"\u6A21\u578B.3mf","path":"/cache/\u6A21\u578B.3mf","size":6,"ignored":"sensitive"}]}})");
            t.replies.push_back(R"({"cmdtype":1,"sequence":1,"result":0,"reply":{"file_lists":[{"name":"huge.3mf","path":"/cache/huge.3mf","size":536870913}]}})");
        }
        return 0;
    }
    if (!t.listed || !has(R"("cmdtype":4)") || !has(R"("sequence":2)")) return -1;
    if (has(R"("file":"legacy.mp4")")) {
        t.replies.push_back(R"({"cmdtype":4,"sequence":2,"result":0,"reply":{"offset":0,"size":0,"total":0}})");
    } else if (has("huge.3mf")) {
        t.replies.push_back(R"({"cmdtype":4,"sequence":2,"result":1,"reply":{"offset":0,"size":0,"total":536870913}})");
    } else {
        auto first = R"({"cmdtype":4,"sequence":2,"result":1,"reply":{"offset":0,"size":3,"total":6}})" "\n\n";
        t.replies.push_back(std::string(first) + std::string("\0\1\2", 3));
        auto final = t.internal
            ? R"({"cmdtype":4,"sequence":2,"result":0,"reply":{"offset":2,"size":3,"total":6}})"
            : R"({"cmdtype":4,"sequence":2,"result":0,"reply":{"offset":3,"size":3,"total":6}})";
        t.replies.push_back(std::string(final) + "\n\n\n\nX");
    }
    return 0;
}

EXPORT int Bambu_ReadSample(Bambu_Tunnel h, Bambu_Sample* sample) {
    auto& t = *static_cast<Tunnel*>(h);
    if (t.replies.empty()) return Bambu_would_block;
    t.data = std::move(t.replies.front());
    t.replies.pop_front();
    sample->buffer = reinterpret_cast<const unsigned char*>(t.data.data());
    sample->size = static_cast<int>(t.data.size());
    return Bambu_success;
}
