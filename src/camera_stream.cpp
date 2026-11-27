#include "camera_stream.hpp"
#include "bambu_tunnel.hpp"
#include "bridge.hpp"
#include "json.hpp"
#include "printer_files.hpp"

#include <cstring>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <algorithm>
#include <csetjmp>
#include <cstdio>

#include <jpeglib.h>
#include <wels/codec_api.h>

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
fn_Bambu_StartStreamEx     p_start_ex = nullptr;
fn_Bambu_GetStreamCount    p_count = nullptr;
fn_Bambu_GetStreamInfo     p_info = nullptr;
fn_Bambu_ReadSample        p_read = nullptr;
fn_Bambu_SendMessage       p_send = nullptr;
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

using Clock = std::chrono::steady_clock;
constexpr size_t max_snapshot_size = 16 * 1024 * 1024;

bool is_jpeg(const unsigned char* p, size_t n) {
    return n >= 4 && p[0] == 0xff && p[1] == 0xd8 && p[n - 2] == 0xff && p[n - 1] == 0xd9;
}

struct SnapshotTunnel {
    Bambu_Tunnel handle = nullptr;
    bool opened = false;
    ~SnapshotTunnel() {
        if (opened) p_close(handle);
        if (handle) p_destroy(handle);
    }
};

// Hold the most recent in-band parameter sets if the IDR sample arrives separately.
bool collect_h264_frame(const unsigned char* data, size_t size, bool& avcc, std::string& sps,
                        std::string& pps, std::string& frame) {
    const bool annexb = !avcc && looks_annexb(data, size);
    auto next_nal = [&](size_t from) {
        while (from + 3 < size && !(data[from] == 0 && data[from + 1] == 0 &&
               (data[from + 2] == 1 || (data[from + 2] == 0 && data[from + 3] == 1)))) ++from;
        return from + 3 < size ? from : size;
    };
    auto nal_size = [&](size_t offset) {
        return (size_t(data[offset]) << 24) | (size_t(data[offset + 1]) << 16) |
               (size_t(data[offset + 2]) << 8) | size_t(data[offset + 3]);
    };
    constexpr char start_code[] = "\0\0\0\1";
    bool idr = false, inline_sps = false, inline_pps = false;
    for (size_t start = 0; start < size;) {
        size_t payload, end;
        if (annexb) {
            payload = start + (data[start + 2] == 1 ? 3 : 4);
            if (payload == size) break;
            end = next_nal(payload + 1);
        } else {
            if (size - start < 4) return false;
            size_t n = nal_size(start);
            payload = start + 4;
            if (!n || n > size - payload) return false;
            end = payload + n;
        }
        int type = data[payload] & 0x1f;
        if ((type == 7 || type == 8) && end - payload <= 64 * 1024) {
            auto& parameter_set = type == 7 ? sps : pps;
            parameter_set.assign(start_code, 4).append(reinterpret_cast<const char*>(data + payload), end - payload);
            (type == 7 ? inline_sps : inline_pps) = true;
        }
        if (type == 5) idr = true;
        start = end;
    }
    if (!annexb && (inline_sps || inline_pps)) avcc = true;
    if (!idr || sps.empty() || pps.empty()) return false;
    size_t prefix = inline_sps && inline_pps ? 0 : sps.size() + pps.size();
    frame.reserve(prefix + size);
    if (prefix) frame.append(sps).append(pps);
    frame.append(reinterpret_cast<const char*>(data), size);
    if (!annexb) {
        for (size_t offset = 0; offset < size; offset += 4 + nal_size(offset))
            std::memcpy(frame.data() + prefix + offset, start_code, 4);
    }
    return true;
}

struct H264Decoder {
    ISVCDecoder* handle = nullptr;
    ~H264Decoder() {
        if (handle) {
            handle->Uninitialize();
            WelsDestroyDecoder(handle);
        }
    }
};

struct JpegError {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
};

struct JpegDestination {
    jpeg_destination_mgr manager{};
    std::string* output;
    JOCTET buffer[4096];
    bool failed = false;

    explicit JpegDestination(std::string& jpeg) : output(&jpeg) {}

    bool append(size_t n) noexcept {
        if (n > max_snapshot_size - output->size()) return false;
        try {
            output->append(reinterpret_cast<const char*>(buffer), n);
            return true;
        } catch (...) {
            return false;
        }
    }

    void reset() {
        manager.next_output_byte = buffer;
        manager.free_in_buffer = sizeof(buffer);
    }
};

