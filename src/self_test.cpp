// ============================================================================
// self_test.cpp — a small, plugin-independent test harness.
//
// This exercises the parts of the bridge that do NOT need the closed-source
// Bambu networking plugin:
//   1. Json dump/parse round-trip
//   2. handle_request() routing (unknown method, params handling)
//   3. BambuBridge with no plugin loaded (safe calls return errors/empties)
//
// It is meant to be run by CI (GitHub Actions) on a fresh Linux runner to prove
// the bridge compiles and its plumbing works.  It does NOT call the plugin.
//
// Exit code 0 = all checks pass.
// ============================================================================
#include "bridge.hpp"
#include "json.hpp"
#include "rpc_handler.hpp"

#include <cstdio>
#include <string>
#include <cstring>

using namespace bambu;

static int g_fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_fails; } } while (0)

static void test_json_roundtrip() {
    Json obj = Json::object();
    obj.set("method", Json::s("version"));
    Json params = Json::object();
    params.set("qos", Json::n(0));
    params.set("flag", Json::n(1));
    params.set("enabled", Json::boolean(true));
    Json arr = Json::array();
    arr.arr.push_back(Json::s("a"));
    arr.arr.push_back(Json::n(3.5));
    params.set("arr", arr);
    obj.set("params", params);

    std::string dumped = json_dump(obj);
    CHECK(!dumped.empty(), "json_dump produced non-empty");
    CHECK(dumped.find("\"method\"") != std::string::npos, "dump has method key");

    Json back;
    CHECK(json_parse(dumped, &back), "json_parse succeeds on valid input");
    const Json* m = back.find("method");
    CHECK(m && m->kind == Json::String && m->str == "version", "round-trip method");
    const Json* p = back.find("params");
    CHECK(p && p->kind == Json::Object, "round-trip params object");
    const Json* qos = p ? p->find("qos") : nullptr;
    CHECK(qos && qos->kind == Json::Number && qos->num == 0.0, "round-trip qos number");
    const Json* en = p ? p->find("enabled") : nullptr;
    CHECK(en && en->kind == Json::Bool && en->b == true, "round-trip bool");
    const Json* ar = p ? p->find("arr") : nullptr;
    CHECK(ar && ar->kind == Json::Array && ar->arr.size() == 2, "round-trip array size");

    // invalid input -> false
    Json bad;
    CHECK(!json_parse("{ not json", &bad), "json_parse rejects bad input");

    // serialize a response shape
    Json resp = Json::object();
    resp.set("ok", Json::boolean(false));
    resp.set("error", Json::s("boom"));
    std::string rs = json_dump(resp);
    CHECK(rs.find("\"ok\":false") != std::string::npos, "response has ok:false");
}

static void test_rpc_unknown_method() {
    BambuBridge bridge;   // NOT loaded
    Json req = Json::object();
    req.set("method", Json::s("totally_unknown"));
    Json params = Json::object();
    req.set("params", params);
    Json resp = handle_request(bridge, req);
    const Json* ok = resp.find("ok");
    CHECK(ok && ok->kind == Json::Bool && ok->b == false, "unknown method -> ok:false");
    const Json* err = resp.find("error");
    CHECK(err && err->kind == Json::String && err->str.find("unknown method") != std::string::npos,
          "unknown method error message");
}

static void test_rpc_missing_method() {
    BambuBridge bridge;
    Json req = Json::object();
    req.set("params", Json::object());
    Json resp = handle_request(bridge, req);
    const Json* ok = resp.find("ok");
    CHECK(ok && ok->kind == Json::Bool && ok->b == false, "missing method -> ok:false");
}

static void test_rpc_wrapped_response() {
    BambuBridge bridge;
    Json req = Json::object();
    req.set("method", Json::s("version"));
    req.set("params", Json::object());
    Json resp = handle_request(bridge, req);
    const Json* ok = resp.find("ok");
    CHECK(ok && ok->kind == Json::Bool && ok->b == true, "version -> ok:true");
    const Json* result = resp.find("result");
    CHECK(result && result->kind == Json::String && result->str.empty(), "unloaded version result is empty string");

    Json login = Json::object();
    login.set("method", Json::s("is_user_login"));
    Json lresp = handle_request(bridge, login);
    const Json* lok = lresp.find("ok");
    CHECK(lok && lok->kind == Json::Bool && lok->b == true, "is_user_login -> ok:true");
    const Json* lres = lresp.find("result");
    CHECK(lres && lres->kind == Json::Bool && lres->b == false, "unloaded is_user_login is false");
}

static void test_bridge_safe_calls() {
    BambuBridge bridge;   // no plugin loaded
    CHECK(bridge.version().empty(), "no-plugin version() returns empty");
    CHECK(bridge.is_loaded() == false, "no-plugin is_loaded() false");
    int r = bridge.create_agent("/tmp");
    (void)r;
    std::string id = bridge.get_user_id();
    (void)id;
    std::string err;
    int ir = bridge.init_agent(&err);
    CHECK(ir != 0, "init_agent without plugin fails");
    CHECK(true, "bridge safe calls do not crash");
}

int main() {
    std::fprintf(stdout, "bambu-bridge self-test\n");
    test_json_roundtrip();
    test_rpc_unknown_method();
    test_rpc_missing_method();
    test_rpc_wrapped_response();
    test_bridge_safe_calls();
    if (g_fails == 0) {
        std::fprintf(stdout, "ALL SELF-TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d self-test(s) FAILED\n", g_fails);
    return 1;
}
