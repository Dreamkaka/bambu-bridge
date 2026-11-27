#ifndef BAMBU_BRIDGE_HTTP_WEBSOCKET_HPP
#define BAMBU_BRIDGE_HTTP_WEBSOCKET_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bambu {

std::optional<std::string> websocket_accept(std::string_view key);
std::string websocket_frame(uint8_t opcode, std::string_view payload);
struct WsFrame { uint8_t opcode; std::string payload; };
enum class WsRead { incomplete, frame, protocol_error, unsupported_data, invalid_utf8 };
WsRead websocket_consume(std::string& pending, WsFrame& frame);
bool websocket_utf8_valid(std::string_view text);

} // namespace bambu

#endif
