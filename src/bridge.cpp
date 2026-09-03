#include "bridge.hpp"
#include "json.hpp"
#include "camera_stream.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cerrno>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <pwd.h>
#endif

namespace bambu {

namespace {

void set_opt(unsigned* out, unsigned v) { if (out) *out = v; }

template <typename FnPtr>
FnPtr load_sym(void* p) {
    static_assert(sizeof(FnPtr) == sizeof(void*), "function pointer size mismatch");
    FnPtr f = nullptr;
    std::memcpy(&f, &p, sizeof(f));
    return f;
}

bool path_exists_local(const std::string& p) {
#ifdef _WIN32
    DWORD attrs = ::GetFileAttributesA(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    return ::access(p.c_str(), F_OK) == 0;
#endif
}

std::string join2(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + sep + b;
}

bool mkdir_p(const std::string& dir) {
    if (dir.empty() || path_exists_local(dir)) return true;
#ifdef _WIN32
    return ::CreateDirectoryA(dir.c_str(), nullptr) != 0 || ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string default_home() {
#ifdef _WIN32
    const char* a = std::getenv("APPDATA");
    if (a && *a) return a;
    const char* u = std::getenv("USERPROFILE");
    return u ? u : ".";
#else
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    struct passwd* pw = ::getpwuid(::getuid());
    return (pw && pw->pw_dir) ? pw->pw_dir : ".";
#endif
}

std::string json_field(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pat.size());
    if (colon == std::string::npos) return "";
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    if (i < json.size() && json[i] == '"') {
        std::string out;
        ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) { out += json[i + 1]; i += 2; }
            else out += json[i++];
        }
        return out;
    }
    size_t start = i;
    while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ' ' && json[i] != '\n') ++i;
    return json.substr(start, i - start);
}

bool looks_like_login_data(const Json& j) {
    if (j.kind != Json::Object) return false;
    return j.find("token") || j.find("access_token") || j.find("refresh_token")
        || j.find("code") || j.find("user") || j.find("user_id") || j.find("uidStr");
}

std::string json_str_field(const Json& j, const char* k) {
    const Json* v = j.find(k);
    if (!v) return "";
    if (v->kind == Json::String) return v->str;
    if (v->kind == Json::Number) {
        if (v->num == (long long)v->num) return std::to_string((long long)v->num);
        return std::to_string(v->num);
    }
    if (v->kind == Json::Bool) return v->b ? "true" : "false";
    return "";
}

Json normalize_change_user(const Json& input) {
    if (input.kind != Json::Object) return input;
    std::string command = json_str_field(input, "command");
    const Json* data = input.find("data");
    if ((command == "user_login" || command == "login_token") && data && data->kind == Json::Object)
        return input;
    if (data && looks_like_login_data(*data)) {
        Json out = Json::object();
        out.set("command", Json::s(command.empty() ? "user_login" : command));
        out.set("data", *data);
        return out;
    }
    if (looks_like_login_data(input)) {
        Json out = Json::object();
        out.set("command", Json::s("user_login"));
        out.set("data", input);
        return out;
    }
    return input;
}

PrintParams decode_print_params(const Json& j) {
    auto gs = [](const Json& jj, const char* k) -> std::string {
        const Json* v = jj.find(k); return (v && v->kind == Json::String) ? v->str : "";
    };
    auto gi = [](const Json& jj, const char* k, int d) -> int {
        const Json* v = jj.find(k); return (v && v->kind == Json::Number) ? (int)v->num : d;
    };
    auto gb = [](const Json& jj, const char* k, bool d) -> bool {
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
    p.ams_mapping2        = gs(j, "ams_mapping2");
    p.ams_mapping_info    = gs(j, "ams_mapping_info");
    p.nozzles_info        = gs(j, "nozzles_info");
    p.connection_type     = gs(j, "connection_type");
    p.comments            = gs(j, "comments");
    p.origin_profile_id   = gi(j, "origin_profile_id", 0);
    p.stl_design_id       = gi(j, "stl_design_id", 0);
    p.origin_model_id     = gs(j, "origin_model_id");
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
    p.extra_options       = gs(j, "extra_options");
    p.auto_bed_leveling   = gi(j, "auto_bed_leveling", 0);
    p.auto_flow_cali      = gi(j, "auto_flow_cali", 0);
    p.auto_offset_cali    = gi(j, "auto_offset_cali", 0);
    p.extruder_cali_manual_mode = gi(j, "extruder_cali_manual_mode", -1);
    p.task_ext_change_assist = gb(j, "task_ext_change_assist", false);
    p.try_emmc_print      = gb(j, "try_emmc_print", false);
    return p;
}

} // namespace

BambuBridge::~BambuBridge() { unload(); }