struct JpegEncoder {
    jpeg_compress_struct codec{};
    JpegError error{};
    JpegDestination destination;
    explicit JpegEncoder(std::string& jpeg) : destination(jpeg) {}
    ~JpegEncoder() { jpeg_destroy_compress(&codec); }
};

// Keep libjpeg's longjmp inside this C-style call. The caller owns all resources.
bool encode_i420(JpegEncoder& encoder, JSAMPLE* edge_rows,
                 unsigned char* const planes[3], const SSysMEMBuffer& image) {
    auto& c = encoder.codec;
    auto& dest = encoder.destination;
    dest.manager.init_destination = [](j_compress_ptr ctx) {
        reinterpret_cast<JpegDestination*>(ctx->dest)->reset();
    };
    dest.manager.empty_output_buffer = [](j_compress_ptr ctx) -> boolean {
        auto& d = *reinterpret_cast<JpegDestination*>(ctx->dest);
        if (!d.append(sizeof(d.buffer))) return FALSE;
        d.reset();
        return TRUE;
    };
    dest.manager.term_destination = [](j_compress_ptr ctx) {
        auto& d = *reinterpret_cast<JpegDestination*>(ctx->dest);
        d.failed = !d.append(sizeof(d.buffer) - d.manager.free_in_buffer);
    };
    c.err = jpeg_std_error(&encoder.error.manager);
    c.client_data = &encoder.error;
    c.err->error_exit = [](j_common_ptr ctx) {
        std::longjmp(static_cast<JpegError*>(ctx->client_data)->jump, 1);
    };
    if (setjmp(encoder.error.jump)) return false;
    jpeg_create_compress(&c);
    c.dest = &dest.manager;
    c.image_width = image.iWidth;
    c.image_height = image.iHeight;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 85, TRUE);
    c.raw_data_in = TRUE;
    jpeg_start_compress(&c, TRUE);
    // Pass decoded planes directly. Only copy rows when a cropped width needs padding.
    while (c.next_scanline < c.image_height) {
        JSAMPROW rows[3][16];
        JSAMPARRAY batch[3] = {rows[0], rows[1], rows[2]};
        auto* edge = edge_rows;
        for (int p = 0; p < 3; ++p) {
            const int scale = p == 0 ? 1 : 2;
            const int width = (image.iWidth + scale - 1) / scale;
            const int height = (image.iHeight + scale - 1) / scale;
            const int padded = c.comp_info[p].width_in_blocks * DCTSIZE;
            for (int i = 0; i < 16 / scale; ++i) {
                int y = std::min(static_cast<int>(c.next_scanline) / scale + i, height - 1);
                auto* src = planes[p] + y * image.iStride[p != 0];
                rows[p][i] = src;
                if (padded > width) {
                    std::memcpy(edge, src, width);
                    std::fill(edge + width, edge + padded, src[width - 1]);
                    rows[p][i] = edge;
                    edge += padded;
                }
            }
        }
        if (jpeg_write_raw_data(&c, batch, 16) != 16) return false;
    }
    jpeg_finish_compress(&c);
    return !dest.failed;
}

