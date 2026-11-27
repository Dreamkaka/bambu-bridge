#ifndef BAMBU_BRIDGE_BRIDGE_HPP
#define BAMBU_BRIDGE_BRIDGE_HPP

#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>

#include "plugin_loader.hpp"
#include "bambu_abi.hpp"

namespace bambu {

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

struct EventSink {
    virtual ~EventSink() = default;
    virtual void emit(const std::string& event, const Json& payload) = 0;
    virtual void on_state(const std::string& state) = 0;
};

struct AgentConfig {
    std::string plugin_dir;
    std::string log_dir;
    std::string country_code;
    std::string config_dir;
    std::string cert_file;
    std::string cert_folder;
    std::string cert_filename;
    std::string client_version;
};

class BambuBridge {
public:
    BambuBridge() = default;
    ~BambuBridge();
    BambuBridge(const BambuBridge&) = delete;
    BambuBridge& operator=(const BambuBridge&) = delete;

    bool load_plugin(const AgentConfig& cfg, std::string* err_);
    void unload();
    bool is_loaded() const { return loaded_; }

    int  init_agent(std::string* err = nullptr);
    int  create_agent(const std::string& log_dir);
    int  destroy_agent();
    std::string version();
    std::string client_version() const;

    int  set_country_code(const std::string& cc);
    int  set_config_dir(const std::string& dir);
    int  set_cert_file(const std::string& folder, const std::string& filename);
    int  init_log();
    int  start();
    int  set_extra_http_header(const std::map<std::string, std::string>& headers);
    std::string get_version_str();

    int  connect_server();
    bool is_server_connected() const;
    int  refresh_connection();
    bool is_user_login() const;
    int  user_logout(bool request);
    std::string get_user_id();
    std::string get_user_name();
    std::string get_user_avatar();
    std::string get_user_nickname();
    std::string get_bambulab_host();
    std::string get_host_for_login();
    std::string build_login_cmd();
    std::string build_logout_cmd();
    std::string build_login_info();
    std::string get_user_selected_machine();
    int  set_user_selected_machine(const std::string& dev_id);
    std::string get_studio_info_url();

    int  change_user(const std::string& user_info);
    int  login_with_ticket(const std::string& ticket, std::string* err);

    int  start_subscribe(const std::string& module);
    int  stop_subscribe(const std::string& module);
    int  add_subscribe(const std::vector<std::string>& dev_list);
    int  del_subscribe(const std::vector<std::string>& dev_list);
    void enable_multi_machine(bool enable);
    int  send_message_to_printer(const std::string& dev_id, const std::string& json_str, int qos, int flag);

    int  connect_printer(const std::string& dev_id, const std::string& dev_ip,
                         const std::string& username, const std::string& password, bool use_ssl,
                         bool* plugin_called = nullptr);
    int  disconnect_printer(bool* plugin_called = nullptr);
    bool start_discovery(bool start, bool sending);
    void install_device_cert(const std::string& dev_id, bool lan_only);
    int  check_cert();

    int  get_my_token(const std::string& ticket, std::string* http_body, unsigned* http_code);
    int  get_my_profile(const std::string& token, std::string* http_body, unsigned* http_code);
    int  get_user_print_info(std::string* http_body, unsigned* http_code, bool* plugin_called = nullptr);
    int  get_user_tasks(const std::string& dev_id, int status, int offset, int limit, std::string* http_body);
    int  get_printer_firmware(const std::string& dev_id, std::string* http_body, unsigned* http_code);
    int  get_slice_info(const std::string& project_id, const std::string& profile_id, int plate_index, std::string* slice_json);
    int  get_subtask_info(const std::string& subtask_id, std::string* task_json, std::string* http_body, unsigned* http_code);
    int  get_model_mall_detail_url(std::string* url, const std::string& id);
    int  get_camera_url(const std::string& dev_id, std::string* url);
    int  get_camera_url_for_golive(const std::string& dev_id, const std::string& sdev_id, std::string* url);
    std::string plugin_dir() const { return plugin_dir_; }

