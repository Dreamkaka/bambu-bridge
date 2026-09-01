#ifndef BAMBU_BRIDGE_STDIO_TRANSPORT_HPP
#define BAMBU_BRIDGE_STDIO_TRANSPORT_HPP

// ============================================================================
// stdio_transport.hpp
//
// JSON-RPC over stdin/stdout. Protocol:
//   - Requests come in as one JSON object per line on stdin:
//       {"method":"...","params":{...}}
//   - Respond with one JSON object per line on stdout:
//       {"ok":true,"result":...}
//       {"ok":false,"error":"..."}
//   - Async events are pushed as:
//       {"event":"...","payload":{...}}
//     on the same stdout stream. Clients should distinguish them by the
//     presence of an "event" key (vs "ok").
// ============================================================================

#include <string>
#include "bridge.hpp"

namespace bambu {

// Run the stdio loop forever. `bridge` must be loaded. Returns only on error.
void run_stdio(BambuBridge& bridge);

} // namespace bambu

#endif // BAMBU_BRIDGE_STDIO_TRANSPORT_HPP
