# Bambu Bridge — 跨平台 Bambu 网络插件封装进程

把 Bambu 官方闭源网络插件（`bambu_networking.dll` / `libbambu_networking.so`）封装成**一个独立进程**，对外暴露 **HTTP/REST JSON** 和 **stdio JSON-RPC** 两种接口，供其他应用或 agent 调用。这样第三方程序无需直接处理 C++ ABI / MQTT 签名 / 视频流协商，只需发 JSON。

完整接口见 **[docs/API.md](docs/API.md)**，RPC 参数见 **[docs/RPC.md](docs/RPC.md)**，摄像头见 **[docs/CAMERA.md](docs/CAMERA.md)**，AMS 见 **[docs/AMS.md](docs/AMS.md)**。

```
 [agent / 应用 / LLM]
        │  HTTP JSON 或 stdio JSON-RPC
        ▼
 ┌───────────────────────────┐
 │      bambu-bridge 进程      │
 │  (标准 C++17, 跨平台)      │
 │  main.cpp + bridge.cpp     │
 └─────────────┬─────────────┘
        │ dlopen / LoadLibrary  (C++ ABI 转接)
        ▼
 ┌───────────────────────────┐
 │  bambu_networking(.so/.dll)│  ← 官方闭源插件
 │  + BambuSource + live555 … │
 └───────────────────────────┘
```

---

## 重要：ABI 前提（必须读）

这个插件导出的函数**符号是 C 风格**（`bambu_network_*`，名字可用 `dlsym`/`GetProcAddress` 拿到），但**函数签名用了 C++ 类型**（`std::string`、`std::map`、`std::function` 回调），因此**桥必须用与插件相同的 C++ ABI 编译**：

| 平台 | 插件 | 桥用什么编译 | 能否工作 |
|------|------|-------------|---------|
| **Linux** | `libbambu_networking.so`（GCC/Clang 构建） | **g++ / clang++（libstdc++）** | ✅ 正常 |
| **Windows** | `bambu_networking.dll`（MSVC 构建） | **必须用 MSVC (cl.exe)** | ✅ 正常 |
| **Windows** | `bambu_networking.dll`（MSVC 构建） | **MinGW g++（w64devkit）** | ❌ **C++ ABI 不兼容，跨边界调用会崩/失败** |

- **Linux 上：直接用这个工程 `build.sh`，没问题。** 这是主力平台。
- **Windows 上：请用 MSVC 编译**（见下文 MSVC 说明）。`build.bat` 用的是 MinGW g++，**只能编译出框架**，但调用插件会因 ABI 不匹配而失败——它存在是为了方便你验证能编译，不代表能调通 Windows DLL。

---

## 目录结构

```
bambu-bridge/
├── CMakeLists.txt            # CMake 构建（跨平台）
├── build.sh                  # Linux/macOS g++/clang 一键构建
├── build.bat                 # Windows MinGW g++ 构建（框架可编译，见 ABI 警告）
├── build.ps1                 # （可选）Windows PowerShell 版构建
├── examples/
│   ├── python_example.py     # 通过 HTTP 调用
│   └── node_example.mjs      # 通过 HTTP 调用 (Node)
├── include/
│   └── bambu_abi.hpp         # 插件 C++ ABI 函数指针表（从 OrcaSlicer 提取）
└── src/
    ├── bridge.hpp/.cpp       # 核心：加载插件 + ABI 转接 + 方法封装
    ├── plugin_loader.hpp/.cpp# dlopen / LoadLibrary 跨平台加载
    ├── json.hpp/.cpp         # 自包含精简 JSON 序列化/解析
    ├── rpc_handler.hpp/.cpp  # 共用请求分发（method → bridge 方法）
    ├── stdio_transport.hpp/.cpp # stdio JSON-RPC 传输
    ├── http_server.hpp/.cpp # 极简 HTTP/REST + SSE 服务器
    └── main.cpp              # 入口：参数解析 + 双传输入口
```

---

## 构建

### Linux / macOS（用 g++）

```bash
cd bamboo-bridge
./build.sh                     # 产物 build/bambu-bridge
# 或用 CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bambu-bridge
```

### Windows（建议 MSVC）

用 VS 开发者命令行（`cl.exe`）：

```bash
cl /std:c++17 /O2 /Iinclude /Isrc ^
   /D_WIN32_WINNT=0x0601 ^
   src\main.cpp src\bridge.cpp src\plugin_loader.cpp src\json.cpp ^
   src\rpc_handler.cpp src\stdio_transport.cpp src\http_server.cpp ^
   ws2_32.lib ^
   /Fe:build\bambu-bridge.exe
```

（MinGW：`build.bat`，能编译但调用 Windows DLL 会因 ABI 不匹配而失败，仅供验证框架。）

---

## 运行

```bash
# 同时开 HTTP(:8080) + stdio
./build/bambu-bridge --plugin-dir ../linux_02.05.02.58 --country CN --http 8080 --stdio

# 只开 HTTP
./build/bambu-bridge --plugin-dir ../linux_02.05.02.58 --country CN --http 8080 --no-stdio

# 只开 stdio
./build/bambu-bridge --plugin-dir ../linux_02.05.02.58 --country CN --stdio
```

`--plugin-dir` 指向放 `libbambu_networking.so`（Linux）或 `bambu_networking.dll`（Windows）的目录。

---

完整字段、返回值、事件 payload、登录与控制顺序见 [docs/API.md](docs/API.md)。

## 协议

### HTTP 端点

