#include "camera_stream.hpp"
#include "bambu_tunnel.hpp"
#include "bridge.hpp"
#include "json.hpp"

#include <cstring>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>

namespace bambu {

namespace {

template <typename FnPtr>
FnPtr load_sym(void* p) {
    FnPtr f = nullptr;
    std::memcpy(&f, &p, sizeof(f));
    return f;
}

PluginHandle g_src;
std::mutex g_src_mtx;
bool g_src_inited = false;

fn_Bambu_Create            p_create = nullptr;
fn_Bambu_SetLogger         p_set_logger = nullptr;
fn_Bambu_Open              p_open = nullptr;
fn_Bambu_StartStream       p_start = nullptr;
fn_Bambu_GetStreamCount    p_count = nullptr;
fn_Bambu_GetStreamInfo     p_info = nullptr;
fn_Bambu_ReadSample        p_read = nullptr;
fn_Bambu_Close             p_close = nullptr;
fn_Bambu_Destroy           p_destroy = nullptr;
fn_Bambu_Init              p_init = nullptr;
fn_Bambu_Deinit            p_deinit = nullptr;
fn_Bambu_GetLastErrorMsg   p_lasterr = nullptr;
fn_Bambu_FreeLogMsg        p_free_log = nullptr;

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void bambu_log(void*, int, bambu_tchar const* msg) {
    if (p_free_log && msg) p_free_log(msg);
}

bool nalu_is_aud(const unsigned char* p, size_t n) {
    if (n < 5) return false;
    size_t i = 0;
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) i = 4;
    else if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) i = 3;
    else return false;
    return (p[i] & 0x1f) == 9;
}

bool looks_annexb(const unsigned char* p, size_t n) {
    if (n < 4) return false;
    if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    if (p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    return false;
}

std::vector<unsigned char> avcc_to_annexb(const unsigned char* p, size_t n) {
    std::vector<unsigned char> out;
    size_t i = 0;
    while (i + 4 <= n) {
        unsigned len = (unsigned(p[i]) << 24) | (unsigned(p[i + 1]) << 16) |
                       (unsigned(p[i + 2]) << 8) | unsigned(p[i + 3]);
        i += 4;
        if (len == 0 || i + len > n) break;
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
        out.insert(out.end(), p + i, p + i + len);
        i += len;
    }
    return out;
}

struct Crc32 {
    unsigned t[256]{};
    Crc32() {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i << 24;
            for (int j = 0; j < 8; ++j)
                c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
            t[i] = c;
        }
    }
    unsigned run(const unsigned char* p, size_t n, unsigned c = 0xffffffffu) const {
        for (size_t i = 0; i < n; ++i)
            c = (c << 8) ^ t[((c >> 24) ^ p[i]) & 0xff];
        return c;
    }
};

const Crc32& crc32() {
    static Crc32 c;
    return c;
}

void write_be32(unsigned char* p, unsigned v) {
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}

std::vector<unsigned char> make_pat() {
    unsigned char pkt[188];
    std::memset(pkt, 0xff, 188);
    pkt[0] = 0x47; pkt[1] = 0x40; pkt[2] = 0x00; pkt[3] = 0x10;
    pkt[4] = 0x00;
    unsigned char* s = pkt + 5;
    s[0] = 0x00; s[1] = 0xb0; s[2] = 13;
    s[3] = 0x00; s[4] = 0x01; s[5] = 0xc1; s[6] = 0x00; s[7] = 0x00;
    s[8] = 0x00; s[9] = 0x01; s[10] = 0xe1; s[11] = 0x00;
    unsigned crc = crc32().run(s, 12);
    write_be32(s + 12, crc);
    return std::vector<unsigned char>(pkt, pkt + 188);
}

std::vector<unsigned char> make_pmt() {
    unsigned char pkt[188];
    std::memset(pkt, 0xff, 188);
    pkt[0] = 0x47; pkt[1] = 0x41; pkt[2] = 0x00; pkt[3] = 0x10;
    pkt[4] = 0x00;
    unsigned char* s = pkt + 5;
    s[0] = 0x02; s[1] = 0xb0; s[2] = 18;
    s[3] = 0x00; s[4] = 0x01; s[5] = 0xc1; s[6] = 0x00; s[7] = 0x00;
    s[8] = 0xe1; s[9] = 0x01;
    s[10] = 0xf0; s[11] = 0x00;
    s[12] = 0x1b; s[13] = 0xe1; s[14] = 0x01; s[15] = 0xf0; s[16] = 0x00;
    unsigned crc = crc32().run(s, 17);
    write_be32(s + 17, crc);
    return std::vector<unsigned char>(pkt, pkt + 188);
}

