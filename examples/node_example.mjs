// ============================================================================
// node_example.mjs - talk to bambu-bridge over HTTP/REST JSON (Node >= 18).
//
//   ./build/bambu-bridge --plugin-dir ./linux_02.05.02.58 --country CN \
//                        --http 8080 --no-stdio
//   node examples/node_example.mjs
// ============================================================================
const BASE = "http://127.0.0.1:8080";

async function rpc(method, params = {}) {
  const r = await fetch(`${BASE}/rpc`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ method, params }),
  });
  return r.json();
}

const serial = "22E8BJ630901079";

(async () => {
  console.log("version:", await rpc("version"));
  console.log("create_agent:", await rpc("create_agent", { log_dir: "/tmp" }));
  console.log("set_country_code:", await rpc("set_country_code", { country_code: "CN" }));
  console.log("connect_server:", await rpc("connect_server"));
  console.log("add_subscribe:", await rpc("add_subscribe", { dev_list: [serial] }));

  const push = await rpc("send_message_to_printer", {
    dev_id: serial,
    json_str: JSON.stringify({ pushing: { sequence_id: "0", command: "pushall" } }),
    qos: 0,
    flag: 0,
  });
  console.log("pushall:", push);

  const pause = await rpc("send_message_to_printer", {
    dev_id: serial,
    json_str: JSON.stringify({ print: { sequence_id: "0", command: "pause" } }),
    qos: 0,
    flag: 0,
  });
  console.log("pause:", pause);
})().catch((e) => { console.error("ERROR:", e); process.exit(1); });