bool BambuBridge::load_plugin(const AgentConfig& cfg, std::string* err_) {
    unload();
    plugin_dir_ = cfg.plugin_dir;
    country_code_ = cfg.country_code.empty() ? "CN" : cfg.country_code;
    config_dir_ = cfg.config_dir;
    log_dir_ = cfg.log_dir;
    cert_folder_ = cfg.cert_folder;
    cert_filename_ = cfg.cert_filename.empty() ? "slicer_base64.cer" : cfg.cert_filename;
    if (!cfg.cert_file.empty() && cert_folder_.empty())
        cert_folder_ = cfg.cert_file;

    prepare_plugin_search_path(plugin_dir_);
    companions_ = load_companion_plugins(plugin_dir_);
    {
        std::string cam_err;
        load_camera_source(plugin_dir_, &cam_err);
    }
    net_ = load_network_plugin(plugin_dir_, err_);
    if (!net_.is_loaded()) return false;
    src_ = load_source_plugin(plugin_dir_, nullptr);

    std::string serr;
    if (!resolve_symbols(&serr)) {
        if (err_) *err_ = serr;
        unload();
        return false;
    }
    loaded_ = true;
    last_error_.clear();
    return true;
}

void BambuBridge::unload() {
    stop_dispatcher();
    if (agent_) destroy_agent();
    net_.unload();
    src_.unload();
    companions_.clear();
    loaded_ = false;
    started_ = false;
    agent_ = nullptr;
    last_error_.clear();
}

std::string BambuBridge::version() { return get_version_str(); }

