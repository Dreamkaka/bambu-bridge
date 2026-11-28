# bambu-bridge 接口文档

`bambu-bridge` 把 Bambu 网络插件封装成独立进程，对外提供 **HTTP/REST + SSE**、可选的本机 **Web API + WebSocket** 和 **stdio 行分隔 JSON-RPC**。下方启动示例使用项目内的 `open_bambu_networking_plugin`。

进程启动时会自动完成与 OrcaSlicer 相同的 agent 初始化：

`create_agent` → `set_config_dir` → `init_log` → `set_cert_file` → HTTP 头 → 回调注册 → `set_country_code` → `start` → `start_discovery`

调用方**不要**再手动走一遍这套生命周期，除非你在做热重启。

全部 RPC 方法与参数见 **[RPC.md](RPC.md)**。摄像头 / MPEG-TS 见 **[CAMERA.md](CAMERA.md)**。AMS 见 **[AMS.md](AMS.md)**。

---

## 1. 启动

Windows（必须 MSVC 编译，MinGW 与 DLL 的 C++ ABI 不兼容）：

```bat
run.bat
```

Linux：

```bash
./build/bambu-bridge \
  --plugin-dir ./open_bambu_networking_plugin/bin/linux_x64 \
  --country CN \
  --http 8080 --no-stdio
```

### 命令行参数

| 参数 | 说明 | 默认 |
|------|------|------|
| `--plugin-dir <path>` | 含 `bambu_networking(.dll/.so)` 及 companion 库的目录 | **必填** |
| `--country <US\|CN>` | 区域，决定登录域名 `.com` / `.cn` | `CN` |
| `--config-dir <path>` | 插件配置目录（登录态会写在这里） | `~/bambu-bridge` 或 `%APPDATA%\bambu-bridge` |
| `--log-dir <path>` | 插件日志目录 | `<config-dir>/log` |
| `--cert <folder>` | 含 `slicer_base64.cer` 的目录（公开 TLS 证书，不是私钥） | 自动探测 |
| `--client-version <ver>` | 上报给云端的 `X-BBL-Client-Version` 和摄像头 `net_ver` | 插件 `get_version()` |
| `--http <port>` | 开 HTTP 服务 | 未指定则不开 HTTP |
| `--http-loopback` | 仅在 `127.0.0.1` 监听 HTTP/REST 和 MCP；容器默认启用 | 关闭 |
| `--web-api` | 启用 `/api/v1` 和浏览器 WebSocket；要求 `--http` 的端口为 1–65535，隐含 `--http-loopback` | 关闭 |
| `--stdio` | 开 stdin/stdout JSON-RPC | 未指定 `--http` 时默认开 |
| `--no-stdio` | 只开 HTTP | |
| `--mcp` | 供 MCP 客户端启动的标准 stdio 服务；不能与 `--stdio` 或启用的 HTTP 同时使用 | 关闭 |
| `--mcp-enable-control` | 在 MCP 工具列表额外启用暂停、恢复、停止和文件删除 | 关闭 |
| `--help` | 帮助 | |

