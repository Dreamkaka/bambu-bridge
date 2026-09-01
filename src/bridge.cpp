#include "bridge.hpp"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace bambu {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

void set_opt(unsigned* out, unsigned v) { if (out) *out = v; }

// Portable conversion of a void* (from dlsym/GetProcAddress) into a function
// pointer. ISO C++ forbids a direct cast; memcpy is the standard-safe way that
// works on GCC/Clang/MinGW/MSVC. The plugin symbols are all function pointers.
template <typename FnPtr>
FnPtr load_sym(void* p) {
    static_assert(sizeof(FnPtr) == sizeof(void*), "function pointer size mismatch");
    FnPtr f = nullptr;
    std::memcpy(&f, &p, sizeof(f));
    return f;
}

} // namespace

BambuBridge::~BambuBridge() { unload(); }

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
bool BambuBridge::load_plugin(const AgentConfig& cfg, std::string* err_) {
    unload();
    plugin_dir_ = cfg.plugin_dir;
    net_ = load_network_plugin(cfg.plugin_dir, err_);
    if (!net_.is_loaded()) return false;
    src_ = load_source_plugin(cfg.plugin_dir, nullptr);   // companion, non-fatal

    std::string serr;
    if (!resolve_symbols(&serr)) {
        if (err_) *err_ = serr;
        unload();
        return false;
    }
    loaded_ = true;
    last_error_.clear();

    if (!cfg.country_code.empty()) set_country_code(cfg.country_code);
    if (!cfg.config_dir.empty())   set_config_dir(cfg.config_dir);
    if (!cfg.cert_file.empty())    set_cert_file(cfg.cert_file, "");
    return true;
}

void BambuBridge::unload() {
    if (agent_) destroy_agent();
    net_.unload(); src_.unload();
    loaded_ = false;
    agent_  = nullptr;
    last_error_.clear();
}

std::string BambuBridge::version() { return get_version_str(); }

