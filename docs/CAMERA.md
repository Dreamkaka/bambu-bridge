# 摄像头 / 视频流

Bambu 云预览 **不是** 普通 RTSP / HLS。OrcaSlicer 的做法是：

1. `bambu_network_get_camera_url` 换到 `bambu:///` 私有 URL  
2. `BambuSource`（`Bambu_Create` / `Open` / `StartStream` / `ReadSample`）读 H264  
3. 播放器自己解码

bambu-bridge 在第 3 步把 H264 **封装成 MPEG-TS**，用 HTTP 推出去。VLC、ffmpeg、mpegts.js 可以直接播。

WebUI 的监控在概览页，详细数据页只显示遥测字段和波形。概览使用随前端打包的 mpegts.js；点击「播放监控」时才请求 `/camera.ts`。停止、切换设备、离开概览或将标签页放到后台都会释放播放器，不会轮询 `/camera.jpg` 或在失败后自动重试。

**不是** RTSP 服务器，也 **不是** m3u8/HLS。`bambu:///` 不能丢进 VLC。

```
打印机摄像头
    │  TUTK / Agora / LAN TCP:6000 / RTSP
    ▼
BambuSource.dll  (Bambu_Tunnel)
    │  H264 Annex-B / AVCC
    ▼
bambu-bridge 封装 MPEG-TS
    │  HTTP  video/mp2t
    ▼
VLC / ffplay / 浏览器 mpegts.js
```

---

## 前提

- `--plugin-dir` 里要有 `BambuSource.dll`（或 `libBambuSource.so`）以及 `live555` 等 companion  
- 进程开了 `--http <port>`  
- 云端身份：`--client-version`（默认用插件 `get_version()`），写入 HTTP 头 `X-BBL-Client-Version` 和拉流 URL 的 `net_ver`  
- **云端**：已登录；打开 `/camera.ts` 时会自动 `add_subscribe` + 选中该机  
- **局域网**：要知道 IP 和 access code（`list_printers` 的 `dev_access_code`，或打印机屏幕）

## OrcaSlicer 实际怎么播

预览只走 `MediaPlayCtrl`，**不用** `get_camera_url_for_golive`。

1. 看 `ipcam.liveview.local` / `remote`、`tutk_server`、`rtsp_url`  
2. **能 LAN 就 LAN**：`bambu:///local/{ip}.?port=6000&user=bblp&passwd=` 或 `rtsps___` / `rtsp___`  
3. 否则云端：`get_camera_url(serial + "|" + ota_ver + "|" + "\"tutk\"" 或 agora)`  
4. 回调必须 `starts_with("bambu:///")` 才 `load()`；否则从 `...[403]` 里抠错误码，提示失败  
5. `bambu:///` 交给 `BambuSource`（wxMediaCtrl / gstbambusrc）读 H264  

`get_camera_url_for_golive` 在 Orca 预览里**没有调用**。`brtc_service` 在切片机里主要表示 TCP/上传能力，不是这路直播。

插件失败时回调经常是 `" [403]"` 这种字符串，**不是 URL**。只有 `bambu:///` 才算成功。

`pushall` 的 `payload.ipcam` 可判断能力：

| 字段 | 含义 |
|------|------|
| `ipcam.ipcam_dev` | `"1"` 才有摄像头 |
| `ipcam.liveview.remote` | `tutk` / `agora` / `tutk_agaro` / `none` |
| `ipcam.liveview.local` | `local` / `rtsps` / `rtsp` / `disabled` / `none` |
| `ipcam.rtsp_url` | 若为 `rtsps://...` 可试 VLC 直连，不必走 bridge |
| `ipcam.tutk_server` | `disable` 时云端 TUTK 不可用 |
| `ipcam.agora_service` | `disable` 时 Agora 不可用 |
| `ipcam.brtc_service` | `enable` 不代表 TUTK 能换票；Orca 预览仍走 TUTK/LAN |

---

## HTTP 播放（推荐）

Base：`http://127.0.0.1:<port>`

### `GET /camera.ts`

MPEG-TS 直播，`Content-Type: video/mp2t`。连接保持到客户端断开。

#### Query

