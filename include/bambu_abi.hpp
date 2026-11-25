#ifndef BAMBU_BRIDGE_BAMBU_ABI_HPP
#define BAMBU_BRIDGE_BAMBU_ABI_HPP

// ============================================================================
// bambu_abi.hpp
//
// C++ ABI surface of the closed-source Bambu networking plugin
// (bambu_networking.dll / libbambu_networking.so).
//
// This is extracted from OrcaSlicer's BBLNetworkPlugin.hpp and
// bamboo_networking.hpp. It is the *call* ABI we must satisfy to invoke the
// plugin through dlopen()/LoadLibrary() + GetProcAddress()/dlsym().
//
// IMPORTANT ABI NOTES
// -------------------
// * The exported symbols are `extern "C"` (no name mangling) and can be
//   resolved by name.  The *function signatures*, however, use C++ types:
//   std::string, std::map, std::vector, std::function, and C++ structs that
//   themselves contain std::string members.
//
// * Consequently the bridge MUST be built with a C++ compiler whose
//   std::string / std::function / std::map ABI matches the plugin's own build.
//     - Linux  (.so, built by GCC/Clang):  build the host with g++/clang++ (libstdc++).  OK.
//     - Windows (DLL built by MSVC):      build the host with MSVC.  MinGW (w64devkit) will NOT
//       be ABI-compatible for C++ types passed across the boundary.
//
// * You only need to "see" these typedefs to call the plugin.  Do not add extra
//   members to the structs -- layout is defined by the plugin.
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <functional>

// ----------------------------------------------------------------------------
// Error codes (from bamboo_networking.hpp)
// ----------------------------------------------------------------------------
#define BAMBU_NETWORK_SUCCESS                      0
#define BAMBU_NETWORK_ERR_INVALID_HANDLE          -1
#define BAMBU_NETWORK_ERR_CONNECT_FAILED          -2
#define BAMBU_NETWORK_ERR_SEND_MSG_FAILED         -4
#define BAMBU_NETWORK_ERR_TIMEOUT                -17
#define BAMBU_NETWORK_ERR_CANCELED               -18
#define BAMBU_NETWORK_ERR_INVALID_RESULT         -19

enum class MessageFlag : int {
    MSG_FLAG_NONE = 0,
    MSG_SIGN      = 1 << 0,
    MSG_ENCRYPT   = 1 << 1,
};

// ----------------------------------------------------------------------------
// Callback signatures (std::function) expected by the plugin.
// We keep the real C++ types here so the host can match the plugin's ABI.
// ----------------------------------------------------------------------------
typedef std::function<void(int online_login, bool login)>              OnUserLoginFn;
typedef std::function<void(std::string topic_str)>                     OnPrinterConnectedFn;
typedef std::function<void(int status, std::string dev_id, std::string msg)> OnLocalConnectedFn;
typedef std::function<void(int return_code, int reason_code)>          OnServerConnectedFn;
typedef std::function<void(std::string dev_id, std::string msg)>       OnMessageFn;
typedef std::function<void(unsigned http_code, std::string http_body)> OnHttpErrorFn;
typedef std::function<std::string()>                                   GetCountryCodeFn;
typedef std::function<void(std::string topic)>                         GetSubscribeFailureFn;
typedef std::function<void(int status, int code, std::string msg)>     OnUpdateStatusFn;
typedef std::function<bool()>                                          WasCancelledFn;
typedef std::function<bool(int status, std::string job_info)>          OnWaitFn;
typedef std::function<void(std::string dev_info_json_str)>             OnMsgArrivedFn;
typedef std::function<void(std::function<void()>)>                     QueueOnMainFn;
typedef std::function<void(int progress)>                              ProgressFn;
typedef std::function<void(int retcode, std::string info)>             LoginFn;
typedef std::function<void(int result, std::string info)>              ResultFn;
typedef std::function<bool()>                                          CancelFn;
typedef std::function<bool(std::map<std::string, std::string> info)>   CheckFn;
typedef std::function<void(std::string url, int status)>               OnServerErrFn;

// ----------------------------------------------------------------------------
// Structs used as parameters (layout defined by plugin, do not reorder).
// ----------------------------------------------------------------------------
struct detectResult {
    std::string result_msg;
    std::string command;
    std::string dev_id;
    std::string model_id;
    std::string dev_name;
    std::string version;
    std::string bind_state;
    std::string connect_type;
};

