# bambu-bridge RPC 参数手册

本文列出当前全部 `method`、参数、返回值。HTTP 两种调法等价：

```http
POST /rpc
{"method":"<name>","params":{...}}

POST /api/<name>
{...params...}
```

stdio：一行一个 `{"method":"...","params":{...}}`。

摄像头拉流（`/camera.ts`、LAN/云端、VLC）见 **[CAMERA.md](CAMERA.md)**。

---

## 能力边界（先读）

**通道已经打通。** 登录、订阅、发 MQTT 之后，理论上能控制打印机支持的全部 MQTT 功能。

分两层：

1. **已封装 method**：参数写成普通 JSON 字段，bridge 拼成打印机协议。日常控制用这些。
2. **万能出口** `send_message` / `send_message_to_printer` / `printer_command`：`json_str` 原样下发。OrcaSlicer 有、这里没封装的命令走这条。

不是每个机型都认每一条命令。P1/X1/A1/H2D 字段不同；不支持时通常仍返回 `code=0`，但 `payload` 无对应变化。

控制前必须：

1. 已登录（`is_user_login=true`）
2. 云端：`add_subscribe({dev_list:[serial]})`
3. LAN：`connect_printer` 成功

---

## 通用约定

### 请求

```json
{ "method": "pushall", "params": { "dev_id": "SERIAL" } }
```

缺 `params` 当作 `{}`。

### 响应

成功：`{ "ok": true, "result": ... }`  
分发失败：`{ "ok": false, "error": "unknown method: ..." }`

插件返回码在 `result` 里，**不会**把外层 `ok` 改成 false。`result` 为数字时，`0` 成功，负数失败。

### 控制类 `result`（默认 `wait=true`）

```json
{
  "ok": true,
  "result": {
    "code": 0,
    "timed_out": false,
    "dev_id": "SERIAL",
    "source": "cloud",
    "msg": "{...原始字符串...}",
    "payload": { }
  }
}
```

| 字段 | 含义 |
|------|------|
| `code` | 发出去的插件返回码，`0` 已发出 |
| `timed_out` | 超时没等到该机下一条 MQTT |
| `source` | `cloud` / `lan` |
| `msg` | 原始 JSON 字符串 |
| `payload` | `msg` 再 parse 后的对象，优先用这个 |
| `error` | 仅超时时有 |

`wait=false` 时只返回 `{ "code": 0 }`，回报走 SSE `/events`。

### 所有控制命令共用字段

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `dev_id` | string | | 打印机序列号，必填 |
| `lan` | bool | `false` | `true` 走局域网 MQTT |
| `wait` | bool | `true` | 等该机下一条消息 |
| `timeout_ms` | number | `8000` | 等待超时 |
| `qos` | number | `0` | MQTT QoS |
| `flag` | number | `0` | 消息标志 |

下面表格只写各方法的**特有参数**。

### 云端 HTTP 类 `result`

```json
{ "code": 0, "http_code": 200, "http_body": "{...字符串...}" }
```

`http_body` 要再 `JSON.parse` 一次。

---

## 1. 生命周期 / 状态

| method | 参数 | result |
|--------|------|--------|
| `status` | 无 | `{loaded, agent_ready, version, logged_in, server_connected, user_id, user_name, country, host}` |
| `version` | 无 | 插件版本字符串 |
| `init_agent` | 无 | `{code}`，启动时已调用 |
| `create_agent` | `log_dir` string | int |
| `destroy_agent` | 无 | int |
| `set_country_code` | `country_code` string `CN`/`US` | int |
| `set_config_dir` | `config_dir` string | int |
| `set_cert_file` | `folder` string，`filename` string | int |
| `init_log` | 无 | int |
| `start` | 无 | int |
| `load_plugin` | 无 | `{}` 占位，真正加载只在进程启动 |

---

## 2. 登录 / 账号

| method | 参数 | result |
|--------|------|--------|
| `get_login_url` | 无 | `{host, url}` |
| `login_with_ticket` | `ticket` string | `{code, logged_in, user_id, user_name, error?}` |
| `change_user` | `user_info` string；空则把整个 params 当 payload | int |
| `is_user_login` | 无 | bool |
| `user_logout` | `request` bool 默认 `true` | int |
| `get_user_id` / `get_user_name` / `get_user_avatar` / `get_user_nickname` | 无 | string |
| `get_bambulab_host` | 无 | string |
| `get_studio_info_url` | 无 | string |
| `build_login_cmd` / `build_login_info` / `build_logout_cmd` | 无 | string，WebView 用 |
| `connect_server` / `refresh_connection` | 无 | int |
| `is_server_connected` | 无 | bool |
| `get_user_selected_machine` | 无 | string |
| `set_user_selected_machine` | `dev_id` string | int |
| `get_my_token` | `ticket` string | `{code, http_code, http_body}` |
| `get_my_profile` | `token` string | `{code, http_code, http_body}` |

