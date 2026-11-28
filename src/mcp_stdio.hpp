#ifndef BAMBU_BRIDGE_MCP_STDIO_HPP
#define BAMBU_BRIDGE_MCP_STDIO_HPP

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>

namespace bambu {
class BambuBridge;

// Reserve the original stdout for protocol frames before loading any plugins.
std::FILE* open_mcp_output();
bool run_mcp_stdio(BambuBridge& bridge, std::FILE* output, bool enable_control);

bool supports_mcp_http_version(std::string_view version);
// Empty reply means HTTP 202; this transport does not issue session IDs.
std::string dispatch_mcp_http(BambuBridge& bridge, const std::string& body, bool enable_control,
                              const std::atomic<bool>& running);
}

#endif