// By-value layout for the bundled, unversioned Open Bamboo 02.08.04 ABI.
struct PrintParams {
    std::string dev_id;
    std::string task_name;
    std::string project_name;
    std::string preset_name;
    std::string filename;
    std::string config_filename;
    int plate_index;
    std::string ftp_folder;
    std::string ftp_file;
    std::string ftp_file_md5;
    std::string nozzle_mapping;
    std::string ams_mapping;
    std::string ams_mapping2;
    std::string ams_mapping_info;
    std::string nozzles_info;
    std::string connection_type;
    std::string comments;
    int origin_profile_id = 0;
    int stl_design_id = 0;
    std::string origin_model_id;
    std::string print_type;
    std::string dst_file;
    std::string dev_name;
    std::string dev_ip;
    bool use_ssl_for_ftp;
    bool use_ssl_for_mqtt;
    std::string username;
    std::string password;
    bool task_bed_leveling;
    bool task_flow_cali;
    bool task_vibration_cali;
    bool task_layer_inspect;
    bool task_record_timelapse;
    bool task_timelapse_use_internal;
    bool task_use_ams;
    std::string task_bed_type;
    std::string extra_options;
    int auto_bed_leveling{0};
    int auto_flow_cali{0};
    int auto_offset_cali{0};
    int extruder_cali_manual_mode{-1};
    bool task_ext_change_assist;
    bool try_emmc_print;
    std::string svc_context;
    std::string slicer_uid;
    std::string queue_plate_id;
};

struct TaskQueryParams {
    std::string dev_id;
    int status = 0;
    int offset = 0;
    int limit = 20;
};

struct PublishParams {
    std::string project_name;
    std::string project_3mf_file;
    std::string preset_name;
    std::string project_model_id;
    std::string design_id;
    std::string config_filename;
};

