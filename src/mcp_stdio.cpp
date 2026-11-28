#include "mcp_stdio.hpp"
#include "base64.hpp"
#include "camera_stream.hpp"
#include "json.hpp"
#include "rpc_handler.hpp"
#include "http_web_api.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#ifdef _WIN32
#  include <random>
#endif
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
    std::optional<FileOperation> file = std::nullopt;
};
constexpr Tool tools[] = {
    {"bridge_status", "status", "Read bridge and cloud connection status.", Args::none},
    {"list_printers", "list_printers", "List printers bound to the signed-in Bambu account.", Args::none},
    {"printer_status", "pushall", "Request a full status report from the printer.", Args::printer},
    {"printer_firmware", "get_version", "Read printer firmware versions.", Args::printer},
    {"camera_snapshot", nullptr, "Capture one JPEG frame from the printer camera.", Args::camera},
    {"list_printer_files", nullptr, "List files on printer storage, not cloud print history.", Args::none, false, FileOperation::list},
    {"printer_file_thumbnail", nullptr, "Read a printer file thumbnail and optional model metadata.", Args::none, false, FileOperation::thumbnail},
    {"download_printer_file", nullptr, "Save one printer file to a unique local path on the Bridge host and return that path.", Args::none, false, FileOperation::download},
    {"delete_printer_file", nullptr, "Permanently delete one file from printer storage. Show the exact printer and file and obtain explicit user confirmation before calling.", Args::none, true, FileOperation::remove},
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

Json storage_schema(FileOperation operation) {
    auto props = Json::object(), required = Json::array();
    auto add = [&](const char* key, const char* kind, const char* desc, bool needed = false) -> Json& {
        if (needed) required.arr.push_back(Json::s(key));
        props.set(key, Json::object().set("type", Json::s(kind)).set("description", Json::s(desc)));
        return *props.find(key);
    };
    auto choices = [](std::initializer_list<const char*> values) {
        auto result = Json::array();
        for (const auto* value : values) result.arr.push_back(Json::s(value));
        return result;
    };
    add("dev_id", "string", "Printer serial number.", true).set("pattern", Json::s("^[A-Za-z0-9_-]{1,64}$"));
    add("source", "string", "Cloud uses the signed-in account; LAN requires dev_ip and password.", true)
        .set("enum", choices({"cloud", "lan"}));
    add("storage", "string", "Printer storage volume.", true).set("enum", choices({"external", "internal"}));
    add("type", "string", "File category.", true).set("enum", choices({"model", "timelapse", "video"}));
    add("dev_ip", "string", "Printer IPv4 address for LAN only.");
    add("password", "string", "LAN access code; do not include it in user-visible responses.");
    if (operation != FileOperation::list) {
        add("path", "string", "Exact path from list_printer_files. Supply path or name, never both.");
        add("name", "string", "Exact file name, only if list_printer_files did not return a path.");
    }
    if (operation == FileOperation::thumbnail) {
        add("plate", "integer", "Model plate number; model files only.")
            .set("minimum", Json::n(1)).set("maximum", Json::n(256)).set("default", Json::n(1));
        add("include_metadata", "boolean", "Also return file details and available 3MF metadata (Base64).");
    }
    if (operation == FileOperation::remove)
        add("confirm", "boolean", "Set true only after the user explicitly confirms permanent deletion.", true)
            .set("const", Json::boolean(true));
    auto schema = Json::object();
    schema.set("type", Json::s("object")).set("properties", std::move(props))
          .set("required", std::move(required)).set("additionalProperties", Json::boolean(false));
    if (operation != FileOperation::list) {
        auto refs = Json::array();
        refs.arr.push_back(Json::object().set("required", choices({"path"})));
        refs.arr.push_back(Json::object().set("required", choices({"name"})));
        schema.set("oneOf", std::move(refs));
    }
    return schema;
}

Json list_tools(bool enable_control) {
    auto list = Json::array();
    for (const auto& tool : tools) {
        if (tool.control && !enable_control) continue;
        auto input = tool.file ? storage_schema(*tool.file) : Json::object();
        if (!tool.file) {
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
            input.set("type", Json::s("object")).set("properties", std::move(props))
                 .set("required", std::move(required)).set("additionalProperties", Json::boolean(false));
        }
        auto hints = Json::object();
        hints.set("readOnlyHint", Json::boolean(!tool.control && tool.file != FileOperation::download))
             .set("destructiveHint", Json::boolean(tool.file == FileOperation::remove ||
                                                  std::string_view(tool.name) == "stop_print"))
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
    data.str = base64_encode(image);
    auto block = Json::object();
    block.set("type", Json::s("image")).set("mimeType", Json::s("image/jpeg")).set("data", std::move(data));
    return content_result(std::move(block));
}

namespace fs = std::filesystem;

fs::path create_download_dir() {
    const auto base = fs::temp_directory_path();
#ifdef _WIN32
    std::random_device random;
    for (int i = 0; i < 8; ++i) {
        auto dir = base / ("bambu-bridge-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(dir)) return dir;
    }
    return {};
#else
    auto pattern = (base / "bambu-bridge-XXXXXX").native();
    if (!mkdtemp(pattern.data())) return {};
    return fs::path(std::move(pattern));
#endif
}

struct LocalDownload {
    fs::path directory, path;
    std::ofstream stream;
    std::string name;
    size_t size = 0;
    bool failed = false, keep = false;

    ~LocalDownload() {
        if (keep || directory.empty()) return;
        if (stream.is_open()) stream.close();
        std::error_code ignored;
        if (!path.empty()) fs::remove(path, ignored);
        fs::remove(directory, ignored);
    }

    bool append(std::string_view filename, std::string_view bytes) {
        if (!stream.is_open()) {
            directory = create_download_dir();
            if (directory.empty()) { failed = true; return false; }
            name = filename;
            // The printer's filename is display data, never a writable host path.
            path = directory / "download";
            const auto dot = filename.find_last_of('.');
            if (dot != filename.npos) {
                const auto suffix = filename.substr(dot + 1);
                if (!suffix.empty() && suffix.size() <= 10 &&
                    suffix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == suffix.npos)
                    path += filename.substr(dot);
            }
            stream.open(path, std::ios::binary);
        }
        if (!bytes.empty()) stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) { failed = true; return false; }
        size += bytes.size();
        return true;
    }
};

Json call_file_tool(BambuBridge& bridge, FileOperation op, const Json& args, const std::atomic<bool>& running) {
    const auto& device = string_arg(args, "dev_id");
    Json input = Json::object();
    bool saw_id = false, confirmed = false;
    for (const auto& kv : args.obj) {
        if (kv.first == "dev_id") {
            if (saw_id) return text_result("Duplicate dev_id", true);
            saw_id = true;
        } else if (kv.first == "confirm") {
            if (op != FileOperation::remove || confirmed || kv.second.kind != Json::Bool || !kv.second.b)
                return text_result("Deletion requires explicit user confirmation (confirm: true)", true);
            confirmed = true;
        } else {
            input.obj.push_back(kv);
        }
    }
    if (op == FileOperation::remove && !confirmed)
        return text_result("Deletion requires explicit user confirmation (confirm: true)", true);

    LocalDownload download;
    auto response = printer_files(bridge, device, op, input, running,
        [&](std::string_view name, std::string_view bytes) { return download.append(name, bytes); });
    if (download.failed) return text_result("Could not write a local download on the Bridge host", true);
    if (response.status != 200) {
        const auto* error = response.body.find("error");
        return text_result(error ? json_dump(*error) : "Printer storage request failed", true);
    }
    if (op == FileOperation::download) {
        if (!download.stream.is_open()) return text_result("Printer did not send a file", true);
        download.stream.close();
        if (!download.stream) return text_result("Could not finish the local download on the Bridge host", true);
        auto result = text_result(json_dump(Json::object().set("local_path", Json::s(download.path.u8string()))
            .set("name", Json::s(download.name)).set("size", Json::n(static_cast<double>(download.size)))));
        download.keep = true;
        return result;
    }
    if (response.image) {
        return content_result(Json::object().set("type", Json::s("image"))
            .set("mimeType", Json::s(response.image->content_type)).set("data", Json::s(base64_encode(response.image->bytes))));
    }
    auto* result = response.body.find("result");
    if (!result) return text_result("Printer storage response was incomplete", true);
    if (auto* thumbnail = result->find("thumbnail")) {
        auto* mime = thumbnail->find("content_type");
        auto* data = thumbnail->find("data_base64");
        if (!mime || !data) return text_result("Printer storage response was incomplete", true);
        auto block = Json::object();
        block.set("type", Json::s("image")).set("mimeType", std::move(*mime)).set("data", std::move(*data));
        auto metadata = Json::object();
        for (auto& field : result->obj)
            if (field.first != "thumbnail") metadata.obj.push_back(std::move(field));
        auto combined = content_result(std::move(block));
        combined.find("content")->arr.push_back(Json::object().set("type", Json::s("text"))
                                                              .set("text", Json::s(json_dump(metadata))));
        return combined;
    }
    return text_result(json_dump(*result));
}

Json call_tool(BambuBridge& bridge, const Tool& tool, const Json& args, const std::atomic<bool>& running) {
    if (tool.file) return call_file_tool(bridge, *tool.file, args, running);
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

Json dispatch(BambuBridge& bridge, const Json& request, State& state, bool enable_control,
              const std::atomic<bool>& running) {
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
            result.set("result", call_tool(bridge, tool, args ? *args : empty, running));
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

std::string dispatch_mcp_http(BambuBridge& bridge, const std::string& body, bool enable_control,
                              const std::atomic<bool>& running) {
    State state = State::stateless;
    Json request;
    auto reply = json_parse(body, &request) ? dispatch(bridge, request, state, enable_control, running)
                                           : rpc_error(Json::null(), -32700, "Parse error");
    return reply.kind == Json::Null ? std::string{} : json_dump(reply);
}

bool run_mcp_stdio(BambuBridge& bridge, std::FILE* output, bool enable_control) {
    State state = State::fresh;
    const std::atomic<bool> running{true};
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json request;
        auto reply = json_parse(line, &request) ? dispatch(bridge, request, state, enable_control, running)
                                                : rpc_error(Json::null(), -32700, "Parse error");
        if (reply.kind == Json::Null) continue;
        const auto data = json_dump(reply);
        if (std::fwrite(data.data(), 1, data.size(), output) != data.size() ||
            std::fputc('\n', output) == EOF || std::fflush(output) != 0) return false;
    }
    return !std::cin.bad();
}

} // namespace bambu