void write_pts(unsigned char* p, int four_bits, unsigned long long pts) {
    p[0] = (unsigned char)((four_bits << 4) | (((pts >> 30) & 0x07) << 1) | 1);
    p[1] = (unsigned char)((pts >> 22) & 0xff);
    p[2] = (unsigned char)((((pts >> 15) & 0x7f) << 1) | 1);
    p[3] = (unsigned char)((pts >> 7) & 0xff);
    p[4] = (unsigned char)(((pts & 0x7f) << 1) | 1);
}

std::vector<unsigned char> pes_packets(const unsigned char* nal, size_t n,
                                       unsigned long long pts90, bool key, unsigned& cc) {
    std::vector<unsigned char> pes;
    pes.resize(14);
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xe0;
    pes[4] = 0x00; pes[5] = 0x00;
    pes[6] = 0x80; pes[7] = 0x80; pes[8] = 5;
    write_pts(&pes[9], 0x02, pts90);
    pes.insert(pes.end(), nal, nal + n);

    std::vector<unsigned char> out;
    size_t off = 0;
    bool first = true;
    while (off < pes.size()) {
        unsigned char pkt[188];
        std::memset(pkt, 0xff, 188);
        pkt[0] = 0x47;
        pkt[1] = (unsigned char)((first ? 0x40 : 0x00) | 0x01);
        pkt[2] = 0x01;
        size_t remain = pes.size() - off;
        size_t payload_room = 184;
        size_t header = 4;
        if (first) {
            size_t adapt = 8;
            if (remain < 188 - 4 - adapt) adapt = 188 - 4 - remain;
            pkt[3] = (unsigned char)(0x30 | (cc & 0x0f));
            pkt[4] = (unsigned char)(adapt - 1);
            pkt[5] = (unsigned char)(0x10 | (key ? 0x40 : 0x00));
            unsigned long long pcr = pts90 * 300ULL;
            unsigned long long pcr_base = pcr / 300ULL;
            unsigned pcr_ext = (unsigned)(pcr % 300ULL);
            pkt[6] = (unsigned char)((pcr_base >> 25) & 0xff);
            pkt[7] = (unsigned char)((pcr_base >> 17) & 0xff);
            pkt[8] = (unsigned char)((pcr_base >> 9) & 0xff);
            pkt[9] = (unsigned char)((pcr_base >> 1) & 0xff);
            pkt[10] = (unsigned char)(((pcr_base & 1) << 7) | 0x7e | ((pcr_ext >> 8) & 1));
            pkt[11] = (unsigned char)(pcr_ext & 0xff);
            header = 4 + adapt;
            payload_room = 188 - header;
            first = false;
        } else if (remain < 184) {
            size_t stuff = 184 - remain;
            pkt[3] = (unsigned char)(0x30 | (cc & 0x0f));
            if (stuff == 1) {
                pkt[4] = 0;
                header = 5;
            } else {
                pkt[4] = (unsigned char)(stuff - 1);
                if (stuff > 1) pkt[5] = 0x00;
                header = 4 + stuff;
            }
            payload_room = 188 - header;
        } else {
            pkt[3] = (unsigned char)(0x10 | (cc & 0x0f));
        }
        cc = (cc + 1) & 0x0f;
        size_t take = payload_room < remain ? payload_room : remain;
        std::memcpy(pkt + header, pes.data() + off, take);
        out.insert(out.end(), pkt, pkt + 188);
        off += take;
    }
    return out;
}

bool is_keyframe(const unsigned char* p, size_t n) {
    for (size_t i = 0; i + 4 < n; ++i) {
        size_t sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) sc = i + 3;
        else if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = i + 4;
        if (!sc) continue;
        int t = p[sc] & 0x1f;
        if (t == 5 || t == 7) return true;
    }
    return false;
}

} // namespace

bool camera_source_ready() {
    std::lock_guard<std::mutex> lk(g_src_mtx);
    return g_src.is_loaded() && p_create && p_open && p_start && p_read;
}

