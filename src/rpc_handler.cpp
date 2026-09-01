#include "rpc_handler.hpp"

#include <string>
#include <vector>

namespace bambu {

namespace {

// Extract a string param with default.
std::string str_param(const Json& p, const char* k, const std::string& d = "") {
    const Json* v = p.find(k);
    return (v && v->kind == Json::String) ? v->str : d;
}
// Extract a number/int param with default.
double num_param(const Json& p, const char* k, double d = 0.0) {
    const Json* v = p.find(k);
    return (v && v->kind == Json::Number) ? v->num : d;
}
// Extract a bool param with default.
bool bool_param(const Json& p, const char* k, bool d = false) {
    const Json* v = p.find(k);
    return (v && v->kind == Json::Bool) ? v->b : d;
}
// Extract a string array param.
std::vector<std::string> str_array_param(const Json& p, const char* k) {
    std::vector<std::string> out;
    const Json* v = p.find(k);
    if (!v || v->kind != Json::Array) return out;
    for (const auto& e : v->arr)
        if (e.kind == Json::String) out.push_back(e.str);
    return out;
}

Json resp_ok(Json result) {
    Json r = Json::object();
    r.set("ok", Json::boolean(true));
    r.set("result", std::move(result));
    return r;
}
Json resp_err(const std::string& e) {
    Json r = Json::object();
    r.set("ok", Json::boolean(false));
    r.set("error", Json::s(e));
    return r;
}
Json JsonNumber(double d) { return Json::n(d); }
Json JsonStr(const std::string& s) { return Json::s(s); }
Json JsonBool(bool b) { return Json::boolean(b); }

} // namespace

Json handle_request(BambuBridge& bridge, const Json& request) {
    const Json* method = request.find("method");
    const Json* params = request.find("params");
    if (!method || method->kind != Json::String)
        return resp_err("request missing string 'method'");

    const std::string m = method->str;
    Json p = params ? *params : Json::object();

    try {
        // ---- lifecycle ----
        if (m == "load_plugin") {           // bridge is created per-process; this is informational
            return resp_ok(Json::object());
        }
        if (m == "create_agent") {
            return JsonNumber(bridge.create_agent(str_param(p, "log_dir")));
        }
        if (m == "destroy_agent") {
            return JsonNumber(bridge.destroy_agent());
        }
        if (m == "version") {
            return JsonStr(bridge.version());
        }

        // ---- plugin setup ----
        if (m == "set_country_code")  { return JsonNumber(bridge.set_country_code(str_param(p, "country_code"))); }
        if (m == "set_config_dir")    { return JsonNumber(bridge.set_config_dir(str_param(p, "config_dir"))); }
        if (m == "set_cert_file")     { return JsonNumber(bridge.set_cert_file(str_param(p, "folder"), str_param(p, "filename"))); }
        if (m == "start")             { return JsonNumber(bridge.start()); }

        // ---- login / server ----
        if (m == "connect_server")    { return JsonNumber(bridge.connect_server()); }
        if (m == "is_server_connected") { return JsonBool(bridge.is_server_connected()); }
        if (m == "refresh_connection")  { return JsonNumber(bridge.refresh_connection()); }
        if (m == "is_user_login")       { return JsonBool(bridge.is_user_login()); }
        if (m == "user_logout")         { return JsonNumber(bridge.user_logout(bool_param(p, "request", true))); }
        if (m == "get_user_id")         { return JsonStr(bridge.get_user_id()); }
        if (m == "get_user_name")       { return JsonStr(bridge.get_user_name()); }
        if (m == "get_user_avatar")     { return JsonStr(bridge.get_user_avatar()); }
        if (m == "get_bambulab_host")   { return JsonStr(bridge.get_bambulab_host()); }
        if (m == "get_user_selected_machine") { return JsonStr(bridge.get_user_selected_machine()); }
        if (m == "set_user_selected_machine") { return JsonNumber(bridge.set_user_selected_machine(str_param(p, "dev_id"))); }
        if (m == "get_studio_info_url") { return JsonStr(bridge.get_studio_info_url()); }

        // ---- subscribe / MQTT ----
        if (m == "add_subscribe") {
            return JsonNumber(bridge.add_subscribe(str_array_param(p, "dev_list")));
        }
        if (m == "del_subscribe") {
            return JsonNumber(bridge.del_subscribe(str_array_param(p, "dev_list")));
        }
        if (m == "send_message") {
            return JsonNumber(bridge.send_message(str_param(p, "dev_id"), str_param(p, "json_str"), (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0)));
        }
        if (m == "send_message_to_printer") {
            return JsonNumber(bridge.send_message_to_printer(str_param(p, "dev_id"), str_param(p, "json_str"), (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0)));
        }

        // ---- printers ----
        if (m == "connect_printer") {
            return JsonNumber(bridge.connect_printer(str_param(p, "dev_id"), str_param(p, "dev_ip"),
                                                     str_param(p, "username"), str_param(p, "password"),
                                                     bool_param(p, "use_ssl", true)));
        }
        if (m == "disconnect_printer") { return JsonNumber(bridge.disconnect_printer()); }

        // ---- cloud read ----
        if (m == "get_my_token") {
            std::string body; unsigned code = 0;
            int r = bridge.get_my_token(str_param(p, "ticket"), &body, &code);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("http_code", JsonNumber((double)code));
            res.set("http_body", JsonStr(body));
            return resp_ok(res);
        }
        if (m == "get_my_profile") {
            std::string body; unsigned code = 0;
            int r = bridge.get_my_profile(str_param(p, "token"), &body, &code);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("http_code", JsonNumber((double)code));
            res.set("http_body", JsonStr(body));
            return resp_ok(res);
        }
        if (m == "get_user_tasks") {
            std::string body;
            int r = bridge.get_user_tasks(str_param(p, "dev_id"), (int)num_param(p, "status", 0),
                                          (int)num_param(p, "offset", 0), (int)num_param(p, "limit", 20), &body);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("http_body", JsonStr(body));
            return resp_ok(res);
        }
        if (m == "get_printer_firmware") {
            std::string body; unsigned code = 0;
            int r = bridge.get_printer_firmware(str_param(p, "dev_id"), &body, &code);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("http_code", JsonNumber((double)code));
            res.set("http_body", JsonStr(body));
            return resp_ok(res);
        }
        if (m == "get_slice_info") {
            std::string slice;
            int r = bridge.get_slice_info(str_param(p, "project_id"), str_param(p, "profile_id"),
                                          (int)num_param(p, "plate_index", 0), &slice);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("slice_json", JsonStr(slice));
            return resp_ok(res);
        }
        if (m == "get_subtask_info") {
            std::string tjson, body; unsigned code = 0;
            int r = bridge.get_subtask_info(str_param(p, "subtask_id"), &tjson, &body, &code);
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("task_json", JsonStr(tjson));
            res.set("http_code", JsonNumber((double)code));
            res.set("http_body", JsonStr(body));
            return resp_ok(res);
        }
        if (m == "get_model_mall_detail_url") {
            std::string url;
            int r = bridge.get_model_mall_detail_url(&url, str_param(p, "id"));
            Json res = Json::object();
            res.set("code", JsonNumber(r));
            res.set("url", JsonStr(url));
            return resp_ok(res);
        }

        // ---- print ----
        if (m == "start_print") {
            const Json* params_obj = params;
            Json pp = params_obj ? *params_obj : Json::object();
            auto update = [](int, std::string) { /* events sink via bridge */ };
            auto cancel = []() -> bool { return false; };
            int r = bridge.start_print(pp, update, cancel);
            return JsonNumber(r);
        }
        if (m == "start_local_print") {
            const Json* params_obj = params;
            Json pp = params_obj ? *params_obj : Json::object();
            auto update = [](int, std::string) {};
            auto cancel = []() -> bool { return false; };
            int r = bridge.start_local_print(pp, update, cancel);
            return JsonNumber(r);
        }

        return resp_err("unknown method: " + m);
    } catch (const std::exception& e) {
        return resp_err(std::string("exception: ") + e.what());
    } catch (...) {
        return resp_err("unknown exception");
    }
}

} // namespace bambu
