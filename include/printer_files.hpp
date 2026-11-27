#ifndef BAMBU_BRIDGE_PRINTER_FILES_HPP
#define BAMBU_BRIDGE_PRINTER_FILES_HPP

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace bambu {

class BambuBridge;
struct Json;

enum class FileControlStatus { ok, unavailable, connect_failed, timeout, protocol_error, stopped };

// Shares BambuSource initialization with the camera, but starts a CTRL stream.
class FileControl {
public:
    ~FileControl();
    FileControl() = default;
    FileControl(const FileControl&) = delete;
    FileControl& operator=(const FileControl&) = delete;

    FileControlStatus open(const std::string& plugin_dir, const std::string& url,
                           const std::atomic<bool>& running);
    using Reply = std::function<bool(int result, const Json& fields, std::string_view bytes)>;
    // Callback views are borrowed only until it returns.
    FileControlStatus request(int command, Json req, const std::atomic<bool>& running, const Reply& on_reply);

private:
    void* tunnel_ = nullptr;
    int sequence_ = 0;
};

std::string resolve_storage_cloud_url(BambuBridge& bridge, const std::string& device);

} // namespace bambu

#endif
