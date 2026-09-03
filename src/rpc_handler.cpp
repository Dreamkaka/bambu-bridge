#include "rpc_handler.hpp"
#include "http_server.hpp"
#include "json.hpp"

#include <string>
#include <vector>

namespace bambu {

namespace {

std::string str_param(const Json& p, const char* k, const std::string& d = "") {
    const Json* v = p.find(k);
    return (v && v->kind == Json::String) ? v->str : d;
}
double num_param(const Json& p, const char* k, double d = 0.0) {
    const Json* v = p.find(k);
    return (v && v->kind == Json::Number) ? v->num : d;
}
bool bool_param(const Json& p, const char* k, bool d = false) {
    const Json* v = p.find(k);
    if (!v) return d;
    if (v->kind == Json::Bool) return v->b;
    if (v->kind == Json::Number) return v->num != 0;
    return d;
}
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
Json ok_num(double d) { return resp_ok(Json::n(d)); }
Json ok_str(const std::string& s) { return resp_ok(Json::s(s)); }
Json ok_bool(bool b) { return resp_ok(Json::boolean(b)); }

Json http_result(int r, unsigned code, const std::string& body, const char* body_key = "http_body") {
    Json res = Json::object();
    res.set("code", Json::n(r));
    res.set("http_code", Json::n((double)code));
    res.set(body_key, Json::s(body));
    return resp_ok(res);
}

} // namespace

Json handle_request(BambuBridge& bridge, const Json& request) {
    const Json* method = request.find("method");
    const Json* params = request.find("params");
    if (!method || method->kind != Json::String)
        return resp_err("request missing string 'method'");

    const std::string m = method->str;
    Json p = params ? *params : Json::object();

    try {
        if (m == "load_plugin") return resp_ok(Json::object());
        if (m == "init_agent") {
            std::string err;
            int r = bridge.init_agent(&err);
            Json res = Json::object();
            res.set("code", Json::n(r));
            if (r != 0) res.set("error", Json::s(err));
            return resp_ok(res);
        }
        if (m == "create_agent") return ok_num(bridge.create_agent(str_param(p, "log_dir")));
        if (m == "destroy_agent") return ok_num(bridge.destroy_agent());
        if (m == "version") return ok_str(bridge.version());
        if (m == "status") {
            Json res = Json::object();
            res.set("loaded", Json::boolean(bridge.is_loaded()));
            res.set("agent_ready", Json::boolean(bridge.agent_ready()));
            res.set("version", Json::s(bridge.version()));
            res.set("logged_in", Json::boolean(bridge.is_user_login()));
            res.set("server_connected", Json::boolean(bridge.is_server_connected()));
            res.set("user_id", Json::s(bridge.get_user_id()));
            res.set("user_name", Json::s(bridge.get_user_name()));
            res.set("country", Json::s(bridge.country_code()));
            res.set("host", Json::s(bridge.get_host_for_login()));
            return resp_ok(res);
        }

        if (m == "set_country_code") return ok_num(bridge.set_country_code(str_param(p, "country_code")));
        if (m == "set_config_dir") return ok_num(bridge.set_config_dir(str_param(p, "config_dir")));
        if (m == "set_cert_file") return ok_num(bridge.set_cert_file(str_param(p, "folder"), str_param(p, "filename")));
        if (m == "init_log") return ok_num(bridge.init_log());
        if (m == "start") return ok_num(bridge.start());

        if (m == "connect_server") return ok_num(bridge.connect_server());
        if (m == "is_server_connected") return ok_bool(bridge.is_server_connected());
        if (m == "refresh_connection") return ok_num(bridge.refresh_connection());
        if (m == "is_user_login") return ok_bool(bridge.is_user_login());
        if (m == "build_login_cmd") return ok_str(bridge.build_login_cmd());
        if (m == "build_login_info") return ok_str(bridge.build_login_info());
        if (m == "build_logout_cmd") return ok_str(bridge.build_logout_cmd());
        if (m == "user_logout") return ok_num(bridge.user_logout(bool_param(p, "request", true)));
        if (m == "change_user") {
            std::string info = str_param(p, "user_info");
            if (info.empty() && p.kind == Json::Object) info = json_dump(p);
            return ok_num(bridge.change_user(info));
        }
        if (m == "login_with_ticket") {
            std::string err;
            int r = bridge.login_with_ticket(str_param(p, "ticket"), &err);
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("logged_in", Json::boolean(bridge.is_user_login()));
            res.set("user_id", Json::s(bridge.get_user_id()));
            res.set("user_name", Json::s(bridge.get_user_name()));
            if (r != 0) res.set("error", Json::s(err));
            return resp_ok(res);
        }
        if (m == "get_user_id") return ok_str(bridge.get_user_id());
        if (m == "get_user_name") return ok_str(bridge.get_user_name());
        if (m == "get_user_avatar") return ok_str(bridge.get_user_avatar());
        if (m == "get_user_nickname") return ok_str(bridge.get_user_nickname());
        if (m == "get_bambulab_host") return ok_str(bridge.get_bambulab_host());
        if (m == "get_login_url") {
            std::string host = bridge.get_host_for_login();
            int port = http_listen_port();
            std::string locale = (bridge.country_code() == "CN") ? "zh-CN" : "en";
            Json res = Json::object();
            res.set("host", Json::s(host));
            res.set("url", Json::s(port > 0 ? build_login_url(host, port, locale) : host + "/sign-in"));
            return resp_ok(res);
        }
        if (m == "get_user_selected_machine") return ok_str(bridge.get_user_selected_machine());
        if (m == "set_user_selected_machine") return ok_num(bridge.set_user_selected_machine(str_param(p, "dev_id")));
        if (m == "get_studio_info_url") return ok_str(bridge.get_studio_info_url());

        if (m == "start_subscribe") return ok_num(bridge.start_subscribe(str_param(p, "module", "app")));
        if (m == "stop_subscribe") return ok_num(bridge.stop_subscribe(str_param(p, "module", "app")));
        if (m == "add_subscribe") return ok_num(bridge.add_subscribe(str_array_param(p, "dev_list")));
        if (m == "del_subscribe") return ok_num(bridge.del_subscribe(str_array_param(p, "dev_list")));
        if (m == "enable_multi_machine") {
            bridge.enable_multi_machine(bool_param(p, "enable", true));
            return ok_num(0);
        }
        if (m == "send_message") {
            return ok_num(bridge.send_message(str_param(p, "dev_id"), str_param(p, "json_str"),
                                              (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0)));
        }
        if (m == "send_message_to_printer") {
            return ok_num(bridge.send_message_to_printer(str_param(p, "dev_id"), str_param(p, "json_str"),
                                                         (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0)));
        }
        if (m == "printer_command") {
            return ok_num(bridge.printer_command(str_param(p, "dev_id"), str_param(p, "json_str"),
                                                 bool_param(p, "lan", false),
                                                 (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0)));
        }
        if (m == "pushall") {
            std::string dev = str_param(p, "dev_id");
            bool lan = bool_param(p, "lan", false);
            Json cmd = Json::object();
            Json pushing = Json::object();
            pushing.set("sequence_id", Json::s("0"));
            pushing.set("command", Json::s("pushall"));
            pushing.set("version", Json::n(1));
            pushing.set("push_target", Json::n(1));
            cmd.set("pushing", pushing);
            return ok_num(bridge.printer_command(dev, json_dump(cmd), lan, 0, 0));
        }
        if (m == "pause" || m == "resume" || m == "stop") {
            std::string cmd_name = (m == "stop") ? "stop" : m;
            Json cmd = Json::object();
            Json print = Json::object();
            print.set("sequence_id", Json::s("0"));
            print.set("command", Json::s(cmd_name));
            cmd.set("print", print);
            return ok_num(bridge.printer_command(str_param(p, "dev_id"), json_dump(cmd),
                                                 bool_param(p, "lan", false), 0, 0));
        }

        if (m == "connect_printer") {
            return ok_num(bridge.connect_printer(str_param(p, "dev_id"), str_param(p, "dev_ip"),
                                                 str_param(p, "username", "bblp"), str_param(p, "password"),
                                                 bool_param(p, "use_ssl", true)));
        }
        if (m == "disconnect_printer") return ok_num(bridge.disconnect_printer());
        if (m == "start_discovery") return ok_bool(bridge.start_discovery(bool_param(p, "start", true), bool_param(p, "sending", false)));
        if (m == "install_device_cert") {
            bridge.install_device_cert(str_param(p, "dev_id"), bool_param(p, "lan_only", false));
            return ok_num(0);
        }

        if (m == "get_my_token") {
            std::string body; unsigned code = 0;
            int r = bridge.get_my_token(str_param(p, "ticket"), &body, &code);
            return http_result(r, code, body);
        }
        if (m == "get_my_profile") {
            std::string body; unsigned code = 0;
            int r = bridge.get_my_profile(str_param(p, "token"), &body, &code);
            return http_result(r, code, body);
        }
        if (m == "get_user_print_info" || m == "list_printers") {
            std::string body; unsigned code = 0;
            int r = bridge.get_user_print_info(&body, &code);
            return http_result(r, code, body);
        }
        if (m == "get_user_tasks") {
            std::string body;
            int r = bridge.get_user_tasks(str_param(p, "dev_id"), (int)num_param(p, "status", 0),
                                          (int)num_param(p, "offset", 0), (int)num_param(p, "limit", 20), &body);
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("http_body", Json::s(body));
            return resp_ok(res);
        }
        if (m == "get_printer_firmware") {
            std::string body; unsigned code = 0;
            int r = bridge.get_printer_firmware(str_param(p, "dev_id"), &body, &code);
            return http_result(r, code, body);
        }
        if (m == "get_slice_info") {
            std::string slice;
            int r = bridge.get_slice_info(str_param(p, "project_id"), str_param(p, "profile_id"),
                                          (int)num_param(p, "plate_index", 0), &slice);
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("slice_json", Json::s(slice));
            return resp_ok(res);
        }
        if (m == "get_subtask_info") {
            std::string tjson, body; unsigned code = 0;
            int r = bridge.get_subtask_info(str_param(p, "subtask_id"), &tjson, &body, &code);
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("task_json", Json::s(tjson));
            res.set("http_code", Json::n((double)code));
            res.set("http_body", Json::s(body));
            return resp_ok(res);
        }
        if (m == "get_model_mall_detail_url") {
            std::string url;
            int r = bridge.get_model_mall_detail_url(&url, str_param(p, "id"));
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("url", Json::s(url));
            return resp_ok(res);
        }
        if (m == "get_camera_url") {
            std::string url;
            int r = bridge.get_camera_url(str_param(p, "dev_id"), &url);
            Json res = Json::object();
            res.set("code", Json::n(r));
            res.set("url", Json::s(url));
            return resp_ok(res);
        }

        if (m == "start_print") {
            Json pp = params ? *params : Json::object();
            int r = bridge.start_print(pp, {}, []() { return false; });
            return ok_num(r);
        }
        if (m == "start_local_print") {
            Json pp = params ? *params : Json::object();
            int r = bridge.start_local_print(pp, {}, []() { return false; });
            return ok_num(r);
        }

        return resp_err("unknown method: " + m);
    } catch (const std::exception& e) {
        return resp_err(std::string("exception: ") + e.what());
    } catch (...) {
        return resp_err("unknown exception");
    }
}

} // namespace bambu
