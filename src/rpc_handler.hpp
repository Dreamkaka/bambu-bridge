#ifndef BAMBU_BRIDGE_RPC_HANDLER_HPP
#define BAMBU_BRIDGE_RPC_HANDLER_HPP

// ============================================================================
// rpc_handler.hpp
//
// Shared request dispatcher used by BOTH transport layers (stdio JSON-RPC and
// HTTP/REST).  Given a JSON request object of the form:
//
//     { "method": "...", "params": {...} | [...] }
//
// it routes the call to the BambuBridge and returns a response JSON object:
//
//     { "ok": true|false, "result": {...} }
//     { "ok": false, "error": "..." }
//
// Transports only need to: parse the request into Json, call handle_request(),
// then serialize the returned Json (or send the result / error string).
// ============================================================================

#include <string>
#include "bridge.hpp"

namespace bambu {

// Route one request. `bridge` must already be loaded (load_plugin succeeded).
// Returns a response JSON object (see above).
// Optional provenance for the Web API; does not affect the legacy JSON response.
Json handle_request(BambuBridge& bridge, const Json& request, bool* plugin_called = nullptr);

} // namespace bambu

#endif // BAMBU_BRIDGE_RPC_HANDLER_HPP
