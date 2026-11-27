#ifndef BAMBU_BRIDGE_HTTP_WEB_API_HPP
#define BAMBU_BRIDGE_HTTP_WEB_API_HPP

#include "json.hpp"
#include <atomic>
#include <functional>
#include <string_view>

namespace bambu {

struct WebApiResponse {
    int status;
    Json body;
    std::string extra_headers;
};

// Download callbacks are invoked only on validated chunks. A successful
// download (including an empty file) invokes the sink at least once.
using WebFileSink = std::function<bool(std::string_view name, std::string_view bytes)>;
WebApiResponse dispatch_web_api(BambuBridge& bridge, std::string_view method, std::string_view path,
                                std::string_view query, std::string_view content_type, const std::string& body,
                                const std::atomic<bool>& running, const WebFileSink& file_sink);
bool is_web_device_id(std::string_view value);
WebApiResponse web_api_error(int status, std::string_view code);

} // namespace bambu

#endif