    int  start_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                     std::function<bool()> cancel_cb);
    int  start_local_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                           std::function<bool()> cancel_cb);


    struct CommandWait {
        int code = BAMBU_NETWORK_ERR_TIMEOUT;
        bool plugin_called = false;
        bool timed_out = true;
        std::string dev_id;
        std::string source;
        std::string msg;
        std::string error;
    };
    int  ensure_cloud_ready(const std::string& dev_id, int timeout_ms = 10000, std::string* err = nullptr,
                            bool* plugin_called = nullptr);
    CommandWait printer_command(const std::string& dev_id, const std::string& json_str, bool lan,
                                int timeout_ms = 8000, int qos = 0, int flag = 0, bool wait = true);
    static std::string next_command_sequence() {
        static std::atomic<unsigned> n{20000};
        return std::to_string(n.fetch_add(1, std::memory_order_relaxed));
    }
    std::string last_printer_msg(const std::string& dev_id) const;

    void set_event_sink(EventSink* s);
    void add_event_sink(EventSink* s);
    std::string last_error() const { return last_error_; }
    std::string country_code() const { return country_code_; }
    bool agent_ready() const { return agent_ != nullptr && started_; }
    bool printer_mqtt_ready(const std::string& dev_id) const;
    std::vector<std::string> mqtt_ready_devices() const;