直接启动二进制时，`--plugin-dir` 缺失或加载失败会退出。Windows x64 的 `run.bat` 会自动传入随附插件目录，并默认监听 HTTP `8080`；可以用 `--http <port>` 覆盖。云配置见 [README 的运行说明](../README.md#运行)。

### 本机 Web API

在原启动命令上加 `--web-api --http 8080`，浏览器使用 `http://localhost:8080`。服务只接受回环对端和与监听端口匹配的 `localhost` / `127.0.0.1` Host；浏览器请求必须同源，WebSocket 必须带同源 `Origin`。普通 REST 可不带 `Origin`，供本机命令行调用。启用后，旧 HTTP 路径也受同源约束，不提供 CORS。这不是本机客户端认证：仅在可信本机使用，不要通过代理或端口转发对外开放。

下列路径都带 `/api/v1` 前缀，不接受查询参数。POST 请求体必须是最多 4096 字节的 JSON 对象，通常使用 `Content-Type: application/json`；文件接口还接受下文列出的请求头。GET 和 DELETE 不带请求体。设备 ID 是 1–64 个 ASCII 字母、数字、`_` 或 `-`。

| 方法和相对路径 | 操作 |
|---|---|
| `GET /status` | 读取桥接、账号和 MQTT 就绪状态 |
| `GET /session/login-url` | 返回 `{url}`；在顶级导航或新窗口打开，回调后看 `login_result` 或重读状态 |
| `GET /printers` | 返回 `{printers:[…]}`，仅账号绑定的设备，不含访问码 |
| `POST /lan/connection` | 传 `{dev_id,dev_ip,password}`（IPv4 字面量）；从事件确认连接状态 |
| `DELETE /lan/connection` | 断开当前 LAN 连接 |
| `POST /printers/{dev_id}/status/refresh` | 传 `{"source":"cloud"}` 或 `{"source":"lan"}`，请求设备上报 |
| `POST /printers/{dev_id}/actions/{pause,resume,stop}` | 同样指定 `source`；只允许暂停、继续、停止 |
| `POST /printers/{dev_id}/files` | 读取打印机存储文件列表，参数见下文 |
| `POST /printers/{dev_id}/files/download` | 下载列表中的一个文件，成功返回二进制附件 |
| `POST /printers/{dev_id}/files/delete` | 删除列表中的一个文件，要求 `Content-Type: application/json` |
| `POST /printers/{dev_id}/files/thumbnail` | 获取一个文件的缩略图，成功返回 PNG 或 JPEG |

成功返回 `{"ok":true,"result":…}`；失败返回 `{"ok":false,"error":{"code":"…"}}`，插件明确返回非零时另含 `error.plugin_code`。`202` 只表示请求已被接受，不代表设备已经执行，不应据此更新打印状态或自动重发。`409 login_required` 表示账号未登录，`503 agent_unavailable` 表示 agent 未就绪。

连接 `ws://localhost:8080/api/v1/ws` 后等待首帧 `{"event":"ready","payload":{}}`，再读 `/status` 和 `/printers`，并刷新当前关注的设备。事件包括 `printer_status`（按 `dev_id`、`source` 过滤，`report` 可能只是增量）、`account_changed`、`login_result`、`cloud_connection`、`lan_connection`（`status:0` 表示已连接）。不重放历史，不因连接 WebSocket 增删 MQTT 订阅。断线后将旧设备状态标记为未确认，重连后重新读取和刷新。相机仍用现有 HTTP 接口。

浏览器界面在 [`web/`](../web/)。`--web-api` 时，若当前工作目录、可执行文件所在目录，或其上一级目录中存在 `web/dist/index.html`（在 `web/` 里执行 `npm run build`），`GET /` 返回这个界面；查询里带 `ticket` 时仍走登录回调。开发时在 `web/` 执行 `npm run dev`，由 Vite 把 `/api`、`/camera.jpg`、`/camera.ts`、`/camera.html` 和 `/camera` 转发到 `http://127.0.0.1:8080`（可用环境变量 `BAMBU_BRIDGE` 改目标）。转发会改写 `Host` 与 `Origin`，否则本机接口会拒绝。Vite 只监听 `127.0.0.1`。

界面有四页。「概览」是环形状态台：外环按任务百分比绘制，中间的层叠按当前层／总层数绘制，不是模型预览或喷头实时位置；左右是温度、耗材槽和打印机。「数据」展示所选设备、所选通道的读数、温度曲线、画面，以及非敏感上报字段，可按名称、路径或读数搜索，左右翻页。点数值旁的走势按钮可看近期实际样本；历史仅保留在当前页面内，单个样本不会画成走势，断线后的缓存标为未确认。「文件」按存储和类型列出打印机里的文件并下载，打开页面或切换分类时自动读取。「设置」用来登录、切换云端或局域网，并填写局域网地址和画面协议。

#### 打印机文件

局域网文件服务使用独立连接，不依赖局域网 MQTT 已连通。云端需要登录，且打印机已绑定并支持 TUTK。

删除接口必须使用 `Content-Type: application/json`。列表、下载和缩略图接口也接受未设置该请求头、`text/plain`，以及 `curl -d` 默认使用的 `application/x-www-form-urlencoded`。请求体仍须是原始 JSON 对象，不解析 `key=value` 表单；内容无效返回 `400 invalid_body`，不支持的媒体类型返回 `415 unsupported_media_type`。

文件接口都要求 `source`（`lan` / `cloud`）、`storage`（`external` / `internal`）、`type`（`model` / `timelapse` / `video`）。局域网还需要 `dev_ip`、`password`，只放在 JSON 请求体中；云端不接受这两个字段。例如：

```json
{"source":"lan","dev_ip":"192.168.1.10","password":"ACCESS_CODE","storage":"external","type":"model"}
```

列表返回 `{"ok":true,"result":{"files":[…]}}`。每项有 `name`，打印机提供时还有 `path`、`size`（字节）、`time`（Unix 秒）或 `date`。下载、删除和获取缩略图时，在同一组筛选参数中加 `path`；只有列表项没有 `path` 时才传 `name`。操作前会重读该列表，找不到文件返回 `404 file_not_found`。

- **下载**：返回 `200 application/octet-stream`、`Content-Disposition: attachment` 和分块二进制内容，单文件上限 512 MiB。传输中断时响应没有终止块，客户端不应保存为完整文件。
- **删除**：不提供回收站。打印机确认删除后返回 `200` 和 `{"ok":true,"result":{"deleted":true}}`。若返回 `file_delete_unconfirmed` 或请求超时，先刷新文件列表再决定是否重试。
- **缩略图**：默认返回 `200 image/png` 或 `200 image/jpeg`，响应体是原始图片。模型默认取第 1 盘，可传 `"plate":2` 指定其他盘（1–256）；延时摄影和录像使用打印机生成的预览，不接受 `plate`。没有预览时返回 `404 thumbnail_not_found`。
- **缩略图和元数据**：同一个请求加 `"include_metadata":true`，改为返回 JSON。`result.thumbnail` 包含 `content_type` 和 `data_base64`；`result.file` 包含列表中该文件的字段。模型还返回 `result.model_metadata_base64`，以 3MF 内部路径为键、原始文件内容的 Base64 为值。会尝试读取 `_rels/.rels`、`3D/3dmodel.model`、`Metadata/model_settings.config`、`Metadata/slice_info.config` 和 `Metadata/project_settings.config`，单项最多 256 KiB。未能读取的成员不返回，已取得的图片和文件信息仍返回。录像和延时摄影没有 3MF 元数据。

实现使用 Open Bamboo Networking 的 [BambuSource CTRL 文件浏览接口](https://github.com/ClusterM/open-bamboo-networking/blob/master/research/09.05-ctrl.md)：局域网走 TLS 6000，云端走 TUTK，不是云端打印历史。使用 Open Bamboo Networking 时，云端文件访问需要 `block_cloud = 0`、支持 TUTK CTRL 的 `BambuSource` 和用户自行提供的 slicer 证书配置。`force_ftps` 模式下，上游不提供原始缩略图，且可能不支持按旧式文件名删除；离开局域网访问也不应启用此模式。不同固件可用的存储及文件操作不同，部分机型能列出内部模型但不允许读取或删除。接口不提供上传。


证书是 OrcaSlicer 自带的 DigiCert 公钥证书，用于插件校验 `*.bambulab.com` HTTPS，**不需要也不应该去偷任何私钥**。

---

## 2. 通用约定

### 2.1 RPC 请求

HTTP 和 stdio 共用同一套分发：

```json
{ "method": "<name>", "params": { } }
```

- `method`：字符串，必填。
- `params`：对象，可选。缺省当作 `{}`。

### 2.2 RPC 响应

成功：

```json
{ "ok": true, "result": <any> }
```

失败（未知方法、坏 JSON、内部异常）：

```json
{ "ok": false, "error": "unknown method: foo" }
```

插件返回码（`0` 成功，负数失败）通常包在 `result` 里，**不会**把 `ok` 设成 `false`。例如：

```json
{ "ok": true, "result": -1 }
```

表示 RPC 通了，但插件返回 `BAMBU_NETWORK_ERR_INVALID_HANDLE`。

### 2.3 常见插件返回码

| 值 | 含义 |
|----|------|
| `0` | 成功 |
| `-1` | 无效 handle（插件未加载 / agent 未创建） |
| `-2` | 连接失败 |
| `-4` | 发消息失败 |
| `-17` | 超时 |
| `-18` | 取消 |
| `-19` | 无效结果 |

### 2.4 HTTP 云端接口的 `result` 形状

`get_my_token`、`get_my_profile`、`list_printers`、`get_printer_firmware` 等：

```json
{
  "ok": true,
  "result": {
    "code": 0,
    "http_code": 200,
    "http_body": "{ ... 云端原始 JSON 字符串 ... }"
  }
}
```

`http_body` 是**字符串**，需要再 `JSON.parse` 一次。

### 2.5 发命令并等待 payload

控制命令默认等待匹配的打印机回报。`code: 0` 表示插件接受发送，不代表命令执行成功；`timed_out: true` 表示未等到回报，不宜据此重发。具体匹配规则见 [RPC.md](RPC.md#控制类-result默认-waittrue)。

默认 `wait=true`，`timeout_ms=8000`。

```json
{
  "ok": true,
  "result": {
    "code": 0,
    "timed_out": false,
    "dev_id": "22E8BJ630901079",
    "source": "cloud",
    "msg": "{...打印机原始 JSON 字符串...}",
    "payload": { }
  }
}
```

- `payload`：把 `msg` 再 parse 一次后的对象，直接用这个即可。
- `timed_out=true`：命令已发出（`code` 仍可能是 0），但超时没收到回报。确认已 `add_subscribe`。
- `wait=false`：立刻返回 `{ "code": 0 }`，回报仍走 SSE。

### 2.6 云端 vs 局域网发命令

| 通道 | RPC | 用途 |
|------|-----|------|
| 云端 MQTT | `send_message` | 已绑定账号、走 Bambu 云 |
| 局域网 MQTT | `send_message_to_printer` | LAN 模式，插件内部签名 |
| 自动选择 | `printer_command`（`lan: true/false`） | 封装上面两个 |

云端控制打印机：先 `list_printers`，再 `add_subscribe`，再 `send_message` / `pushall` / `pause`。

---

## 3. HTTP 端点

Base：`http://127.0.0.1:<port>`（示例用 `8080`）。

### `GET /health`

进程探活，不经过 RPC 分发。

```json
{
  "ok": true,
  "version": "02.05.02.76",
  "loaded": true,
  "agent_ready": true,
  "logged_in": false
}
```

### `GET /login`

`302` 跳转到 Bambu 官网 ticket 登录页（与 OrcaSlicer 外部浏览器登录同一套 URL）。

### `GET /login-url`

返回登录 URL，不跳转。

```json
{
  "ok": true,
  "host": "https://bambulab.cn",
  "redirect_url": "http://localhost:8080",
  "url": "https://bambulab.cn/sign-in?&from=studio&source=portal&to=..."
}
```

### `GET /` 或任意路径，query 含 `ticket=`

OAuth 回调。Bambu 登录完成后会把 `ticket` 带回本机。bridge 执行：

`get_my_token(ticket)` → `get_my_profile(token)` → 规范化 `change_user` → `start_subscribe("app")` → `connect_server`

成功页：`Authentication complete`  
失败页：`Authentication failed`

同时向 SSE 推 `login_result`。

### `GET /sign-in/callback`

无 `ticket` 时返回失败 HTML。有 `ticket` 时走上面同一套登录。

### `POST /rpc`

`Content-Type: application/json`

```bash
curl -s -X POST http://127.0.0.1:8080/rpc \
  -H "Content-Type: application/json" \
  -d '{"method":"status","params":{}}'
```

HTTP 状态码几乎总是 `200`；业务成败看 body 里的 `ok` / `result.code`。

### `GET /camera.ts` / `GET /camera.html`

BambuSource 转 MPEG-TS。完整参数、LAN/云端、播放器示例见 **[CAMERA.md](CAMERA.md)**。

```
vlc http://127.0.0.1:8080/camera.ts?dev_id=SERIAL
http://127.0.0.1:8080/camera.html?dev_id=SERIAL
http://127.0.0.1:8080/camera.ts?dev_id=SERIAL&ip=192.168.1.10&password=ACCESS_CODE&proto=local
```

### `GET /camera.jpg`

拍摄当前摄像头的一帧，成功时返回 `200 image/jpeg`（JPEG 二进制，不是 JSON 或 Base64），且不缓存。参数与 `/camera.ts` 相同，`dev_id` 必填。

```bash
curl --fail 'http://127.0.0.1:8080/camera.jpg?dev_id=SERIAL&ip=192.168.1.10&password=ACCESS_CODE&proto=local' -o snapshot.jpg
```

MJPEG 和 H.264 都不需要 FFmpeg。失败时返回 JSON `{"ok":false,"error":"..."}`，状态码见 [相机文档](CAMERA.md)。

### `POST /api/<method>`

REST 简写，等价于 `POST /rpc` 且 `method` 取路径最后一段。body 就是 `params` 对象。

```bash
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"22E8BJ630901079"}'
```

也可用 query：`POST /api/pushall?dev_id=22E8BJ630901079`。默认等待打印机 payload 后返回，不必再翻 SSE。

### `GET /events`

SSE 长连接。每条事件：

```
event: on_message
data: {"event":"on_message","payload":{"dev_id":"...","msg":"...","source":"cloud"}}

```

建议同时用 HTTP RPC 发命令、用 SSE 收打印机推送。

---

## 4. stdio JSON-RPC

一行一个 JSON。请求：

```text
{"method":"status","params":{}}
```

响应：

```text
{"ok":true,"result":{"loaded":true,"logged_in":false,...}}
```

异步事件混在同一 stdout，靠有没有 `event` 键区分：

```text
{"event":"on_message","payload":{"dev_id":"...","msg":"...","source":"cloud"}}
```

### MCP（Streamable HTTP）

HTTP 服务启用后，在本机 MCP 客户端选择 **Streamable HTTP**，连接 `http://127.0.0.1:8080/mcp`。该接口和 `/login` 共用 Bridge 实例及登录状态。`POST /mcp` 对请求返回 JSON-RPC JSON，对通知返回 `202` 空响应。服务不分配会话 ID，也不提供独立 SSE 流（`GET /mcp` 返回 `405`）。仅允许本机连接以及对应的 `Host`、`Origin`；不要通过代理对外暴露。工具和 `--mcp-enable-control` 行为与下述 stdio 相同。

### MCP（stdio）

使用 `bambu-bridge --mcp --plugin-dir <目录>`，不要使用会默认启动 HTTP 的打包脚本。使用 [MCP stdio](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports#stdio)，每行一个 JSON-RPC 2.0 消息；日志写入 stderr，旧版异步事件不会混入 stdout。客户端先发送 `initialize`，再发 `notifications/initialized`，然后调用 `tools/list` 和 `tools/call`。也支持 `ping`。

| 工具 | 参数 | 用途 |
|---|---|---|
| `bridge_status` | 无 | Bridge 和云端登录状态 |
| `list_printers` | 无 | 登录账号绑定的设备 |
| `printer_status` | `dev_id` | 通过云连接请求打印机完整状态 |
| `printer_firmware` | `dev_id` | 通过云连接查询打印机固件版本 |
| `camera_snapshot` | `dev_id`；可选 `ip`、`password`、`proto`、`dev_ver` | 返回 MCP `image` 内容（Base64 JPEG）；提供 `ip` 走 LAN，否则走云端 |
| `list_printer_files` | `dev_id`、`source`、`storage`、`type`；LAN 加 `dev_ip`、`password` | 列出打印机存储中文件 |
| `printer_file_thumbnail` | 同上，加 `path` 或 `name`；可选 `plate`、`include_metadata` | 返回 MCP `image`；请求元数据时另返回一条 JSON 文本 |
| `download_printer_file` | 同列表，加 `path` 或 `name` | 保存到 Bridge 主机的临时目录，返回 `local_path`、`name`、`size` |
| `delete_printer_file` | 同列表，加 `path` 或 `name`、`confirm: true` | 永久删除单个文件；仅在 `--mcp-enable-control` 下可调用 |
| `pause_print`、`resume_print`、`stop_print` | `dev_id` | 通过云连接控制打印；仅在 `--mcp-enable-control` 下可调用 |

文件工具使用[打印机文件接口](#打印机文件)的参数取值。对单个文件操作时，优先传列表返回的 `path`，只有未返回 `path` 才传 `name`。`download_printer_file` 流式写入独立的临时子目录，不覆盖现有文件，也不把原文件塞进 MCP 消息；上限 512 MiB。失败时清理不完整的文件，成功后由调用方移动或清理。路径是 Bridge 进程所在系统的路径，并非远程客户端的文件。

删除前，Agent 应向用户展示准确的打印机和文件并获得确认，再传 `confirm: true`。删除超时后先重新读取文件列表，不要自动重试。

除了图片，工具使用 MCP `text` 内容返回 JSON 结果。插件报错或等待回执超时会标记 `isError: true`；超时不代表打印机没有收到命令。MCP 不暴露任意 G-code、原始 MQTT 或登录凭据接口。

云端工具需要先在 HTTP 模式登录。同一系统用户下，HTTP 和 MCP 使用相同的默认配置目录，无需指定 `--config-dir`。如果登录时指定了自定义目录或非默认 `--country`，MCP 启动时沿用即可。

---

## 5. 推荐调用顺序

```
启动进程
  → GET /health 确认 loaded/agent_ready
  → GET /login  浏览器登录
  → GET /health 确认 logged_in=true
  → POST list_printers
  → POST add_subscribe {dev_list:[serial]}
  → POST set_user_selected_machine {dev_id}
  → POST pushall {dev_id}
  → 监听 GET /events 的 on_message
  → POST pause / resume / stop / send_message
```

局域网：

```
connect_printer {dev_id, dev_ip, username:"bblp", password:<access_code>, use_ssl:true}
  → on_local_connect status=0
  → send_message_to_printer / pushall {lan:true}
```

---

## 6. RPC 方法清单

除非另有说明，下面写的是 `result` 的内容。

### 6.1 生命周期 / 状态

#### `status`

无参数。

```json
{
  "loaded": true,
  "agent_ready": true,
  "version": "02.05.02.76",
  "logged_in": true,
  "server_connected": true,
  "user_id": "...",
  "user_name": "...",
  "country": "CN",
  "host": "https://bambulab.cn"
}
```

#### `version`

无参数。`result`：插件版本字符串。

#### `init_agent`

无参数。进程启动已调用过。`result`：`{ "code": 0 }` 或 `{ "code": -1, "error": "..." }`。

#### `create_agent`

| 参数 | 类型 | 说明 |
|------|------|------|
| `log_dir` | string | 日志目录 |

`result`：插件 int。

#### `destroy_agent`

无参数。`result`：插件 int。

#### `set_country_code`

| 参数 | 类型 |
|------|------|
| `country_code` | string，`CN` / `US` |

#### `set_config_dir`

| 参数 | 类型 |
|------|------|
| `config_dir` | string |

#### `set_cert_file`

| 参数 | 类型 |
|------|------|
| `folder` | string，证书目录 |
| `filename` | string，默认 `slicer_base64.cer` |

#### `init_log` / `start`

无参数。启动时已调用。

#### `load_plugin`

无操作占位，返回空对象。真正加载只发生在进程启动。

---

### 6.2 登录 / 账号

#### `get_login_url`

无参数。

```json
{ "host": "https://bambulab.cn", "url": "https://bambulab.cn/sign-in?..." }
```

HTTP 模式下 `url` 已带本机回调；纯 stdio 且未开 HTTP 时退化为 `host/sign-in`。

#### `login_with_ticket`

| 参数 | 类型 | 说明 |
|------|------|------|
| `ticket` | string | 官网回调里的 ticket |

```json
{
  "code": 0,
  "logged_in": true,
  "user_id": "...",
  "user_name": "..."
}
```

失败时多一个 `error` 字符串。这是浏览器登录的底层方法；一般走 `/login` 即可。

#### `change_user`

| 参数 | 类型 | 说明 |
|------|------|------|
| `user_info` | string | 登录 payload JSON |

若 `user_info` 为空，则把整个 `params` 对象当 payload。会自动规范化成：

```json
{
  "command": "user_login",
  "data": {
    "token": "...",
    "access_token": "...",
    "refresh_token": "...",
    "expires_in": "...",
    "refresh_expires_in": "...",
    "user_id": "...",
    "uidStr": "...",
    "user": { "id": "...", "uid": "...", "uidStr": "...", "name": "...", "account": "...", "avatar": "..." }
  }
}
```

成功后自动 `start_subscribe("app")` + `connect_server`。

#### `is_user_login`

无参数。`result`：bool。

#### `user_logout`

| 参数 | 类型 | 默认 |
|------|------|------|
| `request` | bool | `true`，是否同时请求云端登出 |

#### `get_user_id` / `get_user_name` / `get_user_avatar` / `get_user_nickname`

无参数。`result`：字符串。

#### `get_bambulab_host`

插件返回的云 host。

#### `get_studio_info_url`

Studio 信息页 URL。

#### `build_login_cmd` / `build_login_info` / `build_logout_cmd`

插件给 WebView 用的 JS 命令字符串。HTTP ticket 登录一般用不到。

#### `connect_server` / `is_server_connected` / `refresh_connection`

登录成功后会自动 `connect_server`。`is_server_connected` 返回 bool。

#### `get_user_selected_machine` / `set_user_selected_machine`

| 参数 | 类型 |
|------|------|
| `dev_id` | string（仅 set） |

当前选中的打印机序列号。

#### `get_my_token`

| 参数 | 类型 |
|------|------|
| `ticket` | string |

`result`：`{ code, http_code, http_body }`。`http_body` 含 `accessToken` / `refreshToken`。

#### `get_my_profile`

| 参数 | 类型 |
|------|------|
| `token` | string，access token |

`result`：`{ code, http_code, http_body }`。`http_body` 含 `uidStr` / `name` / `account` / `avatar`。

---

### 6.3 设备列表 / 云端读取

#### `list_printers`（别名 `get_user_print_info`）

无参数。账号下已绑定打印机。

`http_body` 解析后大致为：

```json
{
  "devices": [
    {
      "dev_id": "22E8BJ630901079",
      "dev_name": "X1C",
      "dev_online": true,
      "dev_model_name": "BL-P001",
      "dev_product_name": "X1 Carbon",
      "task_status": "IDLE",
      "dev_access_code": "12345678"
    }
  ]
}
```

#### `get_user_tasks`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `status` | number | `0` |
| `offset` | number | `0` |
| `limit` | number | `20` |

`result`：`{ code, http_body }`。

#### `get_printer_firmware`

| 参数 | 类型 |
|------|------|
| `dev_id` | string |

`result`：`{ code, http_code, http_body }`。

#### `get_slice_info`

| 参数 | 类型 | 默认 |
|------|------|------|
| `project_id` | string | |
| `profile_id` | string | |
| `plate_index` | number | `0` |

`result`：`{ code, slice_json }`。

#### `get_subtask_info`

| 参数 | 类型 |
|------|------|
| `subtask_id` | string |

`result`：`{ code, task_json, http_code, http_body }`。

#### `get_camera_url`

| 参数 | 类型 | 说明 |
|------|------|------|
| `dev_id` | string | 序列号 |
| `protocols` | string | 默认 `"tutk","agora"` |
| `dev_ver` | string | 固件版本，可从 pushall 取 |
| `ip` / `user` / `password` / `proto` | string | LAN：`proto`=`local`/`rtsp`/`rtsps` |

`result`：`{ code, url, play_url }`。`url` 是 `bambu:///`（VLC 不能播）；`play_url` 才是 MPEG-TS。详见 **[CAMERA.md](CAMERA.md)**。

#### `get_model_mall_detail_url`

| 参数 | 类型 |
|------|------|
| `id` | string，模型 id |

`result`：`{ code, url }`。

---

### 6.4 订阅 / 控制

#### `start_subscribe` / `stop_subscribe`

| 参数 | 类型 | 默认 |
|------|------|------|
| `module` | string | `"app"` |

登录成功后已自动 `start_subscribe("app")`。

#### `add_subscribe` / `del_subscribe`

| 参数 | 类型 |
|------|------|
| `dev_list` | string[]，序列号列表 |

订阅指定打印机的云端上报。网关发送命令时会自动订阅目标，不需要等 `on_printer_connected` 才能发第一条命令。

#### `enable_multi_machine`

| 参数 | 类型 | 默认 |
|------|------|------|
| `enable` | bool | `true` |

下列控制命令共用等待参数：

| 参数 | 类型 | 默认 |
|------|------|------|
| `wait` | bool | `true`，等待匹配的回报；`false` 只发布 |
| `timeout_ms` | number | `8000` |

#### `send_message`

云端发 MQTT。

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `json_str` | string，打印机命令 JSON | |
| `qos` | number | `0` |
| `flag` | number | `0` |
| `wait` | bool | `true` |
| `timeout_ms` | number | `8000` |

`json_str` 必须是**字符串**，不要直接塞对象。默认返回 `payload`。

#### `send_message_to_printer`

局域网发 MQTT（插件签名）。参数同 `send_message`。

#### `printer_command`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `json_str` | string | |
| `lan` | bool | `false`（云端） |
| `qos` | number | `0` |
| `flag` | number | `0` |
| `wait` | bool | `true` |
| `timeout_ms` | number | `8000` |

`lan=false` → `send_message`；`lan=true` → `send_message_to_printer`。

#### `pushall`

请求打印机推送全量状态，**默认等回报**。

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `lan` | bool | `false` |
| `wait` | bool | `true` |
| `timeout_ms` | number | `8000` |

```bash
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"22E8BJ630901079"}'
```

`result.payload` 是等待期间收到的 `print.push_status`，可能是周期性增量上报，不保证是全量快照。内部发送：

```json
{
  "pushing": {
    "sequence_id": "0",
    "command": "pushall",
    "version": 1,
    "push_target": 1
  }
}
```

#### `pause` / `resume` / `stop`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `lan` | bool | `false` |
| `wait` | bool | `true` |
| `timeout_ms` | number | `8000` |

内部发送：

```json
{ "print": { "sequence_id": "0", "command": "pause" } }
```

`command` 分别为 `pause` / `resume` / `stop`。

完整打印机控制参数（温度 / 风扇 / 灯光 / AMS / G-code 等）见 **[RPC.md](RPC.md)**。AMS 编号与换料见 **[AMS.md](AMS.md)**。未单独封装的命令用 `send_message` 传 `json_str`。

---

### 6.5 局域网打印机

#### `connect_printer`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `dev_ip` | string | |
| `username` | string | `"bblp"` |
| `password` | string，LAN access code | |
| `use_ssl` | bool | `true` |

连接结果走事件 `on_local_connect`。`status=0` 成功；`msg=="5"` 通常是密码错误。

#### `disconnect_printer`

无参数。断开当前 LAN 连接。

#### `start_discovery`

| 参数 | 类型 | 默认 |
|------|------|------|
| `start` | bool | `true` |
| `sending` | bool | `false` |

SSDP 发现。进程启动已 `start_discovery(true, false)`。发现结果走 `on_ssdp_msg`。

#### `install_device_cert`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `lan_only` | bool | `false` |

打印机连上后内部也会自动装设备证书。

---

### 6.6 打印任务

#### `start_print` / `start_local_print`

`params` 直接映射插件 `PrintParams`。进度走 SSE `print_status`。

常用字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `dev_id` | string | |
| `task_name` | string | |
| `project_name` | string | |
| `filename` | string | 3mf / gcode 路径 |
| `plate_index` | number | |
| `connection_type` | string | `lan` / `cloud` 等 |
| `dev_ip` | string | LAN |
| `username` / `password` | string | LAN |
| `use_ssl_for_ftp` | bool | 默认 `false` |
| `use_ssl_for_mqtt` | bool | 默认 `true` |
| `ams_mapping` | string | |
| `task_use_ams` | bool | |
| `task_bed_leveling` | bool | 默认 `true` |
| `task_flow_cali` | bool | 默认 `true` |
| `task_vibration_cali` | bool | 默认 `true` |
| `task_layer_inspect` | bool | 默认 `true` |
| `task_record_timelapse` | bool | 默认 `false` |

`result`：插件 int。这是同步阻塞调用，可能持续较长时间。

---

## 7. 异步事件

HTTP：`GET /events`（SSE，`event:` + `data:`）  
stdio：stdout 一行 `{"event":"...","payload":{...}}`

| event | payload | 何时 |
|-------|---------|------|
| `state` | `{ "state": "ready" }` | agent 初始化完成 |
| `user_login` | `{ logged_in, user_id, user_name }` | `change_user` / ticket 登录成功 |
| `login_result` | `{ "success": true }` | HTTP 回调页处理完（仅 HTTP） |
| `on_user_login` | `{ online_login, login }` | 插件登录回调 |
| `on_server_connected` | `{ return_code, reason_code }` | 云 MQTT 连上。`return_code==5` 表示登录过期 |
| `on_http_error` | `{ http_code, http_body }` | 插件 HTTP 错误 |
| `on_ssdp_msg` | `{ dev_info }` | LAN 发现，`dev_info` 为 JSON 字符串 |
| `on_printer_connected` | `{ topic }` | 云端打印机 MQTT 就绪；内部会自动 `pushall` |
| `on_subscribe_failure` | `{ topic }` | 订阅失败 |
| `on_message` | `{ dev_id, msg, source:"cloud" }` | 云端打印机上报，`msg` 为 JSON 字符串 |
| `on_user_message` | `{ user_id, msg }` | 用户通道消息 |
| `on_local_connect` | `{ status, dev_id, msg }` | LAN 连接。`0` 成功 |
| `on_local_message` | `{ dev_id, msg, source:"lan" }` | LAN 打印机上报 |
| `on_server_error` | `{ url, status }` | 云服务错误 |
| `print_status` | `{ status, code, msg }` | `start_print` / `start_local_print` 进度 |

`on_message.msg` / `on_local_message.msg` 是打印机协议 JSON 字符串，需要再解析。全量状态一般在 `pushall` 之后出现，字段因机型而异（温度、AMS、任务进度等）。

---

## 8. 打印机命令 JSON（`json_str`）

这些是发给打印机的 MQTT payload，不是 RPC `method`。用 `send_message` / `printer_command` 时自行拼；`pushall` / `pause` / `resume` / `stop` 已封装。

请求全量状态：

```json
{
  "pushing": {
    "sequence_id": "0",
    "command": "pushall",
    "version": 1,
    "push_target": 1
  }
}
```

暂停 / 继续 / 停止：

```json
{ "print": { "sequence_id": "0", "command": "pause" } }
```

取 access code：

```json
{ "system": { "sequence_id": "0", "command": "get_access_code" } }
```

取版本：

```json
{ "info": { "sequence_id": "0", "command": "get_version" } }
```

灯光等更多命令与 OrcaSlicer `MachineObject::publish_json` 相同。云端用 `send_message`，LAN 用 `send_message_to_printer`。

---

## 9. 调用示例

### 9.1 curl：状态 + 列表

```bash
curl -s http://127.0.0.1:8080/health
curl -s -X POST http://127.0.0.1:8080/rpc \
  -H "Content-Type: application/json" \
  -d '{"method":"list_printers","params":{}}'
```

### 9.2 curl：订阅并 pushall（直接拿 payload）

```bash
curl -s -X POST http://127.0.0.1:8080/api/add_subscribe \
  -H "Content-Type: application/json" \
  -d '{"dev_list":["22E8BJ630901079"]}'

curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"22E8BJ630901079"}'
```

返回里看 `result.payload`，不必连 SSE。超时把 `timeout_ms` 调大，或不想等则 `"wait": false`。

### 9.3 Python

```python
import json, requests
BASE = "http://127.0.0.1:8080"

def rpc(method, params=None):
    r = requests.post(BASE + "/rpc", json={"method": method, "params": params or {}})
    r.raise_for_status()
    return r.json()

print(requests.get(BASE + "/health").json())
st = rpc("status")["result"]
if not st["logged_in"]:
    print("open", requests.get(BASE + "/login-url").json()["url"])
    raise SystemExit(0)

body = json.loads(rpc("list_printers")["result"]["http_body"])
dev = body["devices"][0]["dev_id"]
rpc("add_subscribe", {"dev_list": [dev]})
st = rpc("pushall", {"dev_id": dev})
print(st["result"].get("payload"))
```

完整脚本见 `examples/python_example.py`。

### 9.4 监听 SSE

```bash
curl -N http://127.0.0.1:8080/events
```

---

## 10. 错误排查

| 现象 | 可能原因 |
|------|----------|
| 进程立刻退出 | `--plugin-dir` 错，或 companion DLL/SO 找不到 |
| Windows 调用插件崩溃 | 用了 MinGW 编译，必须 MSVC |
| `/login` 后回调失败 | 没带 `--http`；或 `--country` 与账号区域不一致（CN 用 `.cn`） |
| `is_user_login=false` | ticket 兑换失败，看插件 log_dir；确认 `--cert` 指向 `slicer_base64.cer` 所在目录 |
| `list_printers` 空 | 未登录，或账号下没绑定机 |
| 发了 pause 没反应 | 没 `add_subscribe`；或 LAN 机却走了云端 `send_message` |
| `on_local_connect` msg=`5` | LAN access code 错误 |
| `on_server_connected` return_code=`5` | 登录过期，需要重新 `/login` |

插件日志在 `--log-dir`（默认配置目录下的 `log/`）。
