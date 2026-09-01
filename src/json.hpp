#ifndef BAMBU_BRIDGE_JSON_HPP
#define BAMBU_BRIDGE_JSON_HPP

// ============================================================================
// json.hpp
//
// Minimal self-contained JSON serializer/parser for the bridge's Json value.
// No third-party dependency so the bridge builds anywhere with just the C++
// standard library.
//
// Supported: object, array, string, number, bool, null.
// ============================================================================

#include <string>
#include "bridge.hpp"

namespace bambu {

// Serialize a Json value to a compact JSON string.
std::string json_dump(const Json& j);

// Parse a JSON string into `out`.  Returns true on success, false on syntax
// error.  Leading/trailing whitespace is tolerated.
bool json_parse(const std::string& text, Json* out);

} // namespace bambu

#endif // BAMBU_BRIDGE_JSON_HPP