| 方法/路径 | 说明 |
|-----------|------|
| `POST /rpc` | body：`{"method":"...","params":{...}}` → `{"ok":true,"result":...}` 或 `{"ok":false,"error":"..."}` |
| `POST /api/<method>` | REST 简写，body 即 params。`pushall`/`pause` 等默认等待打印机 `payload` 后返回 |
| `GET  /health` | `{"ok":true,"version":"...","loaded":true,"agent_ready":true,"logged_in":false}` |
| `GET  /login` | 302 跳转到 Bambu 账号登录页 |
| `GET  /login-url` | 返回登录 URL JSON |
| `GET  /sign-in/callback` | OAuth ticket 回调，完成 `login_with_ticket` |
| `GET /events` | SSE 事件流，推送插件异步事件（MQTT 消息、登录态、进度） |
| `GET /camera.ts?dev_id=` | BambuSource 转 MPEG-TS 直播，VLC / ffmpeg / mpegts.js 可播 |
| `GET /camera.html?dev_id=` | 浏览器预览页 |

### stdio JSON-RPC

- 一行一个请求：`{"method":"...","params":{...}}`
- 一行一个响应：`{"ok":true,"result":...}` 或 `{"ok":false,"error":"..."}`
- 异步事件：`{"event":"...","payload":{...}}`（客户端靠是否有 `event` 键区分）

---

## 方法清单（`method` → 功能）

进程启动时会自动 `init_agent()`（create → config/log/cert → 回调 → start → discovery）。RPC 一律返回 `{ok, result}`。

### 生命周期
- `init_agent`、`create_agent`、`destroy_agent`、`version`、`status`
- `set_country_code`、`set_config_dir`、`set_cert_file`、`init_log`、`start`

### 登录 / 云端连接
- `login_with_ticket`、`change_user`
- `connect_server`、`is_server_connected`、`refresh_connection`
- `is_user_login`、`user_logout`
- `get_user_id`、`get_user_name`、`get_user_avatar`、`get_user_nickname`
- `get_bambulab_host`、`get_login_url`、`get_user_selected_machine`、`set_user_selected_machine`
- `get_studio_info_url`、`get_my_token`、`get_my_profile`

### 订阅 / MQTT（云端）
- `start_subscribe`、`stop_subscribe`、`add_subscribe`、`del_subscribe`
- `send_message`（云端 MQTT）
- `send_message_to_printer`（局域网，插件签名）
- `printer_command`、`pushall`、`pause`、`resume`、`stop`

### 打印机
- `connect_printer`、`disconnect_printer`、`start_discovery`、`install_device_cert`

### 云端读取 / 任务
- `list_printers` / `get_user_print_info`、`get_user_tasks`、`get_printer_firmware`
- `get_slice_info`、`get_subtask_info`、`get_camera_url`、`get_model_mall_detail_url`

### 打印
- `start_print`、`start_local_print`

---

## 调用示例

### Python（HTTP）

```python
import requests, json
BASE = "http://127.0.0.1:8080"

def rpc(method, params=None):
    r = requests.post(BASE+"/rpc", json={"method": method, "params": params or {}})
    return r.json()

print(rpc("version"))
print(rpc("create_agent", {"log_dir": "/tmp"}))
print(rpc("set_country_code", {"country_code": "CN"}))
print(rpc("connect_server"))
print(rpc("add_subscribe", {"dev_list": ["22E8BJ630901079"]}))
# 发命令（插件内部带签名，解决 mqtt verify failed 问题）
print(rpc("send_message_to_printer", {
    "dev_id": "22E8BJ630901079",
    "json_str": json.dumps({"pushing": {"sequence_id":"0","command":"pushall"}}),
    "qos": 0, "flag": 0,
}))
```

### 远程 curl

```bash
curl -s -X POST http://127.0.0.1:8080/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"version","params":{}}'

curl -s -X POST http://127.0.0.1:8080/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"send_message_to_printer","params":{"dev_id":"22E8BJ630901079","json_str":"{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}","qos":0,"flag":0}}'
```

---

## 实现要点

- **无第三方依赖**：JSON 和 HTTP 服务器都是自实现，跨平台，`g++`/`MSVC` 都能编译。
- **事件驱动**：插件回调（`OnUserLoginFn`、`OnUpdateStatusFn`、`OnMessageFn` 等）在 `bridge.cpp` 里转成 `Json`，通过 `EventSink` 推到 HTTP `/events`（SSE）或 stdio 事件行。
- **ABI 转接**：`bambu_abi.hpp` 严格来自 OrcaSlicer 的 `BBLNetworkPlugin.hpp`，函数指针用 `dlsym`/`GetProcAddress` 解析，用 `std::function`/`std::string` 构造插件期望的回调/参数。

---

## 已知限制 / 注意

1. **Windows + MinGW 不能调板（ABI）**：见开头。Windows 需 MSVC。
2. **视频流（BRTC）**：插件内 `bambu_network_get_camera_url_for_golive` 已封装，但 BRTC 协商逻辑在闭源插件内部；本桥只是转发它返回的 URL，本身不实现 WebRTC 协商。
3. **完整端点仍在扩展中**：当前实现覆盖了控制命令、订阅、登录、任务、打印。部分高级端点（如 `start_publish`、`start_sdcard_print`、tracking、rating 等）未全部封装，可作为后续补充。
4. **插件依赖**：运行目录需有 `libbambu_networking.so` 同目录的 `libBambuSource.so`、`liblive555.so`、`libagora_rtc_sdk.so`（你已备好）。