private:
    bool resolve_symbols(std::string* err);
    bool has_agent() const { return agent_ != nullptr; }
    void install_callbacks();
    void emit(const std::string& event, const Json& payload);
    void emit_state(const std::string& state);
    void start_dispatcher();
    void stop_dispatcher();
    void queue_on_main(std::function<void()> fn);
    void apply_studio_http_headers();
    std::string find_cert_folder() const;
    int  after_login();
    void note_printer_msg(const std::string& dev_id, const std::string& source, const std::string& msg);
    static std::string parse_printer_id(const std::string& topic);
    void mark_printer_mqtt(const std::string& dev_id);
    void unmark_printer_mqtt(const std::string& dev_id);

    PluginHandle net_, src_;
    std::vector<PluginHandle> companions_;
    bool loaded_ = false;
    bool started_ = false;
    void* agent_ = nullptr;
    std::mutex sink_mtx_;
    std::vector<EventSink*> sinks_;
    std::string last_error_;
    std::string plugin_dir_;
    std::string country_code_;
    std::string client_version_override_;
    std::string config_dir_;
    std::string log_dir_;
    std::string cert_folder_;
    std::string cert_filename_;

    std::mutex dispatch_mtx_;
    std::condition_variable dispatch_cv_;
    std::queue<std::function<void()>> dispatch_q_;
    std::thread dispatch_thread_;
    std::atomic<bool> dispatch_running_{false};

    struct PendingWait {
        std::string dev_id;
        bool lan_only = false;
        std::string group, command, sequence;
        std::condition_variable cv;
        bool done = false;
        std::string source;
        std::string msg;
    };
    mutable std::mutex wait_mtx_;
    std::vector<std::shared_ptr<PendingWait>> pending_waits_;
    std::map<std::string, std::pair<std::string, std::string>> last_msg_;
    std::set<std::string> mqtt_ready_;

    func_check_debug_consistent       fn_check_debug_consistent       = nullptr;
    func_get_version                  fn_get_version                  = nullptr;
    func_create_agent                 fn_create_agent                 = nullptr;
    func_destroy_agent                fn_destroy_agent                = nullptr;
    func_init_log                     fn_init_log                     = nullptr;
    func_set_config_dir               fn_set_config_dir               = nullptr;
    func_set_cert_file                fn_set_cert_file                = nullptr;
    func_set_country_code             fn_set_country_code             = nullptr;
    func_start                        fn_start                        = nullptr;
    func_set_on_ssdp_msg_fn           fn_set_on_ssdp_msg_fn           = nullptr;
    func_set_on_user_login_fn         fn_set_on_user_login_fn         = nullptr;
    func_set_on_printer_connected_fn  fn_set_on_printer_connected_fn  = nullptr;
    func_set_on_server_connected_fn   fn_set_on_server_connected_fn   = nullptr;
    func_set_on_http_error_fn         fn_set_on_http_error_fn         = nullptr;
    func_set_get_country_code_fn      fn_set_get_country_code_fn      = nullptr;
    func_set_on_subscribe_failure_fn  fn_set_on_subscribe_failure_fn  = nullptr;
    func_set_on_message_fn            fn_set_on_message_fn            = nullptr;
    func_set_on_user_message_fn       fn_set_on_user_message_fn       = nullptr;
    func_set_on_local_connect_fn      fn_set_on_local_connect_fn      = nullptr;
    func_set_on_local_message_fn      fn_set_on_local_message_fn      = nullptr;
    func_set_queue_on_main_fn         fn_set_queue_on_main_fn         = nullptr;
    func_connect_server               fn_connect_server               = nullptr;
    func_is_server_connected          fn_is_server_connected          = nullptr;
    func_refresh_connection           fn_refresh_connection           = nullptr;
    func_start_subscribe              fn_start_subscribe              = nullptr;
    func_stop_subscribe               fn_stop_subscribe               = nullptr;
    func_add_subscribe                fn_add_subscribe                = nullptr;
    func_del_subscribe                fn_del_subscribe                = nullptr;
    func_enable_multi_machine         fn_enable_multi_machine         = nullptr;
    func_send_message                 fn_send_message                 = nullptr;
    func_send_message_to_printer      fn_send_message_to_printer      = nullptr;
    func_connect_printer              fn_connect_printer              = nullptr;
    func_disconnect_printer           fn_disconnect_printer           = nullptr;
    func_check_cert                   fn_check_cert                   = nullptr;
    func_install_device_cert          fn_install_device_cert          = nullptr;
    func_start_discovery              fn_start_discovery              = nullptr;
    func_is_user_login                fn_is_user_login                = nullptr;
    func_user_logout                  fn_user_logout                  = nullptr;
    func_get_user_id                  fn_get_user_id                  = nullptr;
    func_get_user_name                fn_get_user_name                = nullptr;
    func_get_user_avatar              fn_get_user_avatar              = nullptr;
    func_get_user_nickanme            fn_get_user_nickanme            = nullptr;
    func_get_bambulab_host            fn_get_bambulab_host            = nullptr;
    func_get_user_selected_machine    fn_get_user_selected_machine    = nullptr;
    func_set_user_selected_machine    fn_set_user_selected_machine    = nullptr;
    func_get_studio_info_url          fn_get_studio_info_url          = nullptr;
    func_change_user                  fn_change_user                  = nullptr;
    func_build_login_cmd              fn_build_login_cmd              = nullptr;
    func_build_logout_cmd             fn_build_logout_cmd             = nullptr;
    func_build_login_info             fn_build_login_info             = nullptr;
    func_set_server_callback          fn_set_server_callback          = nullptr;
    func_set_extra_http_header        fn_set_extra_http_header        = nullptr;
    func_get_user_print_info          fn_get_user_print_info          = nullptr;
    func_get_my_token                 fn_get_my_token                 = nullptr;
    func_get_my_profile               fn_get_my_profile               = nullptr;
    func_get_user_tasks               fn_get_user_tasks               = nullptr;
    func_get_printer_firmware         fn_get_printer_firmware         = nullptr;
    func_get_slice_info               fn_get_slice_info               = nullptr;
    func_get_subtask_info             fn_get_subtask_info             = nullptr;
    func_get_model_mall_detail_url    fn_get_model_mall_detail_url    = nullptr;
    func_get_camera_url               fn_get_camera_url               = nullptr;
    func_get_camera_url_for_golive    fn_get_camera_url_for_golive    = nullptr;
    func_start_print                  fn_start_print                  = nullptr;
    func_start_local_print            fn_start_local_print            = nullptr;
};

} // namespace bambu

#endif
