# AGENTS.md — bambu-bridge

Minimal, high-signal guide for AI agents working in this repository.

---

## 1. Project Overview & Boundaries

`bambu-bridge` is a dependency-free C++17 daemon that wraps Bambu Lab's proprietary closed-source networking plugin (`bambu_networking.dll` / `libbambu_networking.so`) and exposes:
- **HTTP / REST / SSE** (`POST /rpc`, `GET /health`, `GET /events`)
- **stdio JSON-RPC** (line-delimited JSON requests, responses, and events)

### Critical Architecture Boundaries
- `include/bambu_abi.hpp`: Raw ABI function pointers and signatures extracted from OrcaSlicer. **Do not include this in transports (`http_server.*`, `stdio_transport.*`)**.
- `src/bridge.hpp` / `src/bridge.cpp`: The **only** layer that directly touches `bambu_abi.hpp` and loads dynamic libraries (`plugin_loader`).
- `src/rpc_handler.hpp` / `src/rpc_handler.cpp`: Shared dispatch table routing JSON requests (`method` + `params`) to `BambuBridge`.
- `src/json.hpp` / `src/json.cpp`: Standalone minimal JSON parser/serializer. No external JSON libraries.

---

## 2. ABI Compatibility (DO NOT MISS)

The closed-source vendor plugin exports C symbols (`bambu_network_*`) that pass **C++ types** (`std::string`, `std::map`, `std::function`). The bridge must match the plugin toolchain:
- **Linux (`libbambu_networking.so`)**: Built with GCC/Clang → Build bridge with `g++` / `clang++` (libstdc++). Fully compatible.
- **Windows (`bambu_networking.dll`)**: Built with **MSVC** → Bridge **MUST** be built with **MSVC (`cl.exe` / MSVC CMake)**.
- **Windows + MinGW g++**: Can build the bridge binary, but will crash or fail at runtime on plugin calls due to C++ ABI layout mismatch.

---

## 3. Build & Test Commands

### Test Execution (Works without Plugin)
```bash
# Plugin-independent self-test (exercises JSON serialization, routing, safe fallback)
# Windows:
.\build\bambu-bridge-self-test.exe

# Linux:
./build/bambu-bridge-self-test

# Smoke test CLI help:
.\build\bambu-bridge.exe --help        # Windows
./build/bambu-bridge --help            # Linux
```

### Building the Self-Test
```bash
# Linux (g++):
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc \
    src/self_test.cpp src/bridge.cpp src/plugin_loader.cpp src/json.cpp \
    src/rpc_handler.cpp src/stdio_transport.cpp src/http_server.cpp \
    -o build/bambu-bridge-self-test

# Windows (MSVC cl.exe):
cl /std:c++17 /O2 /Iinclude /Isrc /D_WIN32_WINNT=0x0601 \
   src\self_test.cpp src\bridge.cpp src\plugin_loader.cpp src\json.cpp \
   src\rpc_handler.cpp src\stdio_transport.cpp src\http_server.cpp \
   ws2_32.lib /Fe:build\bambu-bridge-self-test.exe
```

### Building the Main Binary
```bash
# Linux (direct build script):
./build.sh                     # uses g++
./build.sh --clang             # uses clang++

# Cross-platform CMake:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target bambu-bridge

# Windows (MSVC developer console direct cl.exe):
cl /std:c++17 /O2 /Iinclude /Isrc /D_WIN32_WINNT=0x0601 ^
   src\main.cpp src\bridge.cpp src\plugin_loader.cpp src\json.cpp ^
   src\rpc_handler.cpp src\stdio_transport.cpp src\http_server.cpp ^
   ws2_32.lib /Fe:build\bambu-bridge.exe
```

---

## 4. Runtime & Verification Gotchas

- **Plugin Directory**: `--plugin-dir <path>` is required at runtime and must point to the directory containing `bambu_networking(.so/.dll)` along with its runtime companion libraries (`libBambuSource`, `live555`, etc.). The process exits with code 1 if plugin loading fails.
- **Startup sequence** (matches OrcaSlicer): `create_agent` → `set_config_dir` → `init_log` → `set_cert_file` → HTTP headers → callbacks (`queue_on_main`, `get_country_code`, MQTT/login/ssdp) → `set_country_code` → `start` → `start_discovery`. This is done by `init_agent()` at process start; callers should not skip it.
- **Login**: Browser `GET /login` (ticket OAuth) → `/sign-in/callback` → `get_my_token` + `get_my_profile` → canonical `change_user` payload → `start_subscribe("app")` + `connect_server`. Devices: `list_printers` / `get_user_print_info`. Control: `add_subscribe` + `send_message` (cloud) or `send_message_to_printer` (LAN).
- **Cert**: `--cert <folder>` or auto-detect `slicer_base64.cer` (OrcaSlicer `resources/cert`).
- **Camera**: `GET /camera.ts?dev_id=` uses `BambuSource` (`Bambu_Create/Open/StartStream/ReadSample`) and remuxes H264 into MPEG-TS. Cloud URL is `get_camera_url("{id}|{ver}|\"tutk\",\"agora\"")`. LAN: `?ip=&password=&proto=local|rtsp|rtsps`. Not a real RTSP server.
- **Transport Flags**:
  - If `--http <port>` is omitted and `--no-stdio` is not set, `--stdio` is automatically active.
  - To run HTTP server only in background: `bambu-bridge --plugin-dir <dir> --http 8080 --no-stdio`.
  - Default `--country` is `CN` unless explicitly set to `US`.
- **Response Format**:
  - Direct helper methods or RPC handlers return `{ "ok": true, "result": ... }` or `{ "ok": false, "error": "..." }`.
  - stdio pushes async messages with `{ "event": "...", "payload": { ... } }`.
- **Git Boundaries**: Never commit closed-source vendor plugin dirs (`win_*`, `linux_*`, etc.) or log dumps (`*.log`). Verify with `git status` before committing.