bool load_camera_source(const std::string& plugin_dir, std::string* err) {
    std::lock_guard<std::mutex> lk(g_src_mtx);
    if (g_src.is_loaded() && p_create) return true;
    std::string e;
    PluginHandle h = load_source_plugin(plugin_dir, &e);
    if (!h.is_loaded()) {
        if (err) *err = e.empty() ? "BambuSource not found" : e;
        return false;
    }
    auto S = [&](const char* n) { return h.resolve(n); };
    p_create = load_sym<fn_Bambu_Create>(S("Bambu_Create"));
    p_set_logger = load_sym<fn_Bambu_SetLogger>(S("Bambu_SetLogger"));
    p_open = load_sym<fn_Bambu_Open>(S("Bambu_Open"));
    p_start = load_sym<fn_Bambu_StartStream>(S("Bambu_StartStream"));
    p_count = load_sym<fn_Bambu_GetStreamCount>(S("Bambu_GetStreamCount"));
    p_info = load_sym<fn_Bambu_GetStreamInfo>(S("Bambu_GetStreamInfo"));
    p_read = load_sym<fn_Bambu_ReadSample>(S("Bambu_ReadSample"));
    p_close = load_sym<fn_Bambu_Close>(S("Bambu_Close"));
    p_destroy = load_sym<fn_Bambu_Destroy>(S("Bambu_Destroy"));
    p_init = load_sym<fn_Bambu_Init>(S("Bambu_Init"));
    p_deinit = load_sym<fn_Bambu_Deinit>(S("Bambu_Deinit"));
    p_lasterr = load_sym<fn_Bambu_GetLastErrorMsg>(S("Bambu_GetLastErrorMsg"));
    p_free_log = load_sym<fn_Bambu_FreeLogMsg>(S("Bambu_FreeLogMsg"));
    if (!p_create || !p_open || !p_start || !p_read || !p_close || !p_destroy) {
        if (err) *err = "BambuSource missing Bambu_* exports";
        return false;
    }
    if (p_init && !g_src_inited) {
        p_init();
        g_src_inited = true;
    }
    g_src = std::move(h);
    return true;
}

std::string build_lan_camera_url(const std::string& ip, const std::string& user,
                                 const std::string& passwd, const std::string& proto,
                                 const std::string& device, const std::string& net_ver,
                                 const std::string& dev_ver) {
    std::string url;
    std::string p = proto;
    for (auto& c : p) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (p == "rtsps")
        url = "bambu:///rtsps___" + user + ":" + passwd + "@" + ip + "/streaming/live/1?proto=rtsps";
    else if (p == "rtsp")
        url = "bambu:///rtsp___" + user + ":" + passwd + "@" + ip + "/streaming/live/1?proto=rtsp";
    else
        url = "bambu:///local/" + ip + ".?port=6000&user=" + user + "&passwd=" + passwd;
    if (!device.empty()) url += "&device=" + device;
    if (!net_ver.empty()) url += "&net_ver=" + net_ver;
    if (!dev_ver.empty()) url += "&dev_ver=" + dev_ver;
    return url;
}

std::string ota_from_json(const std::string& msg) {
    Json j;
    if (msg.empty() || !json_parse(msg, &j)) return {};
    const Json* info = j.find("info");
    const Json* mods = info ? info->find("module") : nullptr;
    if (!mods || mods->kind != Json::Array) return {};
    for (const auto& m : mods->arr) {
        const Json* name = m.find("name");
        const Json* ver = m.find("sw_ver");
        if (name && name->kind == Json::String && name->str == "ota" && ver && ver->kind == Json::String)
            return ver->str;
    }
    return {};
}

std::string resolve_ota_version(BambuBridge& bridge, const std::string& dev_id, const std::string& given) {
    if (!given.empty()) return given;
    std::string v = ota_from_json(bridge.last_printer_msg(dev_id));
    if (!v.empty()) return v;
    Json cmd = Json::object();
    Json info = Json::object();
    info.set("sequence_id", Json::s("0"));
    info.set("command", Json::s("get_version"));
    cmd.set("info", info);
    auto w = bridge.printer_command_wait(dev_id, json_dump(cmd), false, 8000, 0, 0);
    v = ota_from_json(w.msg);
    return v;
}

std::string resolve_camera_url(BambuBridge& bridge, const std::string& dev_id,
                               const std::string& protocols, const std::string& lan_ip,
                               const std::string& lan_user, const std::string& lan_passwd,
                               const std::string& lan_proto, const std::string& dev_ver,
                               std::string* err) {
    if (!lan_ip.empty()) {
        return build_lan_camera_url(lan_ip, lan_user.empty() ? "bblp" : lan_user,
                                    lan_passwd, lan_proto.empty() ? "local" : lan_proto,
                                    dev_id, bridge.version(), dev_ver);
    }
    {
        std::string e;
        int ready = bridge.ensure_cloud_ready(dev_id, 15000, &e);
        if (ready != 0) {
            if (err) *err = e.empty() ? ("ensure_cloud_ready failed: " + std::to_string(ready)) : e;
            return {};
        }
    }
    std::string ver = resolve_ota_version(bridge, dev_id, dev_ver);
    std::string proto = protocols.empty() ? "\"tutk\",\"agora\"" : protocols;
    std::string key = dev_id + "|" + ver + "|" + proto;
    std::string url;
    int r = bridge.get_camera_url(key, &url);
    if (r != 0 || url.empty() || url.rfind("bambu:///", 0) != 0) {
        std::string raw = url.empty() ? ("code=" + std::to_string(r)) : url;
        if (err) {
            *err = "get_camera_url failed: " + raw
                + " (dev_ver=" + (ver.empty() ? std::string("<empty>") : ver) + ")";
            if (raw.find("403") != std::string::npos)
                *err += " cloud camera 403: need OTA version, or tutk disabled, or try LAN ?ip=&password=";
        }
        return {};
    }
    url += "&device=" + dev_id;
    url += "&net_ver=" + bridge.version();
    if (!ver.empty()) url += "&dev_ver=" + ver;
    return url;
}