// ----------------------------------------------------------------------------
// Function-pointer typedefs (the plugin's exported callable entries).
// Names match exactly the exports used by GetProcAddress/dlsym.
// ----------------------------------------------------------------------------
typedef bool (*func_check_debug_consistent)(bool is_debug);
typedef std::string (*func_get_version)(void);
typedef void* (*func_create_agent)(std::string log_dir);
typedef int (*func_destroy_agent)(void* agent);
typedef int (*func_init_log)(void* agent);
typedef int (*func_set_config_dir)(void* agent, std::string config_dir);
typedef int (*func_set_cert_file)(void* agent, std::string folder, std::string filename);
typedef int (*func_set_country_code)(void* agent, std::string country_code);
typedef int (*func_start)(void* agent);
typedef int (*func_set_on_ssdp_msg_fn)(void* agent, OnMsgArrivedFn fn);
typedef int (*func_set_on_user_login_fn)(void* agent, OnUserLoginFn fn);
typedef int (*func_set_on_printer_connected_fn)(void* agent, OnPrinterConnectedFn fn);
typedef int (*func_set_on_server_connected_fn)(void* agent, OnServerConnectedFn fn);
typedef int (*func_set_on_http_error_fn)(void* agent, OnHttpErrorFn fn);
typedef int (*func_set_get_country_code_fn)(void* agent, GetCountryCodeFn fn);
typedef int (*func_set_on_subscribe_failure_fn)(void* agent, GetSubscribeFailureFn fn);
typedef int (*func_set_on_message_fn)(void* agent, OnMessageFn fn);
typedef int (*func_set_on_user_message_fn)(void* agent, OnMessageFn fn);
typedef int (*func_set_on_local_connect_fn)(void* agent, OnLocalConnectedFn fn);
typedef int (*func_set_on_local_message_fn)(void* agent, OnMessageFn fn);
typedef int (*func_set_queue_on_main_fn)(void* agent, QueueOnMainFn fn);
typedef int (*func_connect_server)(void* agent);
typedef bool (*func_is_server_connected)(void* agent);
typedef int (*func_refresh_connection)(void* agent);
typedef int (*func_start_subscribe)(void* agent, std::string module);
typedef int (*func_stop_subscribe)(void* agent, std::string module);
typedef int (*func_add_subscribe)(void* agent, std::vector<std::string> dev_list);
typedef int (*func_del_subscribe)(void* agent, std::vector<std::string> dev_list);
typedef void (*func_enable_multi_machine)(void* agent, bool enable);
typedef int (*func_send_message)(void* agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_connect_printer)(void* agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
typedef int (*func_disconnect_printer)(void* agent);
typedef int (*func_send_message_to_printer)(void* agent, std::string dev_id, std::string json_str, int qos, int flag);
typedef int (*func_check_cert)(void* agent);
typedef void (*func_install_device_cert)(void* agent, std::string dev_id, bool lan_only);
typedef bool (*func_start_discovery)(void* agent, bool start, bool sending);
typedef int (*func_change_user)(void* agent, std::string user_info);
typedef bool (*func_is_user_login)(void* agent);
typedef int (*func_user_logout)(void* agent, bool request);
typedef std::string (*func_get_user_id)(void* agent);
typedef std::string (*func_get_user_name)(void* agent);
typedef std::string (*func_get_user_avatar)(void* agent);
typedef std::string (*func_get_user_nickanme)(void* agent);
typedef std::string (*func_build_login_cmd)(void* agent);
typedef std::string (*func_build_logout_cmd)(void* agent);
typedef std::string (*func_build_login_info)(void* agent);
typedef int (*func_ping_bind)(void* agent, std::string ping_code);
typedef int (*func_bind_detect)(void* agent, std::string dev_ip, std::string sec_link, detectResult& detect);
typedef int (*func_set_server_callback)(void* agent, OnServerErrFn fn);
typedef int (*func_bind)(void* agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
typedef int (*func_unbind)(void* agent, std::string dev_id);
typedef std::string (*func_get_bambulab_host)(void* agent);
typedef std::string (*func_get_user_selected_machine)(void* agent);
typedef int (*func_set_user_selected_machine)(void* agent, std::string dev_id);
typedef int (*func_start_print)(void* agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print)(void* agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_get_user_presets)(void* agent, std::map<std::string, std::map<std::string, std::string>>* user_presets);
typedef std::string (*func_request_setting_id)(void* agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_put_setting)(void* agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code);
typedef int (*func_get_setting_list)(void* agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn);
typedef int (*func_delete_setting)(void* agent, std::string setting_id);
typedef std::string (*func_get_studio_info_url)(void* agent);
typedef int (*func_set_extra_http_header)(void* agent, std::map<std::string, std::string> extra_headers);
typedef int (*func_get_my_message)(void* agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body);
typedef int (*func_check_user_task_report)(void* agent, int* task_id, bool* printable);
typedef int (*func_get_user_print_info)(void* agent, unsigned int* http_code, std::string* http_body);
typedef int (*func_get_user_tasks)(void* agent, TaskQueryParams params, std::string* http_body);
typedef int (*func_get_printer_firmware)(void* agent, std::string dev_id, unsigned* http_code, std::string* http_body);
typedef int (*func_get_task_plate_index)(void* agent, std::string task_id, int* plate_index);
typedef int (*func_get_user_info)(void* agent, int* identifier);
typedef int (*func_request_bind_ticket)(void* agent, std::string* ticket);
typedef int (*func_get_subtask_info)(void* agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body);
typedef int (*func_get_slice_info)(void* agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json);
typedef int (*func_query_bind_status)(void* agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body);
typedef int (*func_modify_printer_name)(void* agent, std::string dev_id, std::string dev_name);
typedef int (*func_get_camera_url)(void* agent, std::string dev_id, std::function<void(std::string)> callback);
typedef int (*func_get_camera_url_for_golive)(void* agent, std::string dev_id, std::string sdev_id, std::function<void(std::string)> callback);
typedef int (*func_get_design_staffpick)(void* agent, int offset, int limit, std::function<void(std::string)> callback);
typedef int (*func_get_model_publish_url)(void* agent, std::string* url);
typedef int (*func_get_model_mall_home_url)(void* agent, std::string* url);
typedef int (*func_get_model_mall_detail_url)(void* agent, std::string* url, std::string id);
typedef int (*func_get_my_token)(void* agent, std::string ticket, unsigned int* http_code, std::string* http_body);
typedef int (*func_get_my_profile)(void* agent, std::string token, unsigned int* http_code, std::string* http_body);
typedef int (*func_track_enable)(void* agent, bool enable);
typedef int (*func_track_remove_files)(void* agent);
typedef int (*func_track_event)(void* agent, std::string evt_key, std::string content);
typedef int (*func_track_header)(void* agent, std::string header);
typedef int (*func_track_update_property)(void* agent, std::string name, std::string value, std::string type);
typedef int (*func_track_get_property)(void* agent, std::string name, std::string& value, std::string type);

// ----------------------------------------------------------------------------
// Legacy (older) ABI variants, useful if the plugin is a legacy build.
// ----------------------------------------------------------------------------
typedef int (*func_send_message_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);
typedef int (*func_send_message_to_printer_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);

#endif // BAMBU_BRIDGE_BAMBU_ABI_HPP