`change_user` 会规范化成：

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

成功后自动 `start_subscribe("app")` + `connect_server`。浏览器登录走 `GET /login` 即可，不必自己调 ticket。

---

## 3. 设备列表 / 云端读取

| method | 参数 | result |
|--------|------|--------|
| `list_printers` 别名 `get_user_print_info` | 无 | `{code, http_code, http_body}` |
| `get_user_tasks` | `dev_id` string，`status` number 默认 0，`offset` 默认 0，`limit` 默认 20 | `{code, http_body}` |
| `get_printer_firmware` | `dev_id` | `{code, http_code, http_body}` |
| `get_slice_info` | `project_id`，`profile_id`，`plate_index` 默认 0 | `{code, slice_json}` |
| `get_subtask_info` | `subtask_id` | `{code, task_json, http_code, http_body}` |
| `get_camera_url` | 见下 | `{code, url, play_url}`。`bambu:///` 不能进 VLC，播 `play_url` / `/camera.ts` |
| `get_model_mall_detail_url` | `id` 模型 id | `{code, url}` |

`list_printers` 的 `http_body` 解析后：

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

### `get_camera_url`

只换票，不推流。播视频用返回的 `play_url` 或直接 `GET /camera.ts`。完整说明见 **[CAMERA.md](CAMERA.md)**。

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | 必填 |
| `protocols` | string | `"tutk","agora"` |
| `dev_ver` | string | 固件版本 |
| `ip` / `user` / `password` / `proto` | string | 有 `ip` 则 LAN；`proto`=`local`/`rtsp`/`rtsps` |

```json
{ "method": "get_camera_url", "params": { "dev_id": "SERIAL" } }
```

`result.url` 是 `bambu:///`（VLC 不能播），`result.play_url` 是 MPEG-TS。

---

## 4. 订阅

| method | 参数 | result |
|--------|------|--------|
| `start_subscribe` | `module` string 默认 `"app"` | int |
| `stop_subscribe` | 同上 | int |
| `add_subscribe` | `dev_list` string[] | int |
| `del_subscribe` | `dev_list` string[] | int |
| `enable_multi_machine` | `enable` bool 默认 `true` | `0` |

云端控制前必须 `add_subscribe`。登录后已自动 `start_subscribe("app")`。

---

## 5. 万能 MQTT 出口

这三条是底层通道。`json_str` 必须是**字符串**，不要直接塞对象。

### `send_message`（云端）

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `json_str` | string | |
| `qos` / `flag` / `wait` / `timeout_ms` | 见共用字段 | |

### `send_message_to_printer`（LAN，插件签名）

参数同 `send_message`。

### `printer_command`

| 参数 | 类型 | 默认 |
|------|------|------|
| `dev_id` | string | |
| `json_str` | string | |
| `lan` | bool | `false` → 云端；`true` → LAN |

示例：关舱灯（未单独封装的写法）

```json
{
  "method": "send_message",
  "params": {
    "dev_id": "SERIAL",
    "json_str": "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"off\",\"led_on_time\":500,\"led_off_time\":500,\"loop_times\":0,\"interval_time\":0}}"
  }
}
```

封装过的等价：`set_light` / `ledctrl`。

---

## 6. 已封装的打印机控制

全部带共用字段 `dev_id` / `lan` / `wait` / `timeout_ms`。`result` 为控制类对象，看 `payload`。

### 6.1 状态 / 任务

#### `pushall`

无特有参数。要全量状态。

内部：

```json
{ "pushing": { "sequence_id": "0", "command": "pushall", "version": 1, "push_target": 1 } }
```

```bash
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL"}'
```

#### `pause` / `resume` / `stop`

无特有参数。`stop` 等于中止任务。

内部：`{"print":{"command":"pause|resume|stop","param":""}}`

#### `get_version`

无特有参数。内部：`{"info":{"command":"get_version"}}`

#### `get_access_code`

无特有参数。内部：`{"system":{"command":"get_access_code"}}`

#### `skip_objects`

| 参数 | 类型 | 说明 |
|------|------|------|
| `obj_list` | number[] | 要跳过的零件 id |

内部：`{"print":{"command":"skip_objects","obj_list":[...]}}`

---

