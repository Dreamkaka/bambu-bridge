#ifndef BAMBU_BRIDGE_MCP_STDIO_HPP
#define BAMBU_BRIDGE_MCP_STDIO_HPP

#include <cstdio>

namespace bambu {
class BambuBridge;

// Reserve the original stdout for protocol frames before loading any plugins.
std::FILE* open_mcp_output();
bool run_mcp_stdio(BambuBridge& bridge, std::FILE* output, bool enable_control);
}

#endif