int stream_camera_mpegts(const std::string& bambu_url, const std::atomic<bool>& running,
                         ByteSink sink, std::string* err) {
    if (!camera_source_ready()) {
        if (err) *err = "BambuSource not loaded";
        return -1;
    }
    Bambu_Tunnel tnl = nullptr;
    if (p_create(&tnl, bambu_url.c_str()) != Bambu_success || !tnl) {
        if (err) *err = p_lasterr ? p_lasterr() : "Bambu_Create failed";
        return -2;
    }
    if (p_set_logger) p_set_logger(tnl, bambu_log, nullptr);
    int rv = p_open(tnl);
    if (rv != Bambu_success) {
        if (err) *err = std::string("Bambu_Open failed: ") + (p_lasterr ? p_lasterr() : std::to_string(rv));
        p_destroy(tnl);
        return rv;
    }
    int tries = 0;
    while ((rv = p_start(tnl, true)) == Bambu_would_block && running) {
        sleep_ms(100);
        if (++tries > 100) break;
    }
    if (rv != Bambu_success) {
        if (err) *err = "Bambu_StartStream failed: " + std::to_string(rv);
        p_close(tnl);
        p_destroy(tnl);
        return rv;
    }

    auto pat = make_pat();
    auto pmt = make_pmt();
    unsigned cc_video = 0;
    unsigned cc_pat = 0, cc_pmt = 0;
    int pkt_n = 0;
    unsigned long long pts0 = 0;
    bool sent_key = false;

    auto emit = [&](std::vector<unsigned char>& pkts) {
        if (pkts.empty()) return true;
        bool ok = sink(pkts.data(), pkts.size());
        pkts.clear();
        return ok;
    };

    std::vector<unsigned char> buf;
    buf.insert(buf.end(), pat.begin(), pat.end());
    buf.insert(buf.end(), pmt.begin(), pmt.end());
    if (!emit(buf)) { p_close(tnl); p_destroy(tnl); return 0; }

    while (running) {
        Bambu_Sample sample{};
        rv = p_read(tnl, &sample);
        if (rv == Bambu_would_block) { sleep_ms(20); continue; }
        if (rv == Bambu_stream_end) break;
        if (rv != Bambu_success) break;
        if (!sample.buffer || sample.size <= 0) continue;

        std::vector<unsigned char> annexb;
        if (looks_annexb(sample.buffer, (size_t)sample.size))
            annexb.assign(sample.buffer, sample.buffer + sample.size);
        else
            annexb = avcc_to_annexb(sample.buffer, (size_t)sample.size);
        if (annexb.empty()) continue;
        if (!nalu_is_aud(annexb.data(), annexb.size())) {
            static const unsigned char aud[] = {0, 0, 0, 1, 0x09, 0xf0};
            annexb.insert(annexb.begin(), aud, aud + sizeof(aud));
        }
        bool key = is_keyframe(annexb.data(), annexb.size()) || (sample.flags & 1);
        if (!sent_key && !key) continue;
        sent_key = true;

        unsigned long long pts90 = sample.decode_time / 100ULL;
        if (pts0 == 0) pts0 = pts90;
        if (pts90 < pts0) pts90 = pts0;
        auto pkts = pes_packets(annexb.data(), annexb.size(), pts90, key, cc_video);
        if (++pkt_n % 40 == 0) {
            auto p1 = make_pat();
            auto p2 = make_pmt();
            p1[3] = (unsigned char)(0x10 | (cc_pat++ & 0x0f));
            p2[3] = (unsigned char)(0x10 | (cc_pmt++ & 0x0f));
            buf.insert(buf.end(), p1.begin(), p1.end());
            buf.insert(buf.end(), p2.begin(), p2.end());
        }
        buf.insert(buf.end(), pkts.begin(), pkts.end());
        if (!emit(buf)) break;
    }

    p_close(tnl);
    p_destroy(tnl);
    return 0;
}

} // namespace bambu