| 参数 | 必填 | 默认 | 说明 |
|------|------|------|------|
| `dev_id` | 是 | | 打印机序列号 |
| `dev_ver` | 云端建议 | 空 | 固件版本，来自 `pushall` / `get_version` |
| `protocols` | 云端 | `"tutk","agora"` | 传给插件的协议列表 |
| `ip` | LAN | | 有此项走局域网，不调云 `get_camera_url` |
| `user` | LAN | `bblp` | |
| `password` | LAN | | access code |
| `proto` | LAN | `local` | `local`：TCP 6000；`rtsp` / `rtsps`：标准路径 `/streaming/live/1` |

#### 云端

```
http://127.0.0.1:8080/camera.ts?dev_id=22E8BJ630901079
http://127.0.0.1:8080/camera.ts?dev_id=22E8BJ630901079&dev_ver=01.07.00.00
```

内部会把 `dev_id|dev_ver|"tutk","agora"` 交给插件，得到 `bambu:///` 后再拉流。  
未传 `dev_ver` 时会从最近 MQTT 或自动 `get_version` 取 OTA `sw_ver`（缺版本云端常返回 403）。

#### 局域网

```
http://127.0.0.1:8080/camera.ts?dev_id=SERIAL&ip=192.168.1.10&password=12345678&proto=local
http://127.0.0.1:8080/camera.ts?dev_id=SERIAL&ip=192.168.1.10&password=12345678&proto=rtsps
```

对应 Orca 的 URL：

| proto | BambuSource 路径 |
|-------|------------------|
| `local` | `bambu:///local/{ip}.?port=6000&user=bblp&passwd=...` |
| `rtsps` | `bambu:///rtsps___bblp:{pwd}@{ip}/streaming/live/1?proto=rtsps` |
| `rtsp` | `bambu:///rtsp___bblp:{pwd}@{ip}/streaming/live/1?proto=rtsp` |

LAN 有 `rtsp_url` 时，也可以 **不经过 bridge**，VLC 直接打开该 RTSP。P1 一类常常只有 `local`（6000），必须走 BambuSource。

### `GET /camera.html?dev_id=`

浏览器预览（mpegts.js）。query 原样转给 `/camera.ts`。

```
http://127.0.0.1:8080/camera.html?dev_id=SERIAL
http://127.0.0.1:8080/camera.html?dev_id=SERIAL&ip=192.168.1.10&password=12345678&proto=local
```

### `GET /camera`

与 `/camera.ts` 相同。

失败时（缺 `dev_id`、换 URL 失败、没 BambuSource）返回 JSON：`{"ok":false,"error":"..."}`，不是 TS。

### `GET /camera.jpg`

每次请求打开摄像头，取一帧后关闭，不需要先开启直播。成功返回 `200`、`Content-Type: image/jpeg` 和 `Cache-Control: no-store`，响应体为 JPEG 文件内容。

使用与 `/camera.ts` 相同的参数：`dev_id` 必填；提供 `ip` 时走局域网，否则走云端（需先登录）。仅提供 BRTC/WebRTC 播放地址的云端摄像头不支持此接口，可尝试 LAN。

```bash
curl --fail 'http://127.0.0.1:8080/camera.jpg?dev_id=SERIAL' -o snapshot.jpg
curl --fail 'http://127.0.0.1:8080/camera.jpg?dev_id=SERIAL&ip=192.168.1.10&password=ACCESS_CODE&proto=local' -o snapshot.jpg
```

MJPEG 摄像头直接返回原始 JPEG；H.264 摄像头等待可独立解码的关键帧，由 bridge 在进程内解码并编码成 JPEG。两种模式均不需要 FFmpeg。

失败时返回 `application/json`（`{"ok":false,"error":"..."}`）：`400` 缺少 `dev_id`，`502` 相机连接或编解码失败，`503` 缺少 BambuSource，`504` 等待取帧超时。

---

## 播放器

VLC：

```
vlc http://127.0.0.1:8080/camera.ts?dev_id=SERIAL
```

ffmpeg / ffplay（低延迟）：

```
ffplay -fflags nobuffer -flags low_delay -i "http://127.0.0.1:8080/camera.ts?dev_id=SERIAL"
```

mpegts.js：

```html
<video id="v" controls autoplay muted></video>
<script src="https://cdn.jsdelivr.net/npm/mpegts.js@1.7.3/dist/mpegts.min.js"></script>
<script>
  const p = mpegts.createPlayer({
    type: "mse",
    isLive: true,
    url: "http://127.0.0.1:8080/camera.ts?dev_id=SERIAL"
  });
  p.attachMediaElement(document.getElementById("v"));
  p.load();
  p.play();
</script>
```

---

## RPC `get_camera_url`

