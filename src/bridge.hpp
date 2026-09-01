#ifndef BAMBU_BRIDGE_BRIDGE_HPP
#define BAMBU_BRIDGE_BRIDGE_HPP

// ============================================================================
// bridge.hpp
//
// The high-level BambuBridge class. Owns the loaded plugin, creates the agent,
// marshals C++ ABI calls into simple synchronous return values / events, and
// exposes a small stable method set for the transport layers (HTTP / JSON-RPC).
//
// This is the ONLY component that talks to the plugin's C++ ABI. Transports
// should not include bambu_abi.hpp directly.
//
// IMPORTANT: we store the plugin's functions as RAW function pointers (func_*)
// from bambu_abi.hpp, NOT std::function. Storing a std::function<>'s result of
// casting a void*/function pointer is UB and won't compile. The C++ callback
// parameters (OnUpdateStatusFn etc.) are std::function typedefs and can be
// constructed directly from lambdas when calling.
// ============================================================================

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <memory>

#include "plugin_loader.hpp"
#include "bambu_abi.hpp"

namespace bambu {

// Minimal JSON value carried across the transport boundary (no external dep).
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool           b    = false;
    double         num  = 0.0;
    std::string    str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    static Json object()                      { Json j; j.kind = Object; return j; }
    static Json array()                       { Json j; j.kind = Array;  return j; }
    static Json s(const std::string& v)       { Json j; j.kind = String; j.str = v; return j; }
    static Json s(const char* v)              { Json j; j.kind = String; j.str = v ? v : ""; return j; }
    static Json n(double v)                   { Json j; j.kind = Number; j.num = v; return j; }
    static Json boolean(bool v)               { Json j; j.kind = Bool;   j.b = v; return j; }
    static Json null()                        { Json j; j.kind = Null;   return j; }

    Json& set(const std::string& key, Json v) {
        if (kind != Object) { kind = Object; obj.clear(); }
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return *this; }
        obj.emplace_back(key, std::move(v));
        return *this;
    }
    Json* find(const std::string& key) { for (auto& kv : obj) if (kv.first == key) return &kv.second; return nullptr; }
    const Json* find(const std::string& key) const { for (auto& kv : obj) if (kv.first == key) return &kv.second; return nullptr; }
};

// Transport implementations implement this to forward async events.
struct EventSink {
    virtual ~EventSink() = default;
    virtual void emit(const std::string& event, const Json& payload) = 0;
    virtual void on_state(const std::string& state) = 0;
};

struct AgentConfig {
    std::string plugin_dir;    // dir containing bamboo_networking(.so/.dll)
    std::string log_dir;       // dir for plugin logs
    std::string country_code;  // "US" / "CN"
    std::string config_dir;    // plugin config dir
    std::string cert_file;     // optional cert file
};

class BambuBridge {
public:
    BambuBridge() = default;
    ~BambuBridge();
    BambuBridge(const BambuBridge&) = delete;
    BambuBridge& operator=(const BambuBridge&) = delete;

    // lifecycle
    bool load_plugin(const AgentConfig& cfg, std::string* err_);
    void unload();
    bool is_loaded() const { return loaded_; }
    int  create_agent(const std::string& log_dir);
    int  destroy_agent();
    std::string version();

    // plugin setup
    int  set_country_code(const std::string& cc);
    int  set_config_dir(const std::string& dir);
    int  set_cert_file(const std::string& folder, const std::string& filename);
    int  start();
    std::string get_version_str();

    // login / server
    int  connect_server();
    bool is_server_connected() const;
    int  refresh_connection();
    bool is_user_login() const;
    int  user_logout(bool request);
    std::string get_user_id();
    std::string get_user_name();
    std::string get_user_avatar();
    std::string get_bambulab_host();
    // Returns the Bambu web host appropriate for the configured country code.
    // If the plugin returns a .com host but country is CN, this maps it to .cn
    // (and vice versa). Used to build the correct login URL.
    std::string get_host_for_login();
    // Plugin-provided login guidance: these ask the plugin what it wants.
    std::string build_login_cmd();
    std::string build_logout_cmd();
    std::string build_login_info();
    void set_on_user_login(OnUserLoginFn fn);
    std::string get_user_selected_machine();
    int  set_user_selected_machine(const std::string& dev_id);
    std::string get_studio_info_url();

    // Establish a login session. The plugin uses Bambu Studio's OAuth flow:
    // ticket -> access_token (get_my_token) -> profile (get_my_profile) ->
    // change_user(payload) -> is_user_login().
    // `payload` is the canonical login payload JSON (see rpc_handler "login").
    int  change_user(const std::string& user_info);
    // One-shot: give a ticket, returns 0 if login succeeded.
    int  login_with_ticket(const std::string& ticket, std::string* err);