int BambuBridge::create_agent(const std::string& log_dir) {
    if (!loaded_ || !fn_create_agent) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    if (agent_) return BAMBU_NETWORK_SUCCESS;
    agent_ = fn_create_agent(log_dir);
    return agent_ ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

int BambuBridge::destroy_agent() {
    if (!agent_ || !fn_destroy_agent) { agent_ = nullptr; return BAMBU_NETWORK_SUCCESS; }
    int r = fn_destroy_agent(agent_);
    agent_ = nullptr;
    return r;
}

// ---------------------------------------------------------------------------
// plugin setup
// ---------------------------------------------------------------------------
int BambuBridge::set_country_code(const std::string& cc) {
    if (!has_agent() || !fn_set_country_code) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_country_code(agent_, cc);
}
int BambuBridge::set_config_dir(const std::string& dir) {
    if (!has_agent() || !fn_set_config_dir) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_config_dir(agent_, dir);
}
int BambuBridge::set_cert_file(const std::string& folder, const std::string& filename) {
    if (!has_agent() || !fn_set_cert_file) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_cert_file(agent_, folder, filename);
}
int BambuBridge::start() {
    if (!has_agent() || !fn_start) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_start(agent_);
}
std::string BambuBridge::get_version_str() {
    if (!fn_get_version) return {};
    return fn_get_version();
}

// ---------------------------------------------------------------------------
// login / server
// ---------------------------------------------------------------------------
int BambuBridge::connect_server() {
    if (!has_agent() || !fn_connect_server) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_connect_server(agent_);
}
bool BambuBridge::is_server_connected() const {
    if (!has_agent() || !fn_is_server_connected) return false;
    return fn_is_server_connected(agent_);
}
int BambuBridge::refresh_connection() {
    if (!has_agent() || !fn_refresh_connection) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_refresh_connection(agent_);
}
bool BambuBridge::is_user_login() const {
    if (!has_agent() || !fn_is_user_login) return false;
    return fn_is_user_login(agent_);
}
int BambuBridge::user_logout(bool request) {
    if (!has_agent() || !fn_user_logout) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_user_logout(agent_, request);
}
std::string BambuBridge::get_user_id() {
    if (!has_agent() || !fn_get_user_id) return {};
    return fn_get_user_id(agent_);
}
std::string BambuBridge::get_user_name() {
    if (!has_agent() || !fn_get_user_name) return {};
    return fn_get_user_name(agent_);
}
std::string BambuBridge::get_user_avatar() {
    if (!has_agent() || !fn_get_user_avatar) return {};
    return fn_get_user_avatar(agent_);
}
std::string BambuBridge::get_bambulab_host() {
    if (!has_agent() || !fn_get_bambulab_host) return {};
    return fn_get_bambulab_host(agent_);
}
std::string BambuBridge::get_user_selected_machine() {
    if (!has_agent() || !fn_get_user_selected_machine) return {};
    return fn_get_user_selected_machine(agent_);
}
int BambuBridge::set_user_selected_machine(const std::string& dev_id) {
    if (!has_agent() || !fn_set_user_selected_machine) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_user_selected_machine(agent_, dev_id);
}
std::string BambuBridge::get_studio_info_url() {
    if (!has_agent() || !fn_get_studio_info_url) return {};
    return fn_get_studio_info_url(agent_);
}

// ---------------------------------------------------------------------------
// subscribe / MQTT
// ---------------------------------------------------------------------------
int BambuBridge::add_subscribe(const std::vector<std::string>& dev_list) {
    if (!has_agent() || !fn_add_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_add_subscribe(agent_, dev_list);
}
int BambuBridge::del_subscribe(const std::vector<std::string>& dev_list) {
    if (!has_agent() || !fn_del_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_del_subscribe(agent_, dev_list);
}
int BambuBridge::send_message(const std::string& dev_id, const std::string& json_str, int qos, int flag) {
    if (!has_agent() || !fn_send_message) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_send_message(agent_, dev_id, json_str, qos, flag);
}
int BambuBridge::send_message_to_printer(const std::string& dev_id, const std::string& json_str, int qos, int flag) {
    if (!has_agent() || !fn_send_message_to_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_send_message_to_printer(agent_, dev_id, json_str, qos, flag);
}

// ---------------------------------------------------------------------------
// printers
// ---------------------------------------------------------------------------
int BambuBridge::connect_printer(const std::string& dev_id, const std::string& dev_ip,
                                 const std::string& username, const std::string& password, bool use_ssl) {
    if (!has_agent() || !fn_connect_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_connect_printer(agent_, dev_id, dev_ip, username, password, use_ssl);
}
int BambuBridge::disconnect_printer() {
    if (!has_agent() || !fn_disconnect_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_disconnect_printer(agent_);
}

// ---------------------------------------------------------------------------
// cloud read APIs
// ---------------------------------------------------------------------------
int BambuBridge::get_my_token(const std::string& ticket, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_my_token) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_my_token(agent_, ticket, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_my_profile(const std::string& token, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_my_profile) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_my_profile(agent_, token, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_user_tasks(const std::string& dev_id, int status, int offset, int limit, std::string* http_body) {
    if (!has_agent() || !fn_get_user_tasks) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    TaskQueryParams p; p.dev_id = dev_id; p.status = status; p.offset = offset; p.limit = limit;
    return fn_get_user_tasks(agent_, p, http_body);
}
int BambuBridge::get_printer_firmware(const std::string& dev_id, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_printer_firmware) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_printer_firmware(agent_, dev_id, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_slice_info(const std::string& project_id, const std::string& profile_id, int plate_index, std::string* slice_json) {
    if (!has_agent() || !fn_get_slice_info) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_get_slice_info(agent_, project_id, profile_id, plate_index, slice_json);
}
int BambuBridge::get_subtask_info(const std::string& subtask_id, std::string* task_json, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_subtask_info) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_subtask_info(agent_, subtask_id, task_json, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_model_mall_detail_url(std::string* url, const std::string& id) {
    if (!has_agent() || !fn_get_model_mall_detail_url) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_get_model_mall_detail_url(agent_, url, id);
}

// ---------------------------------------------------------------------------
// print
// ---------------------------------------------------------------------------
namespace {
// Decode a PrintParams JSON object.
PrintParams decode_print_params(const Json& j) {
    using J = const Json;
    auto gs = [](J& jj, const char* k) -> std::string {
        const Json* v = jj.find(k); return (v && v->kind == Json::String) ? v->str : "";
    };
    auto gi = [](J& jj, const char* k, int d) -> int {
        const Json* v = jj.find(k); return (v && v->kind == Json::Number) ? (int)v->num : d;
    };
    auto gb = [](J& jj, const char* k, bool d) -> bool {
        const Json* v = jj.find(k); return (v && v->kind == Json::Bool) ? v->b : d;
    };
    PrintParams p;
    p.dev_id              = gs(j, "dev_id");
    p.task_name           = gs(j, "task_name");
    p.project_name        = gs(j, "project_name");
    p.preset_name         = gs(j, "preset_name");
    p.filename            = gs(j, "filename");
    p.config_filename     = gs(j, "config_filename");
    p.plate_index         = gi(j, "plate_index", 0);
    p.ftp_folder          = gs(j, "ftp_folder");
    p.ftp_file            = gs(j, "ftp_file");
    p.ftp_file_md5        = gs(j, "ftp_file_md5");
    p.nozzle_mapping      = gs(j, "nozzle_mapping");
    p.ams_mapping         = gs(j, "ams_mapping");
    p.ams_mapping_info    = gs(j, "ams_mapping_info");
    p.nozzles_info        = gs(j, "nozzles_info");
    p.connection_type     = gs(j, "connection_type");
    p.print_type          = gs(j, "print_type");
    p.dst_file            = gs(j, "dst_file");
    p.dev_name            = gs(j, "dev_name");
    p.dev_ip              = gs(j, "dev_ip");
    p.use_ssl_for_ftp     = gb(j, "use_ssl_for_ftp", false);
    p.use_ssl_for_mqtt    = gb(j, "use_ssl_for_mqtt", true);
    p.username            = gs(j, "username");
    p.password            = gs(j, "password");
    p.task_bed_leveling   = gb(j, "task_bed_leveling", true);
    p.task_flow_cali      = gb(j, "task_flow_cali", true);
    p.task_vibration_cali = gb(j, "task_vibration_cali", true);
    p.task_layer_inspect  = gb(j, "task_layer_inspect", true);
    p.task_record_timelapse = gb(j, "task_record_timelapse", false);
    p.task_use_ams        = gb(j, "task_use_ams", false);
    p.task_bed_type       = gs(j, "task_bed_type");
    return p;
}
} // namespace

int BambuBridge::start_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                             std::function<bool()> cancel_cb) {
    if (!has_agent() || !fn_start_print) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    PrintParams p = decode_print_params(params_json);
    OnUpdateStatusFn update = [update_cb](int status, int, std::string msg) { if (update_cb) update_cb(status, msg); };
    WasCancelledFn cancel = [cancel_cb]() -> bool { return cancel_cb ? cancel_cb() : false; };
    OnWaitFn wait = [](int, std::string) -> bool { return true; };
    return fn_start_print(agent_, p, update, cancel, wait);
}

int BambuBridge::start_local_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                                   std::function<bool()> cancel_cb) {
    if (!has_agent() || !fn_start_local_print) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    PrintParams p = decode_print_params(params_json);
    OnUpdateStatusFn update = [update_cb](int status, int, std::string msg) { if (update_cb) update_cb(status, msg); };
    WasCancelledFn cancel = [cancel_cb]() -> bool { return cancel_cb ? cancel_cb() : false; };
    return fn_start_local_print(agent_, p, update, cancel);
}

// ---------------------------------------------------------------------------
// symbol resolution
// ---------------------------------------------------------------------------
bool BambuBridge::resolve_symbols(std::string* err) {
    auto S = [&](const char* n) -> void* { return net_.resolve(n); };
    auto need = [&](void* p, const char* n) -> bool { if (!p) { if (err) *err = std::string("missing symbol: ") + n; return false; } return true; };

    fn_get_version                  = load_sym<func_get_version>(S("bambu_network_get_version"));
    fn_create_agent                 = load_sym<func_create_agent>(S("bambu_network_create_agent"));
    fn_destroy_agent                = load_sym<func_destroy_agent>(S("bambu_network_destroy_agent"));
    fn_init_log                     = load_sym<func_init_log>(S("bambu_network_init_log"));
    fn_set_config_dir               = load_sym<func_set_config_dir>(S("bambu_network_set_config_dir"));
    fn_set_cert_file                = load_sym<func_set_cert_file>(S("bambu_network_set_cert_file"));
    fn_set_country_code             = load_sym<func_set_country_code>(S("bambu_network_set_country_code"));
    fn_start                        = load_sym<func_start>(S("bambu_network_start"));
    fn_connect_server               = load_sym<func_connect_server>(S("bambu_network_connect_server"));
    fn_is_server_connected          = load_sym<func_is_server_connected>(S("bambu_network_is_server_connected"));
    fn_refresh_connection           = load_sym<func_refresh_connection>(S("bambu_network_refresh_connection"));
    fn_is_user_login                = load_sym<func_is_user_login>(S("bambu_network_is_user_login"));
    fn_user_logout                  = load_sym<func_user_logout>(S("bambu_network_user_logout"));
    fn_get_user_id                  = load_sym<func_get_user_id>(S("bambu_network_get_user_id"));
    fn_get_user_name                = load_sym<func_get_user_name>(S("bambu_network_get_user_name"));
    fn_get_user_avatar              = load_sym<func_get_user_avatar>(S("bambu_network_get_user_avatar"));
    fn_get_bambulab_host            = load_sym<func_get_bambulab_host>(S("bambu_network_get_bambulab_host"));
    fn_get_user_selected_machine    = load_sym<func_get_user_selected_machine>(S("bambu_network_get_user_selected_machine"));
    fn_set_user_selected_machine    = load_sym<func_set_user_selected_machine>(S("bambu_network_set_user_selected_machine"));
    fn_get_studio_info_url          = load_sym<func_get_studio_info_url>(S("bambu_network_get_studio_info_url"));
    fn_add_subscribe                = load_sym<func_add_subscribe>(S("bambu_network_add_subscribe"));
    fn_del_subscribe                = load_sym<func_del_subscribe>(S("bambu_network_del_subscribe"));
    fn_send_message                 = load_sym<func_send_message>(S("bambu_network_send_message"));
    fn_send_message_to_printer      = load_sym<func_send_message_to_printer>(S("bambu_network_send_message_to_printer"));
    fn_connect_printer              = load_sym<func_connect_printer>(S("bambu_network_connect_printer"));
    fn_disconnect_printer           = load_sym<func_disconnect_printer>(S("bambu_network_disconnect_printer"));
    fn_get_model_mall_detail_url    = load_sym<func_get_model_mall_detail_url>(S("bambu_network_get_model_mall_detail_url"));
    fn_get_my_token                 = load_sym<func_get_my_token>(S("bambu_network_get_my_token"));
    fn_get_my_profile               = load_sym<func_get_my_profile>(S("bambu_network_get_my_profile"));
    fn_get_user_tasks               = load_sym<func_get_user_tasks>(S("bambu_network_get_user_tasks"));
    fn_get_printer_firmware         = load_sym<func_get_printer_firmware>(S("bambu_network_get_printer_firmware"));
    fn_get_slice_info               = load_sym<func_get_slice_info>(S("bambu_network_get_slice_info"));
    fn_get_subtask_info             = load_sym<func_get_subtask_info>(S("bambu_network_get_subtask_info"));
    fn_start_print                  = load_sym<func_start_print>(S("bambu_network_start_print"));
    fn_start_local_print            = load_sym<func_start_local_print>(S("bambu_network_start_local_print"));

    // Mandatory core symbols — refuse to run without these.
    if (!need((void*)fn_destroy_agent, "bambu_network_destroy_agent")) return false;
    if (!need((void*)fn_connect_server, "bambu_network_connect_server")) return false;
    if (!need((void*)fn_create_agent, "bambu_network_create_agent")) return false;

    return true;
}

} // namespace bambu
