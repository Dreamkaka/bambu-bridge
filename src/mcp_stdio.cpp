#include "mcp_stdio.hpp"
#include "camera_stream.hpp"
#include "json.hpp"
#include "rpc_handler.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#else
#  include <unistd.h>
#endif

namespace bambu {

std::FILE* open_mcp_output() {
    std::fflush(stdout);
#ifdef _WIN32
    const int fd = _dup(_fileno(stdout));
    if (fd < 0) return nullptr;
    auto* output = _fdopen(fd, "wb");
    if (!output) { _close(fd); return nullptr; }
    const auto stderr_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    if (_dup2(_fileno(stderr), _fileno(stdout)) || !SetStdHandle(STD_OUTPUT_HANDLE, stderr_handle)) {
#else
    const int fd = dup(STDOUT_FILENO);
    if (fd < 0) return nullptr;
    auto* output = fdopen(fd, "wb");
    if (!output) { close(fd); return nullptr; }
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
#endif
        std::fclose(output);
        return nullptr;
    }
    return output;
}

namespace {

constexpr const char* protocol = "2025-11-25";

enum class Args { none, printer, camera };
struct Tool {
    const char* name;
    const char* rpc;
    const char* description;
    Args args;
    bool control = false;
};
constexpr Tool tools[] = {
    {"bridge_status", "status", "Read bridge and cloud connection status.", Args::none},
    {"list_printers", "list_printers", "List printers bound to the signed-in Bambu account.", Args::none},
    {"printer_status", "pushall", "Request a full status report from the printer.", Args::printer},
    {"printer_firmware", "get_version", "Read printer firmware versions.", Args::printer},
    {"camera_snapshot", nullptr, "Capture one JPEG frame from the printer camera.", Args::camera},
    {"pause_print", "pause", "Pause the active print. Ask the user to confirm before calling.", Args::printer, true},
    {"resume_print", "resume", "Resume a paused print. Ask the user to confirm before calling.", Args::printer, true},
    {"stop_print", "stop", "Cancel the active print. Ask the user to confirm before calling.", Args::printer, true},
};

struct Field { const char* name; const char* description; };
constexpr Field printer_fields[] = {
    {"dev_id", "Printer serial number."},
};
constexpr Field camera_fields[] = {
    {"dev_id", "Printer serial number."},
    {"ip", "Printer IP for LAN; omit to use cloud."},
    {"password", "LAN camera access code. Do not include it in user-visible responses."},
    {"proto", "LAN camera protocol: local (default), rtsp or rtsps."},
    {"dev_ver", "Optional printer firmware version."},
};

std::string_view string_view(const Json* j) {
    return j && j->kind == Json::String ? std::string_view(j->str) : std::string_view{};
}

const std::string& string_arg(const Json& args, const char* name) {
    static const std::string empty;
    const auto* value = args.find(name);
    return value && value->kind == Json::String ? value->str : empty;
}

template<class Fn>
void for_fields(Args args, Fn fn) {
    if (args == Args::printer) for (const auto& field : printer_fields) fn(field);
    if (args == Args::camera) for (const auto& field : camera_fields) fn(field);
}

Json list_tools(bool enable_control) {
    auto list = Json::array();
    for (const auto& tool : tools) {
        if (tool.control && !enable_control) continue;
        auto props = Json::object();
        auto required = Json::array();
        for_fields(tool.args, [&](const Field& field) {
            auto schema = Json::object();
            schema.set("type", Json::s("string")).set("description", Json::s(field.description));
            if (std::string_view(field.name) == "dev_id") {
                schema.set("minLength", Json::n(1));
                required.arr.push_back(Json::s(field.name));
            }
            props.set(field.name, std::move(schema));
        });
        auto input = Json::object();
        input.set("type", Json::s("object")).set("properties", std::move(props))
             .set("required", std::move(required)).set("additionalProperties", Json::boolean(false));
        auto hints = Json::object();
        hints.set("readOnlyHint", Json::boolean(!tool.control))
             .set("destructiveHint", Json::boolean(std::string_view(tool.name) == "stop_print"))
             .set("openWorldHint", Json::boolean(true));
        list.arr.push_back(Json::object().set("name", Json::s(tool.name))
                                        .set("description", Json::s(tool.description))
                                        .set("inputSchema", std::move(input)).set("annotations", std::move(hints)));
    }
    return Json::object().set("tools", std::move(list));
}

std::string validate_args(const Tool& tool, const Json& args) {
    if (tool.args != Args::none && string_arg(args, "dev_id").find_first_not_of(" \t\r\n") == std::string::npos)
        return "dev_id must be a non-empty printer serial number";
    for (const auto& kv : args.obj) {
        const Field* match = nullptr;
        for_fields(tool.args, [&](const Field& field) { if (kv.first == field.name) match = &field; });
        if (!match) return "Unexpected tool argument";
        if (kv.second.kind != Json::String || kv.second.str.find('\0') != std::string::npos)
            return kv.first + " must be a string without NUL characters";
    }
    if (tool.args == Args::camera) {
        const auto& proto = string_arg(args, "proto");
        if (args.find("proto") && proto != "local" && proto != "rtsp" && proto != "rtsps")
            return "proto must be local, rtsp or rtsps";
        if (string_arg(args, "ip").empty() && (args.find("password") || args.find("proto")))
            return "LAN camera options require ip";
    }
    return {};
}

Json content_result(Json block, bool failed = false) {
    auto content = Json::array();
    content.arr.push_back(std::move(block));
    auto result = Json::object();
    result.set("content", std::move(content)).set("isError", Json::boolean(failed));
    return result;
}

Json text_result(const std::string& text, bool failed = false) {
    return content_result(Json::object().set("type", Json::s("text")).set("text", Json::s(text)), failed);
}

std::string base64(const std::string& image) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((image.size() + 2) / 3 * 4);
    for (size_t i = 0; i < image.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(image[i]);
        unsigned b = i + 1 < image.size() ? static_cast<unsigned char>(image[i + 1]) : 0;
        unsigned c = i + 2 < image.size() ? static_cast<unsigned char>(image[i + 2]) : 0;
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < image.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < image.size() ? alphabet[c & 63] : '=';
    }
    return out;
}

Json snapshot(BambuBridge& bridge, const Json& args) {
    if (!camera_source_ready()) return text_result("BambuSource not loaded", true);
    const auto url = resolve_camera_url(bridge, string_arg(args, "dev_id"), {}, string_arg(args, "ip"),
                                        "bblp", string_arg(args, "password"), string_arg(args, "proto"),
                                        string_arg(args, "dev_ver"), nullptr);
    if (url.empty()) return text_result("Could not resolve camera URL; check login and printer connectivity", true);
    if (url.rfind("bambu:///", 0) != 0) return text_result("WebRTC-only cameras are not supported; try LAN", true);
    std::string image, err;
    if (capture_camera_jpeg(url, image, err) != SnapshotStatus::ok) return text_result(err, true);
    Json data;
    data.kind = Json::String;
    data.str = base64(image);
    auto block = Json::object();
    block.set("type", Json::s("image")).set("mimeType", Json::s("image/jpeg")).set("data", std::move(data));
    return content_result(std::move(block));
}

Json call_tool(BambuBridge& bridge, const Tool& tool, const Json& args) {
    if (auto error = validate_args(tool, args); !error.empty()) return text_result(error, true);
    if (!tool.rpc) return snapshot(bridge, args);
    auto response = handle_request(bridge, Json::object().set("method", Json::s(tool.rpc)).set("params", args));
    const auto* ok = response.find("ok");
    auto* result = response.find("result");
    if (!ok || !ok->b || !result) return text_result(string_arg(response, "error"), true);
    const auto* code = result->find("code");
    const auto* http = result->find("http_code");
    const auto* timed_out = result->find("timed_out");
    const bool failed = (code && code->kind == Json::Number && code->num != 0) ||
                        (http && http->kind == Json::Number && http->num >= 400) ||
                        (timed_out && timed_out->b);
    return text_result(json_dump(*result), failed);
}

Json envelope(const Json& id) {
    return Json::object().set("jsonrpc", Json::s("2.0")).set("id", id);
}

Json rpc_error(const Json& id, int code, const char* message) {
    return envelope(id).set("error", Json::object().set("code", Json::n(code)).set("message", Json::s(message)));
}

enum class State { fresh, initializing, ready, stateless };

Json dispatch(BambuBridge& bridge, const Json& request, State& state, bool enable_control) {
    const auto* id = request.find("id");
    const auto* method = request.find("method");
    const auto* params = request.find("params");
    const auto name = string_view(method);
    const bool valid_id = id && (id->kind == Json::String ||
        (id->kind == Json::Number && std::abs(id->num) < 1e15 && std::floor(id->num) == id->num));
    if (request.kind != Json::Object || string_view(request.find("jsonrpc")) != "2.0" ||
        !method || method->kind != Json::String || (id && !valid_id))
        return rpc_error(valid_id ? *id : Json::null(), -32600, "Invalid Request");
    if (!id) {
        if (name == "notifications/initialized" && state == State::initializing) state = State::ready;
        return Json::null();
    }
    if (params && params->kind != Json::Object) return rpc_error(*id, -32602, "params must be an object");
    if (name == "ping") return envelope(*id).set("result", Json::object());
    if (name == "initialize") {
        if (state != State::fresh && state != State::stateless)
            return rpc_error(*id, -32600, "Already initialized");
        auto version = params ? string_view(params->find("protocolVersion")) : std::string_view{};
        if (version.empty()) return rpc_error(*id, -32602, "protocolVersion is required");
        const auto* negotiated = version == "2024-11-05" && state != State::stateless ? "2024-11-05" :
                                 version == "2025-03-26" ? "2025-03-26" :
                                 version == "2025-06-18" ? "2025-06-18" : protocol;
        state = State::initializing;
        auto info = Json::object();
        info.set("name", Json::s("bambu-bridge")).set("version", Json::s("1.0.0"));
        auto result = Json::object();
        result.set("protocolVersion", Json::s(negotiated)).set("serverInfo", std::move(info))
              .set("capabilities", Json::object().set("tools", Json::object()));
        return envelope(*id).set("result", std::move(result));
    }
    if (state != State::ready && state != State::stateless)
        return rpc_error(*id, -32002, "Server not initialized");
    if (name == "tools/list") return envelope(*id).set("result", list_tools(enable_control));
    if (name != "tools/call") return rpc_error(*id, -32601, "Method not found");
    const auto tool_name = params ? string_view(params->find("name")) : std::string_view{};
    const auto* args = params ? params->find("arguments") : nullptr;
    if (tool_name.empty() || (args && args->kind != Json::Object))
        return rpc_error(*id, -32602, "Expected a tool name and optional arguments object");
    for (const auto& tool : tools) {
        if (tool_name != tool.name || (tool.control && !enable_control)) continue;
        const auto empty = Json::object();
        auto result = envelope(*id);
        try {
            result.set("result", call_tool(bridge, tool, args ? *args : empty));
        } catch (const std::exception&) {
            result.set("result", text_result("Bridge could not execute the tool", true));
        }
        return result;
    }
    return rpc_error(*id, -32602, "Unknown tool");
}

} // namespace

bool supports_mcp_http_version(std::string_view version) {
    return version == "2025-03-26" || version == "2025-06-18" || version == protocol;
}

std::string dispatch_mcp_http(BambuBridge& bridge, const std::string& body, bool enable_control) {
    State state = State::stateless;
    Json request;
    auto reply = json_parse(body, &request) ? dispatch(bridge, request, state, enable_control)
                                           : rpc_error(Json::null(), -32700, "Parse error");
    return reply.kind == Json::Null ? std::string{} : json_dump(reply);
}

bool run_mcp_stdio(BambuBridge& bridge, std::FILE* output, bool enable_control) {
    State state = State::fresh;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json request;
        auto reply = json_parse(line, &request) ? dispatch(bridge, request, state, enable_control)
                                                : rpc_error(Json::null(), -32700, "Parse error");
        if (reply.kind == Json::Null) continue;
        const auto data = json_dump(reply);
        if (std::fwrite(data.data(), 1, data.size(), output) != data.size() ||
            std::fputc('\n', output) == EOF || std::fflush(output) != 0) return false;
    }
    return !std::cin.bad();
}

} // namespace bambu