int BambuBridge::create_agent(const std::string& log_dir) {
    if (!loaded_ || !fn_create_agent) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    if (agent_) return BAMBU_NETWORK_SUCCESS;
    if (!log_dir.empty()) log_dir_ = log_dir;
    if (log_dir_.empty()) log_dir_ = join2(default_home(), "bambu-bridge");
    mkdir_p(log_dir_);
    agent_ = fn_create_agent(log_dir_);
    return agent_ ? BAMBU_NETWORK_SUCCESS : BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

int BambuBridge::destroy_agent() {
    stop_dispatcher();
    if (!agent_ || !fn_destroy_agent) { agent_ = nullptr; started_ = false; return BAMBU_NETWORK_SUCCESS; }
    int r = fn_destroy_agent(agent_);
    agent_ = nullptr;
    started_ = false;
    return r;
}

int BambuBridge::init_agent(std::string* err) {
    if (!loaded_) {
        if (err) *err = "plugin not loaded";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    if (started_ && agent_) return BAMBU_NETWORK_SUCCESS;

    if (config_dir_.empty()) config_dir_ = join2(default_home(), "bambu-bridge");
    if (log_dir_.empty()) log_dir_ = join2(config_dir_, "log");
    mkdir_p(config_dir_);
    mkdir_p(log_dir_);

    int r = create_agent(log_dir_);
    if (r != BAMBU_NETWORK_SUCCESS || !agent_) {
        if (err) *err = "create_agent failed";
        return r == BAMBU_NETWORK_SUCCESS ? BAMBU_NETWORK_ERR_INVALID_HANDLE : r;
    }

    if (fn_set_config_dir) fn_set_config_dir(agent_, config_dir_);
    if (fn_init_log) fn_init_log(agent_);

    if (cert_folder_.empty()) cert_folder_ = find_cert_folder();
    if (!cert_folder_.empty() && fn_set_cert_file)
        fn_set_cert_file(agent_, cert_folder_, cert_filename_);

    apply_studio_http_headers();
    start_dispatcher();
    install_callbacks();

    if (fn_set_country_code) fn_set_country_code(agent_, country_code_);
    if (fn_start) {
        r = fn_start(agent_);
        if (r != BAMBU_NETWORK_SUCCESS) {
            if (err) *err = "start failed: " + std::to_string(r);
            return r;
        }
    }
    started_ = true;
    if (fn_start_discovery) fn_start_discovery(agent_, true, false);
    emit_state("ready");
    return BAMBU_NETWORK_SUCCESS;
}

int BambuBridge::set_country_code(const std::string& cc) {
    country_code_ = cc;
    if (!has_agent() || !fn_set_country_code) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_country_code(agent_, cc);
}
int BambuBridge::set_config_dir(const std::string& dir) {
    config_dir_ = dir;
    if (!has_agent() || !fn_set_config_dir) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_config_dir(agent_, dir);
}
int BambuBridge::set_cert_file(const std::string& folder, const std::string& filename) {
    cert_folder_ = folder;
    if (!filename.empty()) cert_filename_ = filename;
    if (!has_agent() || !fn_set_cert_file) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_cert_file(agent_, folder, cert_filename_);
}
int BambuBridge::init_log() {
    if (!has_agent() || !fn_init_log) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_init_log(agent_);
}
int BambuBridge::start() {
    if (!has_agent() || !fn_start) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    int r = fn_start(agent_);
    if (r == BAMBU_NETWORK_SUCCESS) started_ = true;
    return r;
}
int BambuBridge::set_extra_http_header(const std::map<std::string, std::string>& headers) {
    if (!has_agent() || !fn_set_extra_http_header) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_set_extra_http_header(agent_, headers);
}
std::string BambuBridge::get_version_str() {
    if (!fn_get_version) return {};
    return fn_get_version();
}

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
std::string BambuBridge::get_user_nickname() {
    if (!has_agent() || !fn_get_user_nickanme) return {};
    return fn_get_user_nickanme(agent_);
}
std::string BambuBridge::get_bambulab_host() {
    if (!has_agent() || !fn_get_bambulab_host) return {};
    return fn_get_bambulab_host(agent_);
}

std::string BambuBridge::get_host_for_login() {
    std::string host = get_bambulab_host();
    bool cn = (country_code_ == "CN");
    auto has = [&](const std::string& sub) { return host.find(sub) != std::string::npos; };
    if (cn) {
        if (has("bambulab.com")) {
            size_t p = host.find("bambulab.com");
            host = host.substr(0, p) + "bambulab.cn" + host.substr(p + std::string("bambulab.com").size());
        } else if (host.empty()) {
            host = "https://bambulab.cn";
        }
    } else {
        if (has("bambulab.cn")) {
            size_t p = host.find("bambulab.cn");
            host = host.substr(0, p) + "bambulab.com" + host.substr(p + std::string("bambulab.cn").size());
        } else if (host.empty()) {
            host = "https://bambulab.com";
        }
    }
    return host;
}

std::string BambuBridge::build_login_cmd() {
    if (!has_agent() || !fn_build_login_cmd) return {};
    return fn_build_login_cmd(agent_);
}
std::string BambuBridge::build_logout_cmd() {
    if (!has_agent() || !fn_build_logout_cmd) return {};
    return fn_build_logout_cmd(agent_);
}
std::string BambuBridge::build_login_info() {
    if (!has_agent() || !fn_build_login_info) return {};
    return fn_build_login_info(agent_);
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

int BambuBridge::change_user(const std::string& user_info) {
    if (!has_agent() || !fn_change_user) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    std::string payload = user_info;
    Json parsed;
    if (json_parse(user_info, &parsed))
        payload = json_dump(normalize_change_user(parsed));
    int r = fn_change_user(agent_, payload);
    if (r == BAMBU_NETWORK_SUCCESS) after_login();
    return r;
}

int BambuBridge::login_with_ticket(const std::string& ticket, std::string* err) {
    std::string token_body, profile_body;
    unsigned tcode = 0;
    int tr = get_my_token(ticket, &token_body, &tcode);
    if (tr != 0) { if (err) *err = "get_my_token failed: " + std::to_string(tr); return tr; }
    std::string access = json_field(token_body, "accessToken");
    if (access.empty()) access = json_field(token_body, "access_token");
    if (access.empty()) access = json_field(token_body, "token");
    if (access.empty()) { if (err) *err = "no accessToken in token response"; return BAMBU_NETWORK_ERR_INVALID_RESULT; }

    unsigned pcode = 0;
    int pr = get_my_profile(access, &profile_body, &pcode);
    if (pr != 0) { if (err) *err = "get_my_profile failed: " + std::to_string(pr); return pr; }

    std::string refresh = json_field(token_body, "refreshToken");
    if (refresh.empty()) refresh = json_field(token_body, "refresh_token");
    std::string exp     = json_field(token_body, "expiresIn");
    if (exp.empty()) exp = json_field(token_body, "expires_in");
    std::string rexp    = json_field(token_body, "refreshExpiresIn");
    if (rexp.empty()) rexp = json_field(token_body, "refresh_expires_in");
    std::string uid     = json_field(profile_body, "uidStr");
    if (uid.empty()) uid = json_field(profile_body, "uid");
    if (uid.empty()) uid = json_field(profile_body, "id");
    std::string name    = json_field(profile_body, "name");
    std::string account = json_field(profile_body, "account");
    std::string avatar  = json_field(profile_body, "avatar");

    Json data = Json::object();
    data.set("token", Json::s(access));
    data.set("access_token", Json::s(access));
    data.set("refresh_token", Json::s(refresh));
    data.set("expires_in", Json::s(exp));
    data.set("refresh_expires_in", Json::s(rexp));
    data.set("user_id", Json::s(uid));
    data.set("uidStr", Json::s(uid));
    Json user = Json::object();
    user.set("id", Json::s(uid));
    user.set("uid", Json::s(uid));
    user.set("uidStr", Json::s(uid));
    user.set("name", Json::s(name));
    user.set("account", Json::s(account));
    user.set("avatar", Json::s(avatar));
    data.set("user", user);
    Json payload = Json::object();
    payload.set("command", Json::s("user_login"));
    payload.set("data", data);

    int cu = change_user(json_dump(payload));
    if (cu != 0) { if (err) *err = "change_user failed: " + std::to_string(cu); return cu; }
    if (!is_user_login()) { if (err) *err = "is_user_login() false after change_user"; return BAMBU_NETWORK_ERR_INVALID_RESULT; }
    return BAMBU_NETWORK_SUCCESS;
}

int BambuBridge::after_login() {
    if (!has_agent()) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    if (fn_start_subscribe) fn_start_subscribe(agent_, "app");
    if (fn_connect_server) fn_connect_server(agent_);
    Json ev = Json::object();
    ev.set("logged_in", Json::boolean(is_user_login()));
    ev.set("user_id", Json::s(get_user_id()));
    ev.set("user_name", Json::s(get_user_name()));
    emit("user_login", ev);
    return BAMBU_NETWORK_SUCCESS;
}

int BambuBridge::start_subscribe(const std::string& module) {
    if (!has_agent() || !fn_start_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_start_subscribe(agent_, module);
}
int BambuBridge::stop_subscribe(const std::string& module) {
    if (!has_agent() || !fn_stop_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_stop_subscribe(agent_, module);
}
int BambuBridge::add_subscribe(const std::vector<std::string>& dev_list) {
    if (!has_agent() || !fn_add_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_add_subscribe(agent_, dev_list);
}
int BambuBridge::del_subscribe(const std::vector<std::string>& dev_list) {
    if (!has_agent() || !fn_del_subscribe) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_del_subscribe(agent_, dev_list);
}
void BambuBridge::enable_multi_machine(bool enable) {
    if (!has_agent() || !fn_enable_multi_machine) return;
    fn_enable_multi_machine(agent_, enable);
}
int BambuBridge::send_message(const std::string& dev_id, const std::string& json_str, int qos, int flag) {
    if (!has_agent() || !fn_send_message) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_send_message(agent_, dev_id, json_str, qos, flag);
}
int BambuBridge::send_message_to_printer(const std::string& dev_id, const std::string& json_str, int qos, int flag) {
    if (!has_agent() || !fn_send_message_to_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_send_message_to_printer(agent_, dev_id, json_str, qos, flag);
}
int BambuBridge::ensure_cloud_ready(const std::string& dev_id, int timeout_ms, std::string* err) {
    if (!has_agent()) {
        if (err) *err = "agent not created";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    if (!is_user_login()) {
        if (err) *err = "not logged in; open GET /login first";
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
    if (fn_start_subscribe) fn_start_subscribe(agent_, "app");
    if (!is_server_connected() && fn_connect_server) fn_connect_server(agent_);
    int waited = 0;
    while (!is_server_connected() && waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waited += 200;
        if (!is_server_connected() && fn_connect_server && waited % 2000 == 0)
            fn_connect_server(agent_);
    }
    if (!is_server_connected()) {
        if (err) *err = "cloud MQTT not connected (is_server_connected=false)";
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }
    if (!dev_id.empty()) {
        add_subscribe({dev_id});
        set_user_selected_machine(dev_id);
    }
    return BAMBU_NETWORK_SUCCESS;
}

int BambuBridge::printer_command(const std::string& dev_id, const std::string& json_str, bool lan, int qos, int flag) {
    if (!lan) {
        std::string e;
        int ready = ensure_cloud_ready(dev_id, 10000, &e);
        if (ready != BAMBU_NETWORK_SUCCESS) return ready;
    }
    return lan ? send_message_to_printer(dev_id, json_str, qos, flag)
               : send_message(dev_id, json_str, qos, flag);
}

void BambuBridge::note_printer_msg(const std::string& dev_id, const std::string& source, const std::string& msg) {
    std::lock_guard<std::mutex> lk(wait_mtx_);
    last_msg_[dev_id] = {source, msg};
    for (auto& w : pending_waits_) {
        if (!w || w->done) continue;
        if (!w->dev_id.empty() && w->dev_id != dev_id) continue;
        w->source = source;
        w->msg = msg;
        w->done = true;
        w->cv.notify_all();
    }
}

std::string BambuBridge::last_printer_msg(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(wait_mtx_);
    auto it = last_msg_.find(dev_id);
    if (it == last_msg_.end()) return {};
    return it->second.second;
}

BambuBridge::CommandWait BambuBridge::printer_command_wait(const std::string& dev_id, const std::string& json_str,
                                                           bool lan, int timeout_ms, int qos, int flag) {
    CommandWait out;
    out.dev_id = dev_id;
    auto waiter = std::make_shared<PendingWait>();
    waiter->dev_id = dev_id;
    {
        std::lock_guard<std::mutex> lk(wait_mtx_);
        pending_waits_.push_back(waiter);
    }
    if (!lan) {
        int ready = ensure_cloud_ready(dev_id, timeout_ms > 0 ? timeout_ms : 10000, &out.error);
        if (ready != BAMBU_NETWORK_SUCCESS) {
            std::lock_guard<std::mutex> lk(wait_mtx_);
            pending_waits_.erase(std::remove(pending_waits_.begin(), pending_waits_.end(), waiter), pending_waits_.end());
            out.code = ready;
            out.timed_out = false;
            return out;
        }
    }
    out.code = lan ? send_message_to_printer(dev_id, json_str, qos, flag)
                   : send_message(dev_id, json_str, qos, flag);
    if (out.code != BAMBU_NETWORK_SUCCESS) {
        std::lock_guard<std::mutex> lk(wait_mtx_);
        pending_waits_.erase(std::remove(pending_waits_.begin(), pending_waits_.end(), waiter), pending_waits_.end());
        out.timed_out = false;
        if (out.code == BAMBU_NETWORK_ERR_CONNECT_FAILED)
            out.error = lan ? "LAN MQTT not connected; call connect_printer first"
                            : "cloud MQTT send failed; wait for server_connected then retry";
        return out;
    }
    if (timeout_ms <= 0) timeout_ms = 8000;
    {
        std::unique_lock<std::mutex> lk(wait_mtx_);
        out.timed_out = !waiter->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return waiter->done; });
        out.source = waiter->source;
        out.msg = waiter->msg;
        pending_waits_.erase(std::remove(pending_waits_.begin(), pending_waits_.end(), waiter), pending_waits_.end());
    }
    if (!out.timed_out && out.code == BAMBU_NETWORK_SUCCESS && out.msg.empty()) {
        out.msg = last_printer_msg(dev_id);
    }
    return out;
}

int BambuBridge::connect_printer(const std::string& dev_id, const std::string& dev_ip,
                                 const std::string& username, const std::string& password, bool use_ssl) {
    if (!has_agent() || !fn_connect_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_connect_printer(agent_, dev_id, dev_ip, username, password, use_ssl);
}
int BambuBridge::disconnect_printer() {
    if (!has_agent() || !fn_disconnect_printer) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_disconnect_printer(agent_);
}
bool BambuBridge::start_discovery(bool start, bool sending) {
    if (!has_agent() || !fn_start_discovery) return false;
    return fn_start_discovery(agent_, start, sending);
}
void BambuBridge::install_device_cert(const std::string& dev_id, bool lan_only) {
    if (!has_agent() || !fn_install_device_cert) return;
    fn_install_device_cert(agent_, dev_id, lan_only);
}
int BambuBridge::check_cert() {
    if (!has_agent() || !fn_check_cert) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return fn_check_cert(agent_);
}

int BambuBridge::get_my_token(const std::string& ticket, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_my_token) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_my_token(agent_, ticket, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_my_profile(const std::string& token, std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_my_profile) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_my_profile(agent_, token, &c, http_body); set_opt(http_code, c); return r;
}
int BambuBridge::get_user_print_info(std::string* http_body, unsigned* http_code) {
    if (!has_agent() || !fn_get_user_print_info) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    unsigned c = 0; int r = fn_get_user_print_info(agent_, &c, http_body); set_opt(http_code, c); return r;
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
int BambuBridge::get_camera_url(const std::string& dev_id, std::string* url) {
    if (!has_agent() || !fn_get_camera_url) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    struct Wait {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        std::string out;
    };
    auto w = std::make_shared<Wait>();
    int r = fn_get_camera_url(agent_, dev_id, [w](std::string u) {
        {
            std::lock_guard<std::mutex> lk(w->mu);
            w->out = std::move(u);
            w->done = true;
        }
        w->cv.notify_all();
    });
    if (r != 0) {
        if (url) *url = w->out;
        return r;
    }
    {
        std::unique_lock<std::mutex> lk(w->mu);
        if (!w->cv.wait_for(lk, std::chrono::seconds(15), [&] { return w->done; })) {
            if (url) *url = w->out;
            return BAMBU_NETWORK_ERR_TIMEOUT;
        }
        if (url) *url = w->out;
        return w->out.empty() ? BAMBU_NETWORK_ERR_INVALID_RESULT : BAMBU_NETWORK_SUCCESS;
    }
}

int BambuBridge::start_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                             std::function<bool()> cancel_cb) {
    if (!has_agent() || !fn_start_print) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    PrintParams p = decode_print_params(params_json);
    OnUpdateStatusFn update = [this, update_cb](int status, int code, std::string msg) {
        if (update_cb) update_cb(status, msg);
        Json ev = Json::object();
        ev.set("status", Json::n(status));
        ev.set("code", Json::n(code));
        ev.set("msg", Json::s(msg));
        emit("print_status", ev);
    };
    WasCancelledFn cancel = [cancel_cb]() -> bool { return cancel_cb ? cancel_cb() : false; };
    OnWaitFn wait = [](int, std::string) -> bool { return true; };
    return fn_start_print(agent_, p, update, cancel, wait);
}

int BambuBridge::start_local_print(const Json& params_json, std::function<void(int, std::string)> update_cb,
                                   std::function<bool()> cancel_cb) {
    if (!has_agent() || !fn_start_local_print) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    PrintParams p = decode_print_params(params_json);
    OnUpdateStatusFn update = [this, update_cb](int status, int code, std::string msg) {
        if (update_cb) update_cb(status, msg);
        Json ev = Json::object();
        ev.set("status", Json::n(status));
        ev.set("code", Json::n(code));
        ev.set("msg", Json::s(msg));
        emit("print_status", ev);
    };
    WasCancelledFn cancel = [cancel_cb]() -> bool { return cancel_cb ? cancel_cb() : false; };
    return fn_start_local_print(agent_, p, update, cancel);
}

void BambuBridge::set_event_sink(EventSink* s) {
    std::lock_guard<std::mutex> lk(sink_mtx_);
    sinks_.clear();
    if (s) sinks_.push_back(s);
}
void BambuBridge::add_event_sink(EventSink* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lk(sink_mtx_);
    for (auto* e : sinks_) if (e == s) return;
    sinks_.push_back(s);
}

void BambuBridge::emit(const std::string& event, const Json& payload) {
    std::vector<EventSink*> copy;
    {
        std::lock_guard<std::mutex> lk(sink_mtx_);
        copy = sinks_;
    }
    for (auto* s : copy) if (s) s->emit(event, payload);
}
void BambuBridge::emit_state(const std::string& state) {
    std::vector<EventSink*> copy;
    {
        std::lock_guard<std::mutex> lk(sink_mtx_);
        copy = sinks_;
    }
    for (auto* s : copy) if (s) s->on_state(state);
}

void BambuBridge::queue_on_main(std::function<void()> fn) {
    if (!fn) return;
    {
        std::lock_guard<std::mutex> lk(dispatch_mtx_);
        dispatch_q_.push(std::move(fn));
    }
    dispatch_cv_.notify_one();
}

void BambuBridge::start_dispatcher() {
    if (dispatch_running_) return;
    dispatch_running_ = true;
    dispatch_thread_ = std::thread([this]() {
        while (dispatch_running_) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(dispatch_mtx_);
                dispatch_cv_.wait_for(lk, std::chrono::milliseconds(200), [&] {
                    return !dispatch_q_.empty() || !dispatch_running_;
                });
                if (!dispatch_running_ && dispatch_q_.empty()) break;
                if (dispatch_q_.empty()) continue;
                fn = std::move(dispatch_q_.front());
                dispatch_q_.pop();
            }
            try { if (fn) fn(); } catch (...) {}
        }
    });
}

void BambuBridge::stop_dispatcher() {
    if (!dispatch_running_) return;
    dispatch_running_ = false;
    dispatch_cv_.notify_all();
    if (dispatch_thread_.joinable()) dispatch_thread_.join();
    std::lock_guard<std::mutex> lk(dispatch_mtx_);
    while (!dispatch_q_.empty()) dispatch_q_.pop();
}

void BambuBridge::install_callbacks() {
    if (!has_agent()) return;
    if (fn_set_queue_on_main_fn) {
        fn_set_queue_on_main_fn(agent_, [this](std::function<void()> fn) { queue_on_main(std::move(fn)); });
    }
    if (fn_set_get_country_code_fn) {
        fn_set_get_country_code_fn(agent_, [this]() { return country_code_; });
    }
    if (fn_set_on_user_login_fn) {
        fn_set_on_user_login_fn(agent_, [this](int online_login, bool login) {
            Json ev = Json::object();
            ev.set("online_login", Json::n(online_login));
            ev.set("login", Json::boolean(login));
            emit("on_user_login", ev);
            if (login) after_login();
        });
    }
    if (fn_set_on_server_connected_fn) {
        fn_set_on_server_connected_fn(agent_, [this](int return_code, int reason_code) {
            Json ev = Json::object();
            ev.set("return_code", Json::n(return_code));
            ev.set("reason_code", Json::n(reason_code));
            emit("on_server_connected", ev);
            if (return_code == 0 && is_user_login()) {
                std::string sel = get_user_selected_machine();
                if (!sel.empty()) set_user_selected_machine(sel);
            }
        });
    }
    if (fn_set_on_http_error_fn) {
        fn_set_on_http_error_fn(agent_, [this](unsigned http_code, std::string http_body) {
            Json ev = Json::object();
            ev.set("http_code", Json::n((double)http_code));
            ev.set("http_body", Json::s(http_body));
            emit("on_http_error", ev);
        });
    }
    if (fn_set_on_ssdp_msg_fn) {
        fn_set_on_ssdp_msg_fn(agent_, [this](std::string dev_info) {
            Json ev = Json::object();
            ev.set("dev_info", Json::s(dev_info));
            emit("on_ssdp_msg", ev);
        });
    }
    if (fn_set_on_printer_connected_fn) {
        fn_set_on_printer_connected_fn(agent_, [this](std::string topic) {
            Json ev = Json::object();
            ev.set("topic", Json::s(topic));
            emit("on_printer_connected", ev);
            std::string dev_id = topic;
            if (dev_id.rfind("tunnel/", 0) == 0) dev_id = dev_id.substr(7);
            if (!dev_id.empty()) {
                Json cmd = Json::object();
                Json pushing = Json::object();
                pushing.set("sequence_id", Json::s("0"));
                pushing.set("command", Json::s("pushall"));
                pushing.set("version", Json::n(1));
                pushing.set("push_target", Json::n(1));
                cmd.set("pushing", pushing);
                send_message(dev_id, json_dump(cmd), 0, 0);
                if (fn_install_device_cert) fn_install_device_cert(agent_, dev_id, false);
            }
        });
    }
    if (fn_set_on_subscribe_failure_fn) {
        fn_set_on_subscribe_failure_fn(agent_, [this](std::string topic) {
            Json ev = Json::object();
            ev.set("topic", Json::s(topic));
            emit("on_subscribe_failure", ev);
        });
    }
    if (fn_set_on_message_fn) {
        fn_set_on_message_fn(agent_, [this](std::string dev_id, std::string msg) {
            Json ev = Json::object();
            ev.set("dev_id", Json::s(dev_id));
            ev.set("msg", Json::s(msg));
            ev.set("source", Json::s("cloud"));
            note_printer_msg(dev_id, "cloud", msg);
            emit("on_message", ev);
        });
    }
    if (fn_set_on_user_message_fn) {
        fn_set_on_user_message_fn(agent_, [this](std::string user_id, std::string msg) {
            Json ev = Json::object();
            ev.set("user_id", Json::s(user_id));
            ev.set("msg", Json::s(msg));
            emit("on_user_message", ev);
        });
    }
    if (fn_set_on_local_connect_fn) {
        fn_set_on_local_connect_fn(agent_, [this](int status, std::string dev_id, std::string msg) {
            Json ev = Json::object();
            ev.set("status", Json::n(status));
            ev.set("dev_id", Json::s(dev_id));
            ev.set("msg", Json::s(msg));
            emit("on_local_connect", ev);
            if (status == 0 && !dev_id.empty()) {
                Json cmd = Json::object();
                Json pushing = Json::object();
                pushing.set("sequence_id", Json::s("0"));
                pushing.set("command", Json::s("pushall"));
                pushing.set("version", Json::n(1));
                pushing.set("push_target", Json::n(1));
                cmd.set("pushing", pushing);
                send_message_to_printer(dev_id, json_dump(cmd), 0, 0);
            }
        });
    }
    if (fn_set_on_local_message_fn) {
        fn_set_on_local_message_fn(agent_, [this](std::string dev_id, std::string msg) {
            Json ev = Json::object();
            ev.set("dev_id", Json::s(dev_id));
            ev.set("msg", Json::s(msg));
            ev.set("source", Json::s("lan"));
            note_printer_msg(dev_id, "lan", msg);
            emit("on_local_message", ev);
        });
    }
    if (fn_set_server_callback) {
        fn_set_server_callback(agent_, [this](std::string url, int status) {
            Json ev = Json::object();
            ev.set("url", Json::s(url));
            ev.set("status", Json::n(status));
            emit("on_server_error", ev);
        });
    }
}

void BambuBridge::apply_studio_http_headers() {
    if (!has_agent() || !fn_set_extra_http_header) return;
    std::map<std::string, std::string> h;
    h["X-BBL-Client-Type"] = "slicer";
    h["X-BBL-Client-Name"] = "BambuStudio";
    h["X-BBL-Client-Version"] = "02.05.02.51";
#if defined(_WIN32)
    h["X-BBL-OS-Type"] = "windows";
#elif defined(__APPLE__)
    h["X-BBL-OS-Type"] = "macos";
#else
    h["X-BBL-OS-Type"] = "linux";
#endif
    h["X-BBL-Language"] = (country_code_ == "CN") ? "zh-cn" : "en";
    fn_set_extra_http_header(agent_, h);
}

std::string BambuBridge::find_cert_folder() const {
    const char* candidates[] = {
        "resources/cert",
        "cert",
        "../OrcaSlicer-bambulab/resources/cert",
        "../../OrcaSlicer-bambulab/resources/cert",
        "../resources/cert",
    };
    auto has_cer = [](const std::string& folder) {
        return path_exists_local(join2(folder, "slicer_base64.cer"));
    };
    if (has_cer(join2(plugin_dir_, "cert"))) return join2(plugin_dir_, "cert");
    for (const char* rel : candidates) {
        std::string p = join2(plugin_dir_, rel);
        if (has_cer(p)) return p;
        if (has_cer(rel)) return rel;
    }
#ifdef _WIN32
    if (has_cer("E:\\bambu-nx\\OrcaSlicer-bambulab\\resources\\cert"))
        return "E:\\bambu-nx\\OrcaSlicer-bambulab\\resources\\cert";
#endif
    return {};
}

bool BambuBridge::resolve_symbols(std::string* err) {
    auto S = [&](const char* n) -> void* { return net_.resolve(n); };
    auto need = [&](void* p, const char* n) -> bool {
        if (!p) { if (err) *err = std::string("missing symbol: ") + n; return false; }
        return true;
    };

    fn_check_debug_consistent       = load_sym<func_check_debug_consistent>(S("bambu_network_check_debug_consistent"));
    fn_get_version                  = load_sym<func_get_version>(S("bambu_network_get_version"));
    fn_create_agent                 = load_sym<func_create_agent>(S("bambu_network_create_agent"));
    fn_destroy_agent                = load_sym<func_destroy_agent>(S("bambu_network_destroy_agent"));
    fn_init_log                     = load_sym<func_init_log>(S("bambu_network_init_log"));
    fn_set_config_dir               = load_sym<func_set_config_dir>(S("bambu_network_set_config_dir"));
    fn_set_cert_file                = load_sym<func_set_cert_file>(S("bambu_network_set_cert_file"));
    fn_set_country_code             = load_sym<func_set_country_code>(S("bambu_network_set_country_code"));
    fn_start                        = load_sym<func_start>(S("bambu_network_start"));
    fn_set_on_ssdp_msg_fn           = load_sym<func_set_on_ssdp_msg_fn>(S("bambu_network_set_on_ssdp_msg_fn"));
    fn_set_on_user_login_fn         = load_sym<func_set_on_user_login_fn>(S("bambu_network_set_on_user_login_fn"));
    fn_set_on_printer_connected_fn  = load_sym<func_set_on_printer_connected_fn>(S("bambu_network_set_on_printer_connected_fn"));
    fn_set_on_server_connected_fn   = load_sym<func_set_on_server_connected_fn>(S("bambu_network_set_on_server_connected_fn"));
    fn_set_on_http_error_fn         = load_sym<func_set_on_http_error_fn>(S("bambu_network_set_on_http_error_fn"));
    fn_set_get_country_code_fn      = load_sym<func_set_get_country_code_fn>(S("bambu_network_set_get_country_code_fn"));
    fn_set_on_subscribe_failure_fn  = load_sym<func_set_on_subscribe_failure_fn>(S("bambu_network_set_on_subscribe_failure_fn"));
    fn_set_on_message_fn            = load_sym<func_set_on_message_fn>(S("bambu_network_set_on_message_fn"));
    fn_set_on_user_message_fn       = load_sym<func_set_on_user_message_fn>(S("bambu_network_set_on_user_message_fn"));
    fn_set_on_local_connect_fn      = load_sym<func_set_on_local_connect_fn>(S("bambu_network_set_on_local_connect_fn"));
    fn_set_on_local_message_fn      = load_sym<func_set_on_local_message_fn>(S("bambu_network_set_on_local_message_fn"));
    fn_set_queue_on_main_fn         = load_sym<func_set_queue_on_main_fn>(S("bambu_network_set_queue_on_main_fn"));
    fn_connect_server               = load_sym<func_connect_server>(S("bambu_network_connect_server"));
    fn_is_server_connected          = load_sym<func_is_server_connected>(S("bambu_network_is_server_connected"));
    fn_refresh_connection           = load_sym<func_refresh_connection>(S("bambu_network_refresh_connection"));
    fn_start_subscribe              = load_sym<func_start_subscribe>(S("bambu_network_start_subscribe"));
    fn_stop_subscribe               = load_sym<func_stop_subscribe>(S("bambu_network_stop_subscribe"));
    fn_add_subscribe                = load_sym<func_add_subscribe>(S("bambu_network_add_subscribe"));
    fn_del_subscribe                = load_sym<func_del_subscribe>(S("bambu_network_del_subscribe"));
    fn_enable_multi_machine         = load_sym<func_enable_multi_machine>(S("bambu_network_enable_multi_machine"));
    fn_send_message                 = load_sym<func_send_message>(S("bambu_network_send_message"));
    fn_send_message_to_printer      = load_sym<func_send_message_to_printer>(S("bambu_network_send_message_to_printer"));
    fn_connect_printer              = load_sym<func_connect_printer>(S("bambu_network_connect_printer"));
    fn_disconnect_printer           = load_sym<func_disconnect_printer>(S("bambu_network_disconnect_printer"));
    fn_check_cert                   = load_sym<func_check_cert>(S("bambu_network_update_cert"));
    fn_install_device_cert          = load_sym<func_install_device_cert>(S("bambu_network_install_device_cert"));
    fn_start_discovery              = load_sym<func_start_discovery>(S("bambu_network_start_discovery"));
    fn_is_user_login                = load_sym<func_is_user_login>(S("bambu_network_is_user_login"));
    fn_user_logout                  = load_sym<func_user_logout>(S("bambu_network_user_logout"));
    fn_get_user_id                  = load_sym<func_get_user_id>(S("bambu_network_get_user_id"));
    fn_get_user_name                = load_sym<func_get_user_name>(S("bambu_network_get_user_name"));
    fn_get_user_avatar              = load_sym<func_get_user_avatar>(S("bambu_network_get_user_avatar"));
    fn_get_user_nickanme            = load_sym<func_get_user_nickanme>(S("bambu_network_get_user_nickanme"));
    fn_get_bambulab_host            = load_sym<func_get_bambulab_host>(S("bambu_network_get_bambulab_host"));
    fn_get_user_selected_machine    = load_sym<func_get_user_selected_machine>(S("bambu_network_get_user_selected_machine"));
    fn_set_user_selected_machine    = load_sym<func_set_user_selected_machine>(S("bambu_network_set_user_selected_machine"));
    fn_get_studio_info_url          = load_sym<func_get_studio_info_url>(S("bambu_network_get_studio_info_url"));
    fn_change_user                  = load_sym<func_change_user>(S("bambu_network_change_user"));
    fn_build_login_cmd              = load_sym<func_build_login_cmd>(S("bambu_network_build_login_cmd"));
    fn_build_logout_cmd             = load_sym<func_build_logout_cmd>(S("bambu_network_build_logout_cmd"));
    fn_build_login_info             = load_sym<func_build_login_info>(S("bambu_network_build_login_info"));
    fn_set_server_callback          = load_sym<func_set_server_callback>(S("bambu_network_set_server_callback"));
    fn_set_extra_http_header        = load_sym<func_set_extra_http_header>(S("bambu_network_set_extra_http_header"));
    fn_get_user_print_info          = load_sym<func_get_user_print_info>(S("bambu_network_get_user_print_info"));
    fn_get_my_token                 = load_sym<func_get_my_token>(S("bambu_network_get_my_token"));
    fn_get_my_profile               = load_sym<func_get_my_profile>(S("bambu_network_get_my_profile"));
    fn_get_user_tasks               = load_sym<func_get_user_tasks>(S("bambu_network_get_user_tasks"));
    fn_get_printer_firmware         = load_sym<func_get_printer_firmware>(S("bambu_network_get_printer_firmware"));
    fn_get_slice_info               = load_sym<func_get_slice_info>(S("bambu_network_get_slice_info"));
    fn_get_subtask_info             = load_sym<func_get_subtask_info>(S("bambu_network_get_subtask_info"));
    fn_get_model_mall_detail_url    = load_sym<func_get_model_mall_detail_url>(S("bambu_network_get_model_mall_detail_url"));
    fn_get_camera_url               = load_sym<func_get_camera_url>(S("bambu_network_get_camera_url"));
    fn_start_print                  = load_sym<func_start_print>(S("bambu_network_start_print"));
    fn_start_local_print            = load_sym<func_start_local_print>(S("bambu_network_start_local_print"));

    if (!need((void*)fn_create_agent, "bambu_network_create_agent")) return false;
    if (!need((void*)fn_destroy_agent, "bambu_network_destroy_agent")) return false;
    if (!need((void*)fn_start, "bambu_network_start")) return false;
    return true;
}

} // namespace bambu