    // subscribe / MQTT
    int  add_subscribe(const std::vector<std::string>& dev_list);
    int  del_subscribe(const std::vector<std::string>& dev_list);
    int  send_message(const std::string& dev_id, const std::string& json_str, int qos, int flag);
    int  send_message_to_printer(const std::string& dev_id, const std::string& json_str, int qos, int flag);

    // printers
    int connect_printer(const std::string& dev_id, const std::string& dev_ip,
                        const std::string& username, const std::string& password, bool use_ssl);
    int disconnect_printer();

    // cloud read APIs (synchronous)
    int  get_my_token(const std::string& ticket, std::string* http_body, unsigned* http_code);
    int  get_my_profile(const std::string& token, std::string* http_body, unsigned* http_code);
    int  get_user_tasks(const std::string& dev_id, int status, int offset, int limit, std::string* http_body);
    int  get_printer_firmware(const std::string& dev_id, std::string* http_body, unsigned* http_code);
    int  get_slice_info(const std::string& project_id, const std::string& profile_id, int plate_index, std::string* slice_json);
    int  get_subtask_info(const std::string& subtask_id, std::string* task_json, std::string* http_body, unsigned* http_code);
    int  get_model_mall_detail_url(std::string* url, const std::string& id);

    // print
    int start_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                    std::function<bool()> cancel_cb);
    int start_local_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                          std::function<bool()> cancel_cb);

    // event sink
    void set_event_sink(EventSink* s) { sink_ = s; }

    std::string last_error() const { return last_error_; }

private:
    bool resolve_symbols(std::string* err);
    bool has_agent() const { return agent_ != nullptr; }

    PluginHandle net_, src_;
    bool loaded_ = false;
    void* agent_ = nullptr;
    EventSink* sink_ = nullptr;
    std::string last_error_;
    std::string plugin_dir_;
    std::string country_code_;

    // RAW function pointers (func_* typedefs from bambu_abi.hpp).
    func_get_version                  fn_get_version                  = nullptr;
    func_create_agent                 fn_create_agent                 = nullptr;
    func_destroy_agent                fn_destroy_agent                = nullptr;
    func_init_log                     fn_init_log                     = nullptr;
    func_set_config_dir               fn_set_config_dir               = nullptr;
    func_set_cert_file                fn_set_cert_file                = nullptr;
    func_set_country_code             fn_set_country_code             = nullptr;
    func_start                        fn_start                        = nullptr;
    func_connect_server               fn_connect_server               = nullptr;
    func_is_server_connected          fn_is_server_connected          = nullptr;
    func_refresh_connection           fn_refresh_connection           = nullptr;
    func_is_user_login                fn_is_user_login                = nullptr;
    func_user_logout                  fn_user_logout                  = nullptr;
    func_get_user_id                  fn_get_user_id                  = nullptr;
    func_get_user_name                fn_get_user_name                = nullptr;
    func_get_user_avatar              fn_get_user_avatar              = nullptr;
    func_get_bambulab_host            fn_get_bambulab_host            = nullptr;
    func_get_user_selected_machine    fn_get_user_selected_machine    = nullptr;
    func_set_user_selected_machine    fn_set_user_selected_machine    = nullptr;
    func_get_studio_info_url          fn_get_studio_info_url          = nullptr;
    func_add_subscribe                fn_add_subscribe                = nullptr;
    func_del_subscribe                fn_del_subscribe                = nullptr;
    func_send_message                 fn_send_message                 = nullptr;
    func_send_message_to_printer      fn_send_message_to_printer      = nullptr;
    func_connect_printer              fn_connect_printer              = nullptr;
    func_disconnect_printer           fn_disconnect_printer           = nullptr;
    func_get_model_mall_detail_url    fn_get_model_mall_detail_url    = nullptr;
    func_get_my_token                 fn_get_my_token                 = nullptr;
    func_get_my_profile               fn_get_my_profile               = nullptr;
    func_change_user                  fn_change_user                 = nullptr;
    func_build_login_cmd              fn_build_login_cmd             = nullptr;
    func_build_logout_cmd             fn_build_logout_cmd            = nullptr;
    func_build_login_info             fn_build_login_info            = nullptr;
    func_set_on_user_login_fn         fn_set_on_user_login_fn        = nullptr;
    func_get_user_tasks               fn_get_user_tasks               = nullptr;
    func_get_printer_firmware         fn_get_printer_firmware         = nullptr;
    func_get_slice_info               fn_get_slice_info               = nullptr;
    func_get_subtask_info             fn_get_subtask_info             = nullptr;
    func_start_print                  fn_start_print                  = nullptr;
    func_start_local_print            fn_start_local_print            = nullptr;
};

} // namespace bambu

#endif // BAMBU_BRIDGE_BRIDGE_HPP
