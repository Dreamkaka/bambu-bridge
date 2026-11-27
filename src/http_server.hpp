#ifndef BAMBU_BRIDGE_HTTP_SERVER_HPP
#define BAMBU_BRIDGE_HTTP_SERVER_HPP

// ============================================================================
// http_server.hpp
//
// Minimal cross-platform HTTP/REST server (no third-party dependency).
//
// Endpoints:
//   POST /rpc          body: {"method":"...","params":{...}}
//                      -> 200 {"ok":true,"result":...}
//                      -> 200 {"ok":false,"error":"..."}
//   GET  /health       -> 200 {"ok":true,"version":"...","loaded":true}
//   GET  /events       -> Server-Sent Events stream of async plugin events.
//   POST /api/<method> body is params; waits for printer payload on control cmds.
//   GET  /camera.ts    MPEG-TS remux of BambuSource H264 (VLC/ffmpeg/mpegts.js).
//   GET  /camera.jpg   Single JPEG frame from BambuSource.
//
// Implemented with raw sockets so it builds with g++/MinGW/MSVC; the only
// platform difference is Winsock init on Windows (handled in main via
// init_sockets()).
// ============================================================================

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include "bridge.hpp"

namespace bambu {

// Start the HTTP server on the given port and run the accept loop on a new
// thread.  Returns false if the socket could not be bound.
// On Linux/macOS call init_sockets() (no-op) before this.
int http_listen_port();
std::string build_login_url(const std::string& host, int port, const std::string& locale = "zh-CN");
bool start_http_server(BambuBridge& bridge, int port, bool mcp_control, bool loopback_only);

// Stop the server and join its thread.
void stop_http_server();

// On Windows this initialises Winsock. On other platforms it is a no-op.
bool init_sockets();

// Platform listen socket descriptor type.
#if defined(_WIN32)
typedef unsigned long long sock_t;
#else
typedef int sock_t;
#endif

} // namespace bambu

#endif // BAMBU_BRIDGE_HTTP_SERVER_HPP