只负责解析出 `bambu:///` 和可播的 HTTP 地址，**自己不推流**。真正播流仍是 `GET /camera.ts`。

```http
POST /rpc
POST /api/get_camera_url
```

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `dev_id` | string | | 序列号，必填 |
| `protocols` | string | `"tutk","agora"` | 云端协议 |
| `dev_ver` | string | | 固件版本 |
| `ip` | string | | 有则走 LAN |
| `user` | string | `bblp` | LAN |
| `password` | string | | LAN access code |
| `proto` | string | | LAN：`local` / `rtsp` / `rtsps` |

成功：

```json
{
  "ok": true,
  "result": {
    "code": 0,
    "url": "bambu:///...?uid=...&authkey=...",
    "play_url": "http://127.0.0.1:8080/camera.ts?dev_id=SERIAL"
  }
}
```

- `url`：给 BambuSource 的私有 URL，VLC 打不开  
- `play_url`：给播放器的 MPEG-TS（仅 HTTP 服务已启动时有）  
失败：`code=-1`，带 `error`

云端会先 `ensure_cloud_ready`（登录、订阅、等 MQTT）。

---

## 和 OrcaSlicer 的对应关系

| Orca | bridge |
|------|--------|
| `get_camera_url(serial + "\|" + ver + "\|\"tutk\",\"agora\"")` | 云端 `/camera.ts?dev_id=&dev_ver=` |
| `bambu:///local/{ip}.?port=6000&user=&passwd=` | `proto=local&ip=&password=` |
| `bambu:///rtsps___user:pwd@ip/streaming/live/1` | `proto=rtsps` |
| `wxMediaCtrl` / `gstbambusrc` 读 sample | `stream_camera_mpegts` 打成 TS |
| `get_camera_url_for_golive`（BRTC） | TUTK/Agora 都 disable 且 `brtc_service=enable` 时自动调用。若返回 `bambu:///`，与 TUTK 一样走 BambuSource → `/camera.ts`；若返回 `https://`/`webrtc://` 则无法封装成 TS |

云端插件回调的 `bambu:///` 还会补 `&device=` `&net_ver=` `&dev_ver=`（与 Orca `MediaPlayCtrl` 一致）。`refresh_url`（Agora token 刷新函数指针）未接，Agora 长连接可能中途失效，可重开 `/camera.ts`。

---

## 分辨率（MQTT，不是拉流）

```json
{"method":"send_message","params":{"dev_id":"SERIAL","json_str":"{\"camera\":{\"command\":\"ipcam_resolution_set\",\"resolution\":\"1080p\",\"sequence_id\":\"0\"}}"}}
```

`resolution` 常见 `720p` / `1080p`，以 `pushall` 里 `ipcam.resolution_supported` 为准。

---

## 限制

- 同一时刻多路 `/camera.ts` 会各开一条 BambuSource tunnel，机型通常只稳定支持一路预览  
- 没有音频  
- 没有 HLS 切片（`.m3u8`），没有 RTSP 监听端口  
- 首帧会等到关键帧，可能黑屏一两秒  
- Windows 必须用 MSVC 编的 bridge，且能 LoadLibrary 到 `BambuSource.dll`

---

## 排查

| 现象 | 原因 |
|------|------|
| `{"error":"missing dev_id"}` | query 没带序列号 |
| `BambuSource not found` / missing exports | `--plugin-dir` 没有 `BambuSource` |
| `not logged in` | 先 `GET /login` |
| `printer MQTT session not ready` | 机离线或未绑定；看 `list_printers` 的 `dev_online` |
| `get_camera_url failed` / URL 不是 `bambu:///` | 云端换票失败；试加 `dev_ver`；看 `tutk_server` 是否 disable |
| `error` 含 `403` | TUTK/Agora 被拒。本机会自动再试 `get_camera_url_for_golive`（BRTC）。若 `ipcam.tutk_server=disable` 且 `agora_service=disable`、`brtc_service=enable`，云端是 WebRTC，**不能**走 `/camera.ts` MPEG-TS，请用 LAN `?ip=&password=&proto=local` |
| VLC 打开 `bambu:///` 失败 | 必须用 `play_url` / `/camera.ts` |
| LAN 无画面 | `proto` 与固件不符：有 RTSP 用 `rtsps`，否则 `local`；密码用 access code |
| 浏览器不能播 | 用 mpegts.js 或改 VLC；原生 `<video src=.ts>` 不行 |
