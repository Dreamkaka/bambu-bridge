#include "rpc_handler.hpp"
#include "http_server.hpp"
#include "json.hpp"
#include "camera_stream.hpp"

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
        auto wait_cmd = [&](const std::string& dev, const std::string& json_str, bool lan) -> Json {
            bool wait = bool_param(p, "wait", true);
            int timeout_ms = (int)num_param(p, "timeout_ms", 8000);
            Json res = Json::object();
            if (!wait) {
                int r = bridge.printer_command(dev, json_str, lan,
                                               (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0));
                res.set("code", Json::n(r));
                return resp_ok(res);
            }
            auto w = bridge.printer_command_wait(dev, json_str, lan, timeout_ms,
                                                 (int)num_param(p, "qos", 0), (int)num_param(p, "flag", 0));
            res.set("code", Json::n(w.code));
            res.set("timed_out", Json::boolean(w.timed_out));
            res.set("dev_id", Json::s(w.dev_id));
            res.set("source", Json::s(w.source));
            res.set("msg", Json::s(w.msg));
            Json parsed;
            if (!w.msg.empty() && json_parse(w.msg, &parsed))
                res.set("payload", parsed);
            if (w.timed_out) res.set("error", Json::s("timeout waiting for printer message"));
            return resp_ok(res);
        };

        if (m == "send_message") {
            return wait_cmd(str_param(p, "dev_id"), str_param(p, "json_str"), false);
        }
        if (m == "send_message_to_printer") {
            return wait_cmd(str_param(p, "dev_id"), str_param(p, "json_str"), true);
        }
        if (m == "printer_command") {
            return wait_cmd(str_param(p, "dev_id"), str_param(p, "json_str"), bool_param(p, "lan", false));
        }
        if (m == "pushall") {
            std::string dev = str_param(p, "dev_id");
            Json cmd = Json::object();
            Json pushing = Json::object();
            pushing.set("sequence_id", Json::s("0"));
            pushing.set("command", Json::s("pushall"));
            pushing.set("version", Json::n(1));
            pushing.set("push_target", Json::n(1));
            cmd.set("pushing", pushing);
            return wait_cmd(dev, json_dump(cmd), bool_param(p, "lan", false));
        }
        auto seq = []() -> std::string {
            static int n = 20000;
            return std::to_string(n++);
        };
        auto print_cmd = [&](const char* command) {
            Json cmd = Json::object();
            Json print = Json::object();
            print.set("sequence_id", Json::s(seq()));
            print.set("command", Json::s(command));
            cmd.set("print", print);
            return cmd;
        };

        if (m == "pause" || m == "resume" || m == "stop") {
            Json cmd = print_cmd(m.c_str());
            cmd.find("print")->set("param", Json::s(""));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "get_version") {
            Json cmd = Json::object();
            Json info = Json::object();
            info.set("sequence_id", Json::s(seq()));
            info.set("command", Json::s("get_version"));
            cmd.set("info", info);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "get_access_code") {
            Json cmd = Json::object();
            Json sys = Json::object();
            sys.set("sequence_id", Json::s(seq()));
            sys.set("command", Json::s("get_access_code"));
            cmd.set("system", sys);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "gcode_line" || m == "send_gcode") {
            Json cmd = print_cmd("gcode_line");
            cmd.find("print")->set("param", Json::s(str_param(p, "gcode", str_param(p, "param"))));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "set_bed_temp") {
            Json cmd = print_cmd("set_bed_temp");
            cmd.find("print")->set("temp", Json::n(num_param(p, "temp")));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "set_nozzle_temp") {
            Json cmd = print_cmd("set_nozzle_temp");
            cmd.find("print")->set("extruder_index", Json::n(num_param(p, "extruder_index", 0)));
            cmd.find("print")->set("target_temp", Json::n(num_param(p, "temp", num_param(p, "target_temp"))));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "set_chamber_temp") {
            Json cmd = print_cmd("set_ctt");
            cmd.find("print")->set("ctt_val", Json::n(num_param(p, "temp", num_param(p, "ctt_val"))));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "set_fan") {
            Json cmd = print_cmd("set_fan");
            cmd.find("print")->set("fan_index", Json::n(num_param(p, "fan_index", num_param(p, "fan_id"))));
            cmd.find("print")->set("speed", Json::n(num_param(p, "speed")));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ledctrl" || m == "set_light") {
            Json cmd = Json::object();
            Json sys = Json::object();
            sys.set("sequence_id", Json::s(seq()));
            sys.set("command", Json::s("ledctrl"));
            sys.set("led_node", Json::s(str_param(p, "led_node", "chamber_light")));
            sys.set("led_mode", Json::s(str_param(p, "led_mode", str_param(p, "mode", "on"))));
            sys.set("led_on_time", Json::n(num_param(p, "led_on_time", 500)));
            sys.set("led_off_time", Json::n(num_param(p, "led_off_time", 500)));
            sys.set("loop_times", Json::n(num_param(p, "loop_times", 1)));
            sys.set("interval_time", Json::n(num_param(p, "interval_time", 1000)));
            cmd.set("system", sys);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "home" || m == "go_home") {
            Json cmd = print_cmd("back_to_center");
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ams_control") {
            Json cmd = print_cmd("ams_control");
            cmd.find("print")->set("param", Json::s(str_param(p, "action", str_param(p, "param", "resume"))));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ams_change_filament") {
            Json cmd = print_cmd("ams_change_filament");
            bool load = bool_param(p, "load", true);
            int ams_id = (int)num_param(p, "ams_id");
            int slot_id = (int)num_param(p, "slot_id");
            int tray_id = ams_id < 16 ? ams_id * 4 + slot_id : ams_id;
            cmd.find("print")->set("curr_temp", Json::n(num_param(p, "curr_temp", num_param(p, "old_temp"))));
            cmd.find("print")->set("tar_temp", Json::n(num_param(p, "tar_temp", num_param(p, "new_temp"))));
            cmd.find("print")->set("ams_id", Json::n(ams_id));
            if (!load) {
                cmd.find("print")->set("target", Json::n(255));
                cmd.find("print")->set("slot_id", Json::n(255));
            } else {
                cmd.find("print")->set("target", Json::n(tray_id == 0 ? ams_id : tray_id));
                cmd.find("print")->set("slot_id", Json::n(slot_id));
            }
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ams_user_setting") {
            Json cmd = print_cmd("ams_user_setting");
            cmd.find("print")->set("ams_id", Json::n(num_param(p, "ams_id", -1)));
            cmd.find("print")->set("startup_read_option", Json::boolean(bool_param(p, "startup_read_option", true)));
            cmd.find("print")->set("tray_read_option", Json::boolean(bool_param(p, "tray_read_option", true)));
            cmd.find("print")->set("calibrate_remain_flag", Json::boolean(bool_param(p, "calibrate_remain_flag", true)));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "print_speed") {
            Json cmd = print_cmd("print_speed");
            std::string lvl = str_param(p, "level", str_param(p, "param"));
            if (lvl.empty()) lvl = std::to_string((int)num_param(p, "level", num_param(p, "param", 2)));
            cmd.find("print")->set("param", Json::s(lvl));
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ipcam_record") {
            Json cmd = Json::object();
            Json cam = Json::object();
            cam.set("sequence_id", Json::s(seq()));
            cam.set("command", Json::s("ipcam_record_set"));
            cam.set("control", Json::s(bool_param(p, "enable", true) ? "enable" : "disable"));
            cmd.set("camera", cam);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "ipcam_timelapse") {
            Json cmd = Json::object();
            Json cam = Json::object();
            cam.set("sequence_id", Json::s(seq()));
            cam.set("command", Json::s("ipcam_timelapse"));
            cam.set("control", Json::s(bool_param(p, "enable", true) ? "enable" : "disable"));
            cmd.set("camera", cam);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
        }
        if (m == "skip_objects") {
            Json cmd = print_cmd("skip_objects");
            const Json* lst = p.find("obj_list");
            Json arr = Json::array();
            if (lst && lst->kind == Json::Array) arr = *lst;
            cmd.find("print")->set("obj_list", arr);
            return wait_cmd(str_param(p, "dev_id"), json_dump(cmd), bool_param(p, "lan", false));
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
            std::string err;
            std::string url = resolve_camera_url(bridge, str_param(p, "dev_id"),
                                                 str_param(p, "protocols", "\"tutk\",\"agora\""),
                                                 str_param(p, "ip"),
                                                 str_param(p, "user", "bblp"),
                                                 str_param(p, "password"),
                                                 str_param(p, "proto"),
                                                 str_param(p, "dev_ver"),
                                                 &err);
            Json res = Json::object();
            res.set("code", Json::n(url.empty() ? -1.0 : 0.0));
            res.set("url", Json::s(url));
            if (http_listen_port() > 0 && !str_param(p, "dev_id").empty()) {
                std::string play = "http://127.0.0.1:" + std::to_string(http_listen_port())
                    + "/camera.ts?dev_id=" + str_param(p, "dev_id");
                if (!str_param(p, "ip").empty()) play += "&ip=" + str_param(p, "ip");
                if (!str_param(p, "password").empty()) play += "&password=" + str_param(p, "password");
                if (!str_param(p, "proto").empty()) play += "&proto=" + str_param(p, "proto");
                res.set("play_url", Json::s(play));
            }
            if (url.empty()) res.set("error", Json::s(err));
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
