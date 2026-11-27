#include "http_web_api.hpp"
#include "rpc_handler.hpp"
#include "http_websocket.hpp"
#include "camera_stream.hpp"
#include "printer_files.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace bambu {

bool is_web_device_id(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

WebApiResponse web_api_error(int status, std::string_view code) {
    Json error = Json::object();
    error.set("code", Json::s(std::string(code)));
    Json response = Json::object();
    response.set("ok", Json::boolean(false));
    response.set("error", std::move(error));
    return {status, std::move(response), {}};
}

namespace {

WebApiResponse success(int status, Json result) {
    Json response = Json::object();
    response.set("ok", Json::boolean(true));
    response.set("result", std::move(result));
    return {status, std::move(response), {}};
}

WebApiResponse upstream_failed(int plugin_code = 0) {
    auto response = web_api_error(502, "upstream_failed");
    if (plugin_code) response.body.obj.back().second.set("plugin_code", Json::n(plugin_code));
    return response;
}

const Json* field(const Json& object, const char* key, Json::Kind kind) {
    const auto* value = object.find(key);
    return value && value->kind == kind ? value : nullptr;
}

bool integer(const Json* value, int& out) {
    if (!value || value->kind != Json::Number || !std::isfinite(value->num) ||
        std::trunc(value->num) != value->num || value->num < std::numeric_limits<int>::min() ||
        value->num > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(value->num);
    return true;
}

bool is_json_media_type(std::string_view type) {
    type = type.substr(0, type.find(';'));
    const auto start = type.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    type = type.substr(start, type.find_last_not_of(" \t") - start + 1);
    constexpr std::string_view json = "application/json";
    return type.size() == json.size() && std::equal(type.begin(), type.end(), json.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
}

struct ProjectedField { const char* key; Json::Kind kind; };
constexpr ProjectedField status_fields[] = {
    {"loaded", Json::Bool}, {"agent_ready", Json::Bool}, {"logged_in", Json::Bool},
    {"server_connected", Json::Bool}, {"version", Json::String}, {"client_version", Json::String},
    {"country", Json::String}, {"mqtt_ready", Json::Array}
};
constexpr ProjectedField printer_fields[] = {
    {"dev_name", Json::String}, {"dev_online", Json::Bool}, {"dev_model_name", Json::String},
    {"dev_product_name", Json::String}, {"task_status", Json::String}
};

WebApiResponse project_printers(const std::string& body) {
    Json directory;
    if (!json_parse(body, &directory, true) || directory.kind != Json::Object) return upstream_failed();
    const auto* devices = field(directory, "devices", Json::Array);
    if (!devices) return upstream_failed();

    Json printers = Json::array();
    std::unordered_set<std::string_view> seen;
    for (const auto& device : devices->arr) {
        const auto* id = field(device, "dev_id", Json::String);
        if (!id || !is_web_device_id(id->str) || !seen.insert(id->str).second) continue;
        Json printer = Json::object();
        printer.set("dev_id", *id);
        for (const auto& f : printer_fields) {
            const auto* value = field(device, f.key, f.kind);
            printer.set(f.key, value ? *value : Json{});
        }
        printers.arr.push_back(std::move(printer));
    }
    Json result = Json::object();
    result.set("printers", std::move(printers));
    return success(200, std::move(result));
}

constexpr double max_file_size = 512 * 1024 * 1024;
constexpr size_t max_file_count = 10000;

bool file_text(std::string_view text, size_t limit) {
    return !text.empty() && text.size() <= limit && websocket_utf8_valid(text) &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

bool safe_file_ref(std::string_view text, bool path) {
    if (!file_text(text, path ? 2048 : 512) || text.find('\\') != text.npos ||
        text.find(':') != text.npos || (path ? text.front() != '/' : text.find('/') != text.npos)) return false;
    if (path) text.remove_prefix(1);
    for (;;) {
        auto slash = text.find('/');
        const auto part = text.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == text.npos) return true;
        text.remove_prefix(slash + 1);
    }
}

std::string url_component(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '~' || c == '.') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}

bool file_number(const Json* value, double max) {
    return value && value->kind == Json::Number && value->num >= 0 && value->num <= max &&
        std::trunc(value->num) == value->num;
}

WebApiResponse control_error(FileControlStatus status, int printer_code) {
    if (status == FileControlStatus::unavailable) return web_api_error(503, "storage_plugin_unavailable");
    if (status == FileControlStatus::connect_failed) return web_api_error(502, "storage_connection_failed");
    if (status == FileControlStatus::timeout || printer_code == 6) return web_api_error(504, "storage_timeout");
    if (printer_code == 10) return web_api_error(404, "file_not_found");
    if (printer_code == 17) return web_api_error(409, "storage_unavailable");
    auto response = web_api_error(502, "storage_request_failed");
    if (printer_code) response.body.obj.back().second.set("printer_code", Json::n(printer_code));
    return response;
}

WebApiResponse printer_files(BambuBridge& bridge, std::string_view id, bool download, const Json& input,
                             const std::atomic<bool>& running, const WebFileSink& sink) {
    const auto* source = field(input, "source", Json::String);
    const auto* storage = field(input, "storage", Json::String);
    const auto* type = field(input, "type", Json::String);
    if (!source || (source->str != "lan" && source->str != "cloud") ||
        !storage || (storage->str != "external" && storage->str != "internal") ||
        !type || (type->str != "model" && type->str != "timelapse" && type->str != "video"))
        return web_api_error(400, "invalid_body");
    const bool lan = source->str == "lan";
    const auto* path = field(input, "path", Json::String);
    const auto* name = field(input, "name", Json::String);
    if (input.obj.size() != 3u + (lan ? 2u : 0u) + (download ? 1u : 0u) ||
        (download && ((path != nullptr) == (name != nullptr) ||
            !safe_file_ref(path ? path->str : name->str, path != nullptr))))
        return web_api_error(400, "invalid_body");

    std::string url;
    if (lan) {
        const auto* ip = field(input, "dev_ip", Json::String);
        const auto* pass = field(input, "password", Json::String);
        in_addr ipv4{};
        if (!ip || !pass || ip->str.find('\0') != std::string::npos || inet_pton(AF_INET, ip->str.c_str(), &ipv4) != 1 ||
            pass->str.empty() || pass->str.size() > 128 ||
            !std::all_of(pass->str.begin(), pass->str.end(), [](unsigned char c) { return c >= ' ' && c <= '~'; }))
            return web_api_error(400, "invalid_body");
        url = build_lan_camera_url(ip->str, "bblp", url_component(pass->str), "local",
                                   std::string(id), bridge.client_version(), {});
    }
    if (!bridge.agent_ready()) return web_api_error(503, "agent_unavailable");
    if (!lan) {
        if (!bridge.is_user_login()) return web_api_error(409, "login_required");
        url = resolve_storage_cloud_url(bridge, std::string(id));
        if (url.empty()) return web_api_error(502, "storage_cloud_unavailable");
    }

    FileControl tunnel;
    auto status = tunnel.open(bridge.plugin_dir(), url, running);
    if (status != FileControlStatus::ok) return control_error(status, 0);

    Json req = Json::object(), files = Json::array();
    req.set("type", *type);
    req.set("notify", Json::s("DETAIL"));
    req.set("api_version", Json::n(2));
    if (storage->str == "internal") req.set("storage", *storage);
    int printer_code = 0;
    size_t entries_seen = 0;
    std::string filename;
    bool ambiguous = false;
    status = tunnel.request(1, std::move(req), running, [&](int code, const Json& reply, std::string_view bytes) {
        if (code != 0 && code != 1) { printer_code = code; return false; }
        const auto* entries = field(reply, "file_lists", Json::Array);
        if (!entries || !bytes.empty() || (code == 1 && entries->arr.empty()) ||
            entries->arr.size() > max_file_count - entries_seen) return false;
        entries_seen += entries->arr.size();
        for (const auto& entry : entries->arr) {
            const auto* n = field(entry, "name", Json::String);
            const auto* p = field(entry, "path", Json::String);
            const bool has_path = p && !p->str.empty();
            if (!n || !safe_file_ref(n->str, false) || (has_path && !safe_file_ref(p->str, true))) continue;
            if (!has_path && type->str != "timelapse") continue; // Legacy printers may return timelapses for every type.
            if (download) {
                if (has_path ? path && p->str == path->str : name && n->str == name->str) {
                    if (!filename.empty() && filename != n->str) ambiguous = true;
                    filename = n->str;
                }
                continue;
            }
            Json item = Json::object();
            item.set("name", *n);
            if (has_path) item.set("path", *p);
            for (const auto* key : {"size", "time"}) {
                const auto* value = entry.find(key);
                if (file_number(value, 9007199254740991.0)) item.set(key, *value);
            }
            if (const auto* date = field(entry, "date", Json::String); date && file_text(date->str, 128))
                item.set("date", *date);
            files.arr.push_back(std::move(item));
        }
        return true;
    });
    if (status != FileControlStatus::ok) return control_error(status, printer_code);
    if (!download) {
        Json result = Json::object();
        result.set("files", std::move(files));
        return success(200, std::move(result));
    }
    // Download only files that this category and volume just advertised.
    if (filename.empty() || ambiguous) return web_api_error(404, "file_not_found");

    req = Json::object();
    req.set(path ? "path" : "file", path ? *path : *name);
    req.set("offset", Json::n(0));
    size_t received = 0;
    double total = -1;
    bool oversized = false;
    status = tunnel.request(4, std::move(req), running, [&](int code, const Json& reply, std::string_view bytes) {
        if (code != 0 && code != 1) { printer_code = code; return false; }
        const auto* size = reply.find("size");
        const auto* offset = reply.find("offset");
        const auto* expected = reply.find("total");
        if (file_number(expected, 9007199254740991.0) && expected->num > max_file_size) {
            oversized = true;
            return false;
        }
        if (!file_number(expected, max_file_size) || !file_number(offset, max_file_size) ||
            !file_number(size, max_file_size) || offset->num != static_cast<double>(received) ||
            size->num != static_cast<double>(bytes.size()) ||
            (total > 0 && total != expected->num)) return false;
        // Some FTPS implementations cannot obtain SIZE until RETR ends.
        total = expected->num;
        received += bytes.size();
        if (received > max_file_size) { oversized = true; return false; }
        if ((total > 0 && received > total) || (code == 1 && bytes.empty()) ||
            (code == 0 && received != total)) return false;
        return sink(filename, bytes);
    });
    if (oversized) return web_api_error(413, "file_too_large");
    if (status != FileControlStatus::ok) return control_error(status, printer_code);
    return {200, {}, {}};
}

} // namespace

WebApiResponse dispatch_web_api(BambuBridge& bridge, std::string_view method, std::string_view path,
                                std::string_view query, std::string_view content_type, const std::string& body,
                                const std::atomic<bool>& running, const WebFileSink& file_sink) {
    try {
        enum class Route { status, login_url, printers, lan, command, files, download };
        Route route;
        const char* rpc = nullptr;
        std::string_view id;
        std::string_view allow = "GET";
        if (path == "/api/v1/status") { route = Route::status; rpc = "status"; }
        else if (path == "/api/v1/session/login-url") { route = Route::login_url; rpc = "get_login_url"; }
        else if (path == "/api/v1/printers") { route = Route::printers; rpc = "list_printers"; }
        else if (path == "/api/v1/lan/connection") {
            route = Route::lan;
            allow = "POST, DELETE";
            rpc = method == "POST" ? "connect_printer" : "disconnect_printer";
        } else {
            constexpr std::string_view prefix = "/api/v1/printers/";
            if (path.rfind(prefix, 0) != 0) return web_api_error(404, "not_found");
            path.remove_prefix(prefix.size());
            const auto slash = path.find('/');
            if (slash == std::string_view::npos || slash == 0) return web_api_error(404, "not_found");
            id = path.substr(0, slash);
            const auto suffix = path.substr(slash);
            route = Route::command;
            if (suffix == "/status/refresh") rpc = "pushall";
            else if (suffix == "/actions/pause") rpc = "pause";
            else if (suffix == "/actions/resume") rpc = "resume";
            else if (suffix == "/actions/stop") rpc = "stop";
            else if (suffix == "/files") route = Route::files;
            else if (suffix == "/files/download") route = Route::download;
            else return web_api_error(404, "not_found");
            allow = "POST";
        }
        if (route == Route::lan ? (method != "POST" && method != "DELETE") : method != allow) {
            auto response = web_api_error(405, "method_not_allowed");
            response.extra_headers = "Allow: " + std::string(allow) + "\r\n";
            return response;
        }
        if (!query.empty()) return web_api_error(400, "invalid_query");
        if (!id.empty() && !is_web_device_id(id)) return web_api_error(400, "invalid_device_id");

        std::string_view source;
        Json params = Json::object();
        if (method != "POST") {
            if (!body.empty()) return web_api_error(400, "invalid_body");
        } else {
            if (body.size() > 4096) return web_api_error(413, "body_too_large");
            if (!is_json_media_type(content_type)) return web_api_error(415, "unsupported_media_type");
            Json input;
            if (!json_parse(body, &input, true) || input.kind != Json::Object) return web_api_error(400, "invalid_body");
            if (route == Route::files || route == Route::download)
                return printer_files(bridge, id, route == Route::download, input, running, file_sink);
            // The parser preserves duplicate object keys. Exact size plus all required keys rejects duplicates.
            if (route == Route::lan) {
                const auto* dev = field(input, "dev_id", Json::String);
                const auto* ip = field(input, "dev_ip", Json::String);
                const auto* pass = field(input, "password", Json::String);
                in_addr ipv4{};
                if (input.obj.size() != 3 || !dev || !ip || !pass || ip->str.find('\0') != std::string::npos ||
                    inet_pton(AF_INET, ip->str.c_str(), &ipv4) != 1 || pass->str.empty() || pass->str.size() > 128 ||
                    !std::all_of(pass->str.begin(), pass->str.end(), [](char ch) { return ch >= ' ' && ch <= '~'; }))
                    return web_api_error(400, "invalid_body");
                if (!is_web_device_id(dev->str)) return web_api_error(400, "invalid_device_id");
                id = dev->str;
                params = std::move(input);
            } else {
                const auto* value = field(input, "source", Json::String);
                if (input.obj.size() != 1 || !value || (value->str != "cloud" && value->str != "lan"))
                    return web_api_error(400, "invalid_body");
                source = value->str == "lan" ? "lan" : "cloud";
                params.set("dev_id", Json::s(std::string(id)));
                params.set("lan", Json::boolean(source == "lan"));
                params.set("wait", Json::boolean(false));
            }
        }

        if (route != Route::status && !bridge.agent_ready()) return web_api_error(503, "agent_unavailable");
        if ((route == Route::printers || source == "cloud") && !bridge.is_user_login())
            return web_api_error(409, "login_required");

        Json request = Json::object();
        request.set("method", Json::s(rpc));
        request.set("params", std::move(params));
        bool plugin_called = false;
        const auto response = handle_request(bridge, request, &plugin_called);
        const auto* ok = field(response, "ok", Json::Bool);
        const auto* result = response.find("result");
        if (!ok || !ok->b || !result) return web_api_error(500, "internal_error");
        Json projected = Json::object();
        if (route == Route::status) {
            for (const auto& f : status_fields) {
                const auto* value = field(*result, f.key, f.kind);
                if (!value) return web_api_error(500, "internal_error");
                projected.set(f.key, *value);
            }
            return success(200, std::move(projected));
        }
        if (route == Route::login_url) {
            const auto* url = field(*result, "url", Json::String);
            if (!url) return web_api_error(500, "internal_error");
            projected.set("url", *url);
            return success(200, std::move(projected));
        }
        int code;
        if (!integer(route == Route::lan ? result : result->find("code"), code))
            return web_api_error(500, "internal_error");
        if (code) return upstream_failed(plugin_called ? code : 0);
        if (route == Route::printers) {
            int http;
            const auto* directory = field(*result, "http_body", Json::String);
            if (!integer(result->find("http_code"), http) || !directory) return web_api_error(500, "internal_error");
            if (http < 200 || http >= 300) return upstream_failed();
            return project_printers(directory->str);
        }
        if (!id.empty()) projected.set("dev_id", Json::s(std::string(id)));
        if (!source.empty()) projected.set("source", Json::s(std::string(source)));
        projected.set("accepted", Json::boolean(true));
        return success(method == "POST" ? 202 : 200, std::move(projected));
    } catch (...) {
        return web_api_error(500, "internal_error");
    }
}

} // namespace bambu
