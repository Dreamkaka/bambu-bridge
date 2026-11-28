#include "bridge.hpp"
#include "plugin_loader.hpp"
#include "stdio_transport.hpp"
#include "mcp_stdio.hpp"
#include "http_server.hpp"
#include "rpc_handler.hpp"
#include "json.hpp"

#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

using namespace bambu;

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --plugin-dir <path>      Directory containing bamboo_networking(.so/.dll) and friends\n"
        "  --country <US|CN>        Region (default: CN)\n"
        "  --config-dir <path>      Plugin config directory\n"
        "  --log-dir <path>         Plugin log directory\n"
        "  --cert <folder>          Directory containing slicer_base64.cer\n"
        "  --client-version <ver>   X-BBL-Client-Version / camera net_ver (default: plugin get_version)\n"
        "  --http <port>            Enable HTTP/REST server on given port (0 = disabled)\n"
        "  --http-loopback         Bind HTTP/REST and MCP to 127.0.0.1 only\n"
        "  --web-api               Enable local Web API (requires --http, implies --http-loopback)\n"
        "  --stdio                  Enable stdio JSON-RPC mode (default: enabled if --http not given)\n"
        "  --no-stdio               Disable stdio JSON-RPC mode\n"
        "  --mcp                   Serve MCP over stdio instead of HTTP/legacy stdio\n"
        "  --mcp-enable-control    Expose print controls and file deletion in MCP mode\n"
        "  --help                   Show this help\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    AgentConfig cfg;
    int  http_port = 0;
    bool use_stdio = false;
    bool http_given = false;
    bool http_loopback = false;
    bool web_api = false;
    const char* final_http_arg = nullptr;
    const char* legacy_http_arg = nullptr;
    bool use_mcp = false;
    bool mcp_control = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s requires a value\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "--plugin-dir") { const char* v = next("--plugin-dir"); if (v) cfg.plugin_dir = v; }
        else if (a == "--country") { const char* v = next("--country"); if (v) cfg.country_code = v; }
        else if (a == "--config-dir") { const char* v = next("--config-dir"); if (v) cfg.config_dir = v; }
        else if (a == "--log-dir") { const char* v = next("--log-dir"); if (v) cfg.log_dir = v; }
        else if (a == "--cert") { const char* v = next("--cert"); if (v) cfg.cert_folder = v; }
        else if (a == "--client-version") { const char* v = next("--client-version"); if (v) cfg.client_version = v; }
        else if (a == "--http") {
            final_http_arg = next("--http");
            if (final_http_arg) legacy_http_arg = final_http_arg;
            http_given = true;
        }
        else if (a == "--http-loopback") http_loopback = true;
        else if (a == "--web-api") web_api = true;
        else if (a == "--stdio") { use_stdio = true; }
        else if (a == "--no-stdio") { use_stdio = false; }
        else if (a == "--mcp") use_mcp = true;
        else if (a == "--mcp-enable-control") mcp_control = true;
        else if (a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }

    if (web_api) {
        if (final_http_arg) {
            const auto end = final_http_arg + std::strlen(final_http_arg);
            const auto parsed = std::from_chars(final_http_arg, end, http_port);
            if (parsed.ec != std::errc{} || parsed.ptr != end) http_port = 0;
        }
        if (http_port < 1 || http_port > 65535) {
            std::fprintf(stderr, "[bridge] --web-api requires --http with a port from 1 to 65535\n");
            return 1;
        }
    } else if (legacy_http_arg) {
        http_port = std::atoi(legacy_http_arg);
    }

    if ((use_mcp && (use_stdio || http_port != 0)) || (mcp_control && !use_mcp && http_port <= 0)) {
        std::fprintf(stderr, "[bridge] --mcp cannot be combined with --stdio or an enabled HTTP port; "
                             "--mcp-enable-control requires an MCP transport\n");
        return 1;
    }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> mcp_output(nullptr, std::fclose);
    if (use_mcp) {
        mcp_output.reset(open_mcp_output());
        if (!mcp_output) {
            std::fprintf(stderr, "[bridge] could not reserve stdout for MCP\n");
            return 1;
        }
    }

    // Default country.
    if (cfg.country_code.empty()) cfg.country_code = "CN";

    // Default transport: if --http not given, enable stdio by default.
    if (!use_mcp && !http_given && !use_stdio) use_stdio = true;

    if (!init_sockets()) { std::fprintf(stderr, "[bridge] socket init failed\n"); return 1; }

    BambuBridge bridge;

    std::string err;
    if (!bridge.load_plugin(cfg, &err)) {
        std::fprintf(stderr, "[bridge] FAILED to load plugin: %s\n", err.c_str());
        std::fprintf(stderr, "[bridge] plugin dir: %s\n", cfg.plugin_dir.c_str());
        return 1;
    }
    std::fprintf(stdout, "[bridge] plugin loaded, version=%s client_version=%s\n",
                 bridge.version().c_str(), bridge.client_version().c_str());
    std::fflush(stdout);

    std::string init_err;
    int ac = bridge.init_agent(&init_err);
    if (ac != 0) {
        std::fprintf(stderr, "[bridge] init_agent failed: %d %s\n", ac, init_err.c_str());
        return 1;
    }
    std::fprintf(stdout, "[bridge] agent ready, host=%s\n", bridge.get_host_for_login().c_str());
    std::fflush(stdout);

    // Start HTTP on a background thread (runs alongside stdio, if enabled).
    if (http_port > 0) {
        if (!start_http_server(bridge, http_port, mcp_control, http_loopback || web_api, web_api)) {
            std::fprintf(stderr, "[bridge] failed to start HTTP server on port %d\n", http_port);
            return 1;
        }
    }

    int exit_code = 0;
    if (use_mcp) {
        if (!run_mcp_stdio(bridge, mcp_output.get(), mcp_control)) exit_code = 1;
    } else if (use_stdio) {
        // stdio loop owns the main thread; HTTP, if any, runs in background.
        run_stdio(bridge);
    } else if (http_port > 0) {
        // HTTP only: keep the process alive.
        while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
    } else {
        std::fprintf(stderr, "[bridge] no transport enabled\n");
        return 1;
    }

    stop_http_server();
    bridge.destroy_agent();
    return exit_code;
}