SnapshotStatus convert_h264_snapshot(const std::string& frame, std::string& jpeg, std::string& err) {
    H264Decoder decoder;
    if (WelsCreateDecoder(&decoder.handle) != 0 || !decoder.handle) {
        err = "could not create H.264 decoder";
        return SnapshotStatus::failed;
    }
    SDecodingParam options{};
    options.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    options.eEcActiveIdc = ERROR_CON_DISABLE;
    if (decoder.handle->Initialize(&options) != 0) {
        err = "could not initialize H.264 decoder";
        return SnapshotStatus::failed;
    }
    unsigned char* planes[3]{};
    SBufferInfo decoded{};
    auto status = decoder.handle->DecodeFrameNoDelay(reinterpret_cast<const unsigned char*>(frame.data()),
                                                    static_cast<int>(frame.size()), planes, &decoded);
    // Main/High streams can queue the decoded IDR for display reordering.
    if (status == dsErrorFree && decoded.iBufferStatus == 0)
        status = decoder.handle->FlushFrame(planes, &decoded);
    const auto& image = decoded.UsrData.sSystemBuffer;
    if (status != dsErrorFree || decoded.iBufferStatus != 1 || image.iFormat != videoFormatI420 ||
        image.iWidth <= 0 || image.iHeight <= 0 || image.iWidth > 8192 || image.iHeight > 8192 ||
        static_cast<size_t>(image.iWidth) * image.iHeight > 16 * 1024 * 1024 ||
        image.iStride[0] < image.iWidth || image.iStride[1] < (image.iWidth + 1) / 2 ||
        !planes[0] || !planes[1] || !planes[2]) {
        err = "could not decode H.264 camera frame";
        return SnapshotStatus::failed;
    }
    std::vector<JSAMPLE> edge_rows;
    if (image.iWidth % 16) edge_rows.resize(((image.iWidth + 15) & ~15) * 24);
    jpeg.clear();
    JpegEncoder encoder(jpeg);
    if (!encode_i420(encoder, edge_rows.data(), planes, image)) {
        jpeg.clear();
        err = "could not encode camera frame as JPEG";
        return SnapshotStatus::failed;
    }
    return SnapshotStatus::ok;
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
    p_start_ex = load_sym<fn_Bambu_StartStreamEx>(S("Bambu_StartStreamEx"));
    p_count = load_sym<fn_Bambu_GetStreamCount>(S("Bambu_GetStreamCount"));
    p_info = load_sym<fn_Bambu_GetStreamInfo>(S("Bambu_GetStreamInfo"));
    p_read = load_sym<fn_Bambu_ReadSample>(S("Bambu_ReadSample"));
    p_send = load_sym<fn_Bambu_SendMessage>(S("Bambu_SendMessage"));
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

const Json* find_ipcam(const Json& root) {
    const Json* p = root.find("print");
    if (p) {
        if (const Json* c = p->find("ipcam")) return c;
    }
    return root.find("ipcam");
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

void collect_camera_protocols(const std::string& msg, const std::string& requested, std::vector<std::string>& out) {
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (const auto& x : out) if (x == s) return;
        out.push_back(s);
    };
    if (!requested.empty()) add(requested);
    Json j;
    std::string tutk, agora, brtc, remote;
    if (!msg.empty() && json_parse(msg, &j)) {
        const Json* ipcam = find_ipcam(j);
        if (ipcam) {
            if (const Json* t = ipcam->find("tutk_server"))
                if (t->kind == Json::String) tutk = t->str;
            if (const Json* a = ipcam->find("agora_service"))
                if (a->kind == Json::String) agora = a->str;
            if (const Json* b = ipcam->find("brtc_service"))
                if (b->kind == Json::String) brtc = b->str;
            const Json* lv = ipcam->find("liveview");
            if (lv) {
                if (const Json* r = lv->find("remote"))
                    if (r->kind == Json::String) remote = r->str;
            }
        }
    }
    if (brtc == "enable" || (tutk == "disable" && agora == "disable"))
        add("brtc");
    if (agora != "disable" && remote == "agora") add("\"agora\"");
    if (tutk != "disable" && remote == "tutk") add("\"tutk\"");
    if (tutk != "disable" && agora != "disable") add("\"tutk\",\"agora\"");
    if (agora != "disable") add("\"agora\"");
    if (tutk != "disable") add("\"tutk\"");
    add("brtc");
    add("\"tutk\",\"agora\"");
}

std::string resolve_ota_version(BambuBridge& bridge, const std::string& dev_id, const std::string& given) {
    if (!given.empty()) return given;
    std::string v = ota_from_json(bridge.last_printer_msg(dev_id));
    if (!v.empty()) return v;
    Json cmd = Json::object();
    Json info = Json::object();
    info.set("sequence_id", Json::s(BambuBridge::next_command_sequence()));
    info.set("command", Json::s("get_version"));
    cmd.set("info", info);
    auto w = bridge.printer_command(dev_id, json_dump(cmd), false);
    v = ota_from_json(w.msg);
    return v;
}

std::string resolve_storage_cloud_url(BambuBridge& bridge, const std::string& device) {
    if (bridge.ensure_cloud_ready(device, 15000) != 0) return {};
    const auto firmware = resolve_ota_version(bridge, device, {});
    std::string url;
    if (bridge.get_camera_url(device + "|" + firmware + "|\"tutk\"", &url) != 0 ||
        url.rfind("bambu:///tutk?", 0) != 0) return {};
    url += "&device=" + device + "&net_ver=" + bridge.client_version();
    if (!firmware.empty()) url += "&dev_ver=" + firmware;
    return url;
}

namespace {
constexpr int ctrl_type = 0x3001;
using ControlClock = std::chrono::steady_clock;
constexpr auto ctrl_idle_timeout = std::chrono::seconds(30);
}

FileControl::~FileControl() {
    if (tunnel_) {
        p_close(tunnel_);
        p_destroy(tunnel_);
    }
}

FileControlStatus FileControl::open(const std::string& plugin_dir, const std::string& url,
                                    const std::atomic<bool>& running) {
    if (!load_camera_source(plugin_dir, nullptr) || !p_send) return FileControlStatus::unavailable;
    if (p_create(&tunnel_, url.c_str()) != Bambu_success || !tunnel_) return FileControlStatus::connect_failed;
    if (p_set_logger && p_free_log) p_set_logger(tunnel_, bambu_log, nullptr);
    if (p_open(tunnel_) != Bambu_success) return FileControlStatus::connect_failed;

    const auto deadline = ControlClock::now() + ctrl_idle_timeout;
    while (running) {
        const int rc = p_start_ex ? p_start_ex(tunnel_, ctrl_type) : p_start(tunnel_, false);
        if (rc == Bambu_success) return FileControlStatus::ok;
        if (rc != Bambu_would_block) return FileControlStatus::connect_failed;
        if (ControlClock::now() >= deadline) return FileControlStatus::timeout;
        sleep_ms(50);
    }
    return FileControlStatus::stopped;
}

FileControlStatus FileControl::request(int command, Json req, const std::atomic<bool>& running, const Reply& on_reply) {
    if (!running) return FileControlStatus::stopped;
    Json envelope = Json::object();
    envelope.set("cmdtype", Json::n(command));
    envelope.set("sequence", Json::n(++sequence_));
    envelope.set("req", std::move(req));
    const auto message = json_dump(envelope);
    if (p_send(tunnel_, ctrl_type, message.data(), static_cast<int>(message.size())) != Bambu_success)
        return FileControlStatus::protocol_error;

    auto deadline = ControlClock::now() + ctrl_idle_timeout;
    while (running) {
        if (ControlClock::now() >= deadline) return FileControlStatus::timeout;
        Bambu_Sample sample{};
        const int rc = p_read(tunnel_, &sample);
        if (rc == Bambu_would_block) { sleep_ms(10); continue; }
        if (rc != Bambu_success || !sample.buffer || sample.size <= 0 || sample.size > 8 * 1024 * 1024)
            return FileControlStatus::protocol_error;
        const std::string_view frame(reinterpret_cast<const char*>(sample.buffer), static_cast<size_t>(sample.size));
        const auto split = frame.find("\n\n");
        Json response;
        if (!json_parse(std::string(frame.substr(0, split)), &response, true) || response.kind != Json::Object)
            return FileControlStatus::protocol_error;
        const auto* seq = response.find("sequence");
        if (!seq || seq->kind != Json::Number || seq->num != sequence_) continue;
        const auto* cmd = response.find("cmdtype");
        if (cmd && (cmd->kind != Json::Number || cmd->num != command)) continue;
        const auto* result = response.find("result");
        if (!result || result->kind != Json::Number || result->num < -2147483648.0 || result->num > 2147483647.0)
            return FileControlStatus::protocol_error;
        const auto code = static_cast<int>(result->num);
        if (code != result->num) return FileControlStatus::protocol_error;
        const auto* reply = response.find("reply");
        const auto data = split == std::string_view::npos ? std::string_view{} : frame.substr(split + 2);
        if (!on_reply(code, reply ? *reply : response, data)) return FileControlStatus::stopped;
        if (code != 1) return FileControlStatus::ok;
        deadline = ControlClock::now() + ctrl_idle_timeout;
    }
    return FileControlStatus::stopped;
}

std::string resolve_camera_url(BambuBridge& bridge, const std::string& dev_id,
                               const std::string& protocols, const std::string& lan_ip,
                               const std::string& lan_user, const std::string& lan_passwd,
                               const std::string& lan_proto, const std::string& dev_ver,
                               std::string* err, std::string* kind) {
    if (kind) kind->clear();
    if (!lan_ip.empty()) {
        if (kind) *kind = "lan";
        return build_lan_camera_url(lan_ip, lan_user.empty() ? "bblp" : lan_user,
                                    lan_passwd, lan_proto.empty() ? "local" : lan_proto,
                                     dev_id, bridge.client_version(), dev_ver);
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
    std::vector<std::string> tries;
    collect_camera_protocols(bridge.last_printer_msg(dev_id), protocols, tries);
    std::string url, last_raw, last_key;
    auto finish_bambu = [&](std::string u, const char* knd) {
        u += "&device=" + dev_id;
        u += "&net_ver=" + bridge.client_version();
        if (!ver.empty()) u += "&dev_ver=" + ver;
        if (kind) *kind = knd;
        return u;
    };
    for (const auto& proto : tries) {
        url.clear();
        int r = 0;
        if (proto == "brtc") {
            last_key = "golive:" + dev_id;
            r = bridge.get_camera_url_for_golive(dev_id, "", &url);
        } else {
            last_key = dev_id + "|" + ver + "|" + proto;
            r = bridge.get_camera_url(last_key, &url);
        }
        auto ok_bambu = !url.empty() && url.rfind("bambu:///", 0) == 0;
        auto looks_err = url.find("[") != std::string::npos && url.find("]") != std::string::npos
                         && url.find("://") == std::string::npos;
        if (r == 0 && ok_bambu)
            return finish_bambu(url, proto == "brtc" ? "brtc" : "cloud");
        if (r == 0 && proto == "brtc" && !url.empty() && !looks_err && url.find("://") != std::string::npos) {
            if (kind) *kind = "brtc";
            return url;
        }
        last_raw = url.empty() ? ("code=" + std::to_string(r)) : url;
    }
    if (err) {
        *err = "get_camera_url failed: " + last_raw
            + " (key=" + last_key + ")";
        if (last_raw.find("403") != std::string::npos)
            *err += " TUTK/Agora 403 and BRTC golive also failed. "
                    "This printer has tutk/agora disabled; try LAN "
                    "/camera.ts?dev_id=&ip=&password=&proto=local";
    }
    return {};
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
    if (rv != Bambu_success && p_start_ex) {
        tries = 0;
        while ((rv = p_start_ex(tnl, 1)) == Bambu_would_block && running) {
            sleep_ms(100);
            if (++tries > 100) break;
        }
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

SnapshotStatus capture_camera_jpeg(const std::string& bambu_url, std::string& jpeg, std::string& err) {
    if (!camera_source_ready()) {
        err = "BambuSource not loaded";
        return SnapshotStatus::unavailable;
    }
    std::string frame;
    {
        auto deadline = Clock::now() + std::chrono::seconds(12);
        SnapshotTunnel tunnel;
        if (p_create(&tunnel.handle, bambu_url.c_str()) != Bambu_success || !tunnel.handle) {
            err = "Bambu_Create failed";
            return SnapshotStatus::failed;
        }
        if (p_set_logger) p_set_logger(tunnel.handle, bambu_log, nullptr);
        if (p_open(tunnel.handle) != Bambu_success) {
            err = "Bambu_Open failed";
            return SnapshotStatus::failed;
        }
        tunnel.opened = true;
        auto start = [&](auto call) {
            while (Clock::now() < deadline) {
                int rc = call();
                if (rc != Bambu_would_block) return rc;
                sleep_ms(100);
            }
            return static_cast<int>(Bambu_would_block);
        };
        int rc = start([&] { return p_start(tunnel.handle, true); });
        if (rc != Bambu_success && rc != Bambu_would_block && p_start_ex)
            rc = start([&] { return p_start_ex(tunnel.handle, 1); });
        if (rc != Bambu_success) {
            err = rc == Bambu_would_block ? "camera stream startup timed out"
                                       : "Bambu_StartStream failed: " + std::to_string(rc);
            return rc == Bambu_would_block ? SnapshotStatus::timeout : SnapshotStatus::failed;
        }
        int video_track = -1;
        if (p_info) {
            int count = p_count ? p_count(tunnel.handle) : 1;
            for (int i = 0; i < count; ++i) {
                Bambu_StreamInfo info{};
                if (p_info(tunnel.handle, i, &info) == Bambu_success && info.type == VIDE) {
                    video_track = i;
                    break;
                }
            }
        }
        bool avcc = false;
        std::string sps, pps;
        while (Clock::now() < deadline) {
            Bambu_Sample sample{};
            rc = p_read(tunnel.handle, &sample);
            if (rc == Bambu_would_block) {
                sleep_ms(20);
                continue;
            }
            if (rc != Bambu_success) {
                err = rc == Bambu_stream_end ? "camera stream ended before a frame was captured"
                                            : "Bambu_ReadSample failed: " + std::to_string(rc);
                return SnapshotStatus::failed;
            }
            if ((video_track >= 0 && sample.itrack != video_track) || !sample.buffer ||
                sample.size <= 0 || static_cast<size_t>(sample.size) > max_snapshot_size) continue;
            if (is_jpeg(sample.buffer, sample.size)) {
                jpeg.assign(reinterpret_cast<const char*>(sample.buffer), sample.size);
                return SnapshotStatus::ok;
            }
            if (collect_h264_frame(sample.buffer, sample.size, avcc, sps, pps, frame)) break;
        }
        if (frame.empty()) {
            err = "timed out waiting for a camera frame";
            return SnapshotStatus::timeout;
        }
    } // Release the camera before running the decoder.
    return convert_h264_snapshot(frame, jpeg, err);
}

} // namespace bambu
