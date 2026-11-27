#ifndef BAMBU_BRIDGE_CAMERA_STREAM_HPP
#define BAMBU_BRIDGE_CAMERA_STREAM_HPP

#include <string>
#include <functional>
#include <atomic>

#include "plugin_loader.hpp"

namespace bambu {

class BambuBridge;

struct CameraSession {
    std::string url;
    std::string error;
    int width = 0;
    int height = 0;
    int fps = 0;
    int video_sub_type = 0;
    bool ok = false;
};

bool camera_source_ready();
bool load_camera_source(const std::string& plugin_dir, std::string* err);

std::string build_lan_camera_url(const std::string& ip, const std::string& user,
                                 const std::string& passwd, const std::string& proto,
                                 const std::string& device, const std::string& net_ver,
                                 const std::string& dev_ver);

std::string resolve_camera_url(BambuBridge& bridge, const std::string& dev_id,
                               const std::string& protocols, const std::string& lan_ip,
                               const std::string& lan_user, const std::string& lan_passwd,
                               const std::string& lan_proto, const std::string& dev_ver,
                               std::string* err, std::string* kind = nullptr);

using ByteSink = std::function<bool(const unsigned char* data, size_t n)>;

int stream_camera_mpegts(const std::string& bambu_url, const std::atomic<bool>& running,
                         ByteSink sink, std::string* err);

enum class SnapshotStatus { ok, failed, unavailable, timeout };
SnapshotStatus capture_camera_jpeg(const std::string& bambu_url, std::string& jpeg, std::string& err);

} // namespace bambu

#endif