### 6.2 温度 / 风扇 / 归零

#### `set_bed_temp`

| 参数 | 类型 | 说明 |
|------|------|------|
| `temp` | number | 热床目标温度 ℃ |

内部：`{"print":{"command":"set_bed_temp","temp":N}}`  
部分旧固件只认 G-code `M140`，不生效时改用 `gcode_line`。

#### `set_nozzle_temp`

| 参数 | 类型 | 默认 |
|------|------|------|
| `temp` 或 `target_temp` | number | |
| `extruder_index` | number | `0`，双头机副挤出机用 `1` |

内部：`{"print":{"command":"set_nozzle_temp","extruder_index":0,"target_temp":N}}`

#### `set_chamber_temp`

| 参数 | 类型 |
|------|------|
| `temp` 或 `ctt_val` | number，腔温 ℃ |

内部：`{"print":{"command":"set_ctt","ctt_val":N}}`  
仅支持腔温控制的机型。

#### `set_fan`

| 参数 | 类型 | 说明 |
|------|------|------|
| `fan_index` 或 `fan_id` | number | 风扇 id |
| `speed` | number | 0–255，部分固件 0–15 |

常见 `fan_index`（因机型而异）：

| 值 | 风扇 |
|----|------|
| `1` | 零件冷却 |
| `2` | 辅助 / 散热 |
| `3` | 腔温 / 过滤 |

内部：`{"print":{"command":"set_fan","fan_index":1,"speed":255}}`

#### `home` / `go_home`

无特有参数。内部：`{"print":{"command":"back_to_center"}}`  
不支持 MQTT 归零的机型用 `gcode_line` 发 `G28`。

---

### 6.3 灯光

#### `ledctrl` 别名 `set_light`

打印机对灯光 JSON **字段齐全** 才接受，缺 `led_on_time` 等会回 `json wrong format`。

| 参数 | 类型 | 默认 |
|------|------|------|
| `led_node` | string | `"chamber_light"` |
| `led_mode` 或 `mode` | string | `"on"` |
| `led_on_time` | number | `500` ms |
| `led_off_time` | number | `500` |
| `loop_times` | number | `1`（不少机型开关灯用 `0`） |
| `interval_time` | number | `1000`（开关灯常用 `0`） |

`led_mode`：`on` / `off` / `flashing`  
`led_node`：`chamber_light`、`chamber_light2`（双灯）、`heatbed_light`

封装调用：

```json
{ "method": "set_light", "params": { "dev_id": "SERIAL", "mode": "on", "loop_times": 0, "interval_time": 0 } }
```

下发的 MQTT 必须是：

```json
{
  "system": {
    "sequence_id": "0",
    "command": "ledctrl",
    "led_node": "chamber_light",
    "led_mode": "on",
    "led_on_time": 500,
    "led_off_time": 500,
    "loop_times": 0,
    "interval_time": 0
  }
}
```

自己拼 `send_message` 的 `json_str` 时同样要带齐这 8 个字段。双灯机再对 `chamber_light2` 发一条。

---

### 6.4 G-code

#### `gcode_line` 别名 `send_gcode`

| 参数 | 类型 |
|------|------|
| `gcode` 或 `param` | string，一行或多行，建议以 `\n` 结尾 |

内部：`{"print":{"command":"gcode_line","param":"G28 \\n"}}`

例：

```json
{ "method": "gcode_line", "params": { "dev_id": "SERIAL", "gcode": "M140 S60\n" } }
```

```json
{ "method": "gcode_line", "params": { "dev_id": "SERIAL", "gcode": "G90\nG1 X10 F3000\n" } }
```

打印中部分 G-code 会被打印机拒绝。

---

### 6.5 AMS

#### `ams_control`

| 参数 | 类型 | 默认 |
|------|------|------|
| `action` 或 `param` | string | `"resume"` |

合法：`resume` / `reset` / `pause` / `done` / `abort`

#### `ams_change_filament`

| 参数 | 类型 | 默认 |
|------|------|------|
| `load` | bool | `true`；`false` 卸载 |
| `ams_id` | number | AMS 编号，外挂从 0 |
| `slot_id` | number | 槽位 0–3 |
| `curr_temp` 或 `old_temp` | number | 当前喷嘴温度 |
| `tar_temp` 或 `new_temp` | number | 目标材料打印温度 |

`load=false` 时内部 `target=255, slot_id=255`。

#### `ams_user_setting`

| 参数 | 类型 | 默认 |
|------|------|------|
| `ams_id` | number | `-1` 全部 AMS |
| `startup_read_option` | bool | `true` 开机读 RFID |
| `tray_read_option` | bool | `true` 插入读 RFID |
| `calibrate_remain_flag` | bool | `true` 余料估计 |

---

### 6.6 速度 / 相机

#### `print_speed`

| 参数 | 类型 | 默认 |
|------|------|------|
| `level` 或 `param` | number 或 string | `2` |

| 值 | 档位 |
|----|------|
| `1` | 静音 |
| `2` | 标准 |
| `3` | 运动 |
| `4` | 狂暴 |

内部 `param` 是字符串 `"1"`–`"4"`。

#### `ipcam_record`

| 参数 | 类型 | 默认 |
|------|------|------|
| `enable` | bool | `true` |

打印中录像。内部 `control`: `enable` / `disable`。

#### `ipcam_timelapse`

| 参数 | 类型 | 默认 |
|------|------|------|
| `enable` | bool | `true` |

延时摄影，需要存储介质。

---

## 7. 局域网

| method | 参数 | result |
|--------|------|--------|
| `connect_printer` | `dev_id`，`dev_ip`，`username` 默认 `bblp`，`password` access code，`use_ssl` 默认 `true` | int |
| `disconnect_printer` | 无 | int |
| `start_discovery` | `start` 默认 `true`，`sending` 默认 `false` | bool |
| `install_device_cert` | `dev_id`，`lan_only` 默认 `false` | `0` |

LAN 连接结果在 SSE `on_local_connect`：`status=0` 成功，`msg=="5"` 密码错。连上后控制用 `lan: true`。

---

## 8. 云端 / 本地发打印

`start_print` / `start_local_print` 的 params 直接映射插件 `PrintParams`。**同步阻塞**，进度走 SSE `print_status`。`result` 为 int。

| 字段 | 类型 | 说明 |
|------|------|------|
| `dev_id` | string | |
| `task_name` | string | |
| `project_name` | string | |
| `preset_name` | string | |
| `filename` | string | 3mf / gcode 路径 |
| `config_filename` | string | |
| `plate_index` | number | |
| `ftp_folder` / `ftp_file` / `ftp_file_md5` | string | |
| `nozzle_mapping` | string | |
| `ams_mapping` / `ams_mapping2` / `ams_mapping_info` | string | |
| `nozzles_info` | string | |
| `connection_type` | string | `lan` / `cloud` 等 |
| `comments` | string | |
| `origin_profile_id` / `stl_design_id` | number | |
| `origin_model_id` | string | |
| `print_type` | string | |
| `dst_file` | string | |
| `dev_name` / `dev_ip` | string | LAN 必填 ip |
| `use_ssl_for_ftp` | bool | 默认 `false` |
| `use_ssl_for_mqtt` | bool | 默认 `true` |
| `username` / `password` | string | LAN |
| `task_bed_leveling` | bool | 默认 `true` |
| `task_flow_cali` | bool | 默认 `true` |
| `task_vibration_cali` | bool | 默认 `true` |
| `task_layer_inspect` | bool | 默认 `true` |
| `task_record_timelapse` | bool | 默认 `false` |
| `task_use_ams` | bool | 默认 `false` |
| `task_bed_type` | string | |
| `extra_options` | string | |
| `auto_bed_leveling` / `auto_flow_cali` / `auto_offset_cali` | number | |
| `extruder_cali_manual_mode` | number | 默认 `-1` |
| `task_ext_change_assist` | bool | |
| `try_emmc_print` | bool | |

---

## 9. 未封装、用 `json_str` 发的命令

下面来自 OrcaSlicer `MachineObject::command_*`。用 `send_message`（云）或 `send_message_to_printer`（LAN），`json_str` 为整段 JSON 字符串。

### 轴 / 挤出

```json
{"print":{"command":"xyz_ctrl","axis":"X","dir":1,"mode":0,"sequence_id":"1"}}
```

`axis`: `X`/`Y`/`Z`；`dir`: `1` 正 / `-1` 负；`mode`: `0` 微动 / `1` 长动（\|位移\|≥10）。

旧固件用 G-code：

```json
{"print":{"command":"gcode_line","param":"G91\nG1 X10 F3000\n","sequence_id":"1"}}
```

挤出长度：

```json
{"print":{"command":"set_extrusion_length","extruder_index":0,"length":10,"sequence_id":"1"}}
```

绝对坐标 / 自动调平：`gcode_line` 发 `G90\n` / `G29\n`。

### AMS 扩展

读 RFID：

```json
{"print":{"command":"ams_get_rfid","ams_id":0,"slot_id":0,"sequence_id":"1"}}
```

槽位耗材信息：

```json
{
  "print": {
    "command": "ams_filament_setting",
    "ams_id": 0, "slot_id": 0, "tray_id": 0,
    "tray_info_idx": "GFA00", "setting_id": "...",
    "tray_color": "FFFFFFFF", "tray_type": "PLA",
    "nozzle_temp_min": 190, "nozzle_temp_max": 230,
    "sequence_id": "1"
  }
}
```

停止烘干：`{"print":{"command":"ams_drying_stop","sequence_id":"1"}}`  
RFID 刷新 G-code：`M620 R<slot>\n`；选槽：`M620 P<tray>\n`；校准：`M620 C<ams_id>\n`。

### 升级 / 错误 / 蜂鸣器

```json
{"upgrade":{"command":"upgrade_confirm","src_id":1,"sequence_id":"1"}}
{"upgrade":{"command":"consistency_confirm","src_id":1,"sequence_id":"1"}}
{"upgrade":{"command":"start","url":"...","module":"...","version":"...","src_id":1,"sequence_id":"1"}}
{"print":{"command":"clean_print_error","subtask_id":"...","print_error":0,"sequence_id":"1"}}
{"print":{"command":"buzzer_ctrl","mode":0,"sequence_id":"1"}}
```

HMS：`resume` / `ignore` / `stop` / `idle_ignore`，带 `err`、`job_id`。

### 相机分辨率 / xcam

```json
{"camera":{"command":"ipcam_resolution_set","resolution":"1080p","sequence_id":"1"}}
{"xcam":{"command":"xcam_control_set","module_name":"first_layer_inspector","control":true,"enable":true,"print_halt":true,"sequence_id":"1"}}
```

`module_name` 常见：`printing_monitor`、`spaghetti_detector`、`buildplate_marker_detector`、`first_layer_inspector`、`auto_recovery_step_loss`。灵敏度字段 `halt_print_sensitivity`。

### 打印选项 / 检测开关

```json
{"print":{"command":"print_option","option":1,"auto_recovery":true,"sequence_id":"1"}}
{"print":{"command":"nozzle_blob_detect","nozzle_blob_detect":true,"sequence_id":"1"}}
{"print":{"command":"print_option","sound_enable":true,"sequence_id":"1"}}
```

具体布尔字段因固件而异，以 `pushall` 的 `payload.print` 为准。

### 校准

```json
{"print":{"command":"calibration","vibration":true,"bed_leveling":true,"xcam_cali":true,"motor_noise":false,"nozzle_cali":false,"sequence_id":"1"}}
```

PA / 流量校准字段较多，直接对照 OrcaSlicer `DeviceManager.cpp` 里 `command_start_pa_calibration` 等。

### 风道（H2D 等）

```json
{"print":{"command":"set_airduct","modeId":0,"submode":0,"sequence_id":"1"}}
```

---

## 10. 方法速查

**状态 / 账号：** `status` `version` `is_user_login` `get_user_id` `get_user_name` `list_printers` `get_login_url`

**订阅：** `add_subscribe` `del_subscribe` `start_subscribe`

**状态与任务：** `pushall` `pause` `resume` `stop` `get_version` `get_access_code` `skip_objects`

**温度风扇灯光：** `set_bed_temp` `set_nozzle_temp` `set_chamber_temp` `set_fan` `set_light` `home`

**AMS：** `ams_control` `ams_change_filament` `ams_user_setting`

**其它：** `gcode_line` `print_speed` `ipcam_record` `ipcam_timelapse`

**底层：** `send_message` `send_message_to_printer` `printer_command`

**LAN：** `connect_printer` `disconnect_printer`

**打印：** `start_print` `start_local_print`

---

## 11. 最小控制示例

```bash
# 1. 登录后列出打印机
curl -s -X POST http://127.0.0.1:8080/api/list_printers -d "{}"

# 2. 订阅
curl -s -X POST http://127.0.0.1:8080/api/add_subscribe \
  -H "Content-Type: application/json" \
  -d '{"dev_list":["SERIAL"]}'

# 3. 拉全量状态（result.payload）
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","timeout_ms":15000}'

# 4. 关灯 / 热床 60℃ / 暂停
curl -s -X POST http://127.0.0.1:8080/api/set_light \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","mode":"off"}'

curl -s -X POST http://127.0.0.1:8080/api/set_bed_temp \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","temp":60}'

curl -s -X POST http://127.0.0.1:8080/api/pause \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL"}'
```

HTTP 协议、事件、登录流程见 [API.md](API.md)。
