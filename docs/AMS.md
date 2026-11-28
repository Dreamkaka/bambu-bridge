# AMS RPC

bambu-bridge **不解析 AMS 状态**，也不维护槽位缓存。状态从打印机 MQTT 来；控制命令按 OrcaSlicer / 打印机协议下发。

已封装 3 个日常 method：`ams_control`、`ams_change_filament`、`ams_user_setting`。其余（RFID、耗材信息、烘干、自动续料等）用 `send_message` / `printer_command` 发原始 JSON。

HTTP 两种调法等价：

```http
POST /rpc
{"method":"ams_change_filament","params":{...}}

POST /api/ams_change_filament
{...params...}
```

控制类默认 `wait=true`，等该机下一条 MQTT。共用字段（`dev_id` / `lan` / `wait` / `timeout_ms`）见 **[RPC.md](RPC.md)**。

---

## 编号

打印机同时用两套编号，混用会换错槽。

### 单元 id（`ams_id`）

MQTT 命令、`pushall` 的 `ams.ams[].id` 用这一套。

| `ams_id` | 含义 |
|----------|------|
| `0`–`3` | 普通 AMS / AMS 2 Pro / AMS Lite，最多 4 台 |
| `128`–`135` | AMS HT（单槽） |
| `255` | 外挂盘（主挤出 / 右） |
| `254` | 外挂盘（副挤出 / 左，双头机） |

普通 AMS 每台 4 槽，`slot_id` 为 `0`–`3`。AMS HT 只有槽 `0`。

### 扁平槽位（`tray_id` / `target`）

旧协议、`ams_mapping` v0、部分 G-code 用扁平下标：

```
普通 AMS:  tray_id = ams_id * 4 + slot_id     # 0–15
AMS HT:    tray_id = ams_id                   # 128–135（命令里）；打印映射里常见 16–23
外挂:      255 / 254
空 / 卸载: 255 或 -1
```

`ams_change_filament` 封装里：`ams_id < 16` 时按 `ams_id * 4 + slot_id` 算 `target`；否则 `target = ams_id`（覆盖 AMS HT 和外挂）。

### 当前进料

`pushall` 的 `print.ams.tray_now`（字符串数字）：

| 值 | 含义 |
|----|------|
| `255` | 无料 / 未进 AMS |
| `254` | 外挂盘 |
| `≥ 128` | AMS HT 单元 id |
| 其它 | `ams_id = tray_now >> 2`，`slot_id = tray_now & 3` |

新固件双头机看 `print.device.extruder.info[].snow`：低 4 bit 槽位，其余为 `ams_id`。

---

## 读状态

没有单独的 `get_ams`。先订阅再 `pushall`，从 `result.payload.print` 取。

```bash
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","timeout_ms":15000}'
```

后续变化走 SSE `/events`（`on_message` / `mqtt`）。

### `print.ams`

```json
{
  "ams": [
    {
      "id": "0",
      "humidity": "4",
      "humidity_raw": "45",
      "temp": "26.0",
      "dry_time": 0,
      "tray": [
        {
          "id": "0",
          "remain": 80,
          "tray_info_idx": "GFA00",
          "tray_type": "PLA",
          "tray_color": "FFFFFFFF",
          "tray_sub_brands": "",
          "nozzle_temp_min": "190",
          "nozzle_temp_max": "230",
          "tag_uid": "...",
          "k": 0.02,
          "n": 1.4
        }
      ]
    }
  ],
  "ams_exist_bits": "1",
  "tray_exist_bits": "f",
  "tray_is_bbl_bits": "f",
  "tray_now": "0",
  "tray_tar": "255",
  "tray_read_done_bits": "f",
  "tray_reading_bits": "0",
  "insert_flag": true,
  "power_on_flag": true,
  "version": 3
}
```

| 字段 | 含义 |
|------|------|
| `ams[].id` | 单元 id，字符串 |
| `ams[].humidity` | 湿度档 `1`–`5`（1 最干，5 最湿）。普通 AMS 只有档位 |
| `ams[].humidity_raw` | 相对湿度百分比；没有此字段时只显示湿度档 |
| `ams[].temp` | 仓温 ℃ |
| `ams[].dry_time` | 剩余烘干时间（秒，能烘干的型号） |
| `ams[].tray[]` | 四槽（HT 一槽）。空槽通常 `remain=-1` 且 `tray_type` 空 |
| `tray_exist_bits` | 槽位占用位图（十六进制字符串） |
| `tray_now` / `tray_tar` | 当前 / 目标扁平槽 |
| `insert_flag` | 插入时读 RFID（对应 `ams_user_setting.tray_read_option`） |
| `power_on_flag` | 开机读 RFID |

AMS 3H 可在 `ams[].zone_info[]` 中按 `zone_id` 上报分区环境。每区独立读取 `temp`、`humidity_raw`（百分比）或 `humidity`（档位），不要用整机读数填补缺失的分区读数。见 [Bambu Studio 的 AMS 解析](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/DeviceCore/DevFilaSystem.cpp)。

概览中的「已夹取」与 AMS 当前进料不是同一个状态：`print.device.nozzle.info[].id` 的 `0x0n` 表示挤出机位置，`0x1n` 表示机架位置；`addr_id` 用来保留同一工具头的槽位编号。`nozzle.exist` 和对应挤出机的 `info` 装载位用来排除空夹具，不使用 `src_id` 或 `tar_id` 判定已夹取。见 [喷嘴位置解析](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/DeviceCore/DevNozzleSystem.cpp) 和 [挤出机装载状态](https://github.com/bambulab/BambuStudio/blob/master/src/slic3r/GUI/DeviceCore/DevExtruderSystem.cpp)。

外挂盘在 `print.vt_tray`（单头）或 `print.vir_slot[]`（双头，`id` 为 `"255"` / `"254"`），字段与 AMS tray 相同。

`tray_color` 为 **RRGGBBAA**，不带 `#`，alpha 通常 `FF`。

### `print.ams_status`

整数：高字节主状态，低字节子步骤。

```
main = (ams_status >> 8) & 0xFF
sub  =  ams_status       & 0xFF
```

| main | 含义 |
|------|------|
| `0x00` | 空闲 |
| `0x01` | 换料中 |
| `0x02` | 读 RFID |
| `0x03` | 辅助进料 |
| `0x04` | 校准 |
| `0x10` | 自检 |
| `0x20` | 调试 |
| `0xFF` | 未知 |

换料中 `sub` 常见值：`0x02` 加热喷嘴 → `0x03` 剪断 → `0x04` 抽旧料 → `0x05`/`0x06` 推新料 → `0x07` 冲刷 → `0x08`/`0x0B` 确认位置。卡住时用 `ams_control` 的 `resume` / `done` / `abort`。

---

## 已封装 method

三个 method 都走 `print` MQTT。`dev_id` 必填。

### `ams_control`

换料流程的继续 / 取消，**不是**开关 AMS。

| 参数 | 类型 | 默认 |
|------|------|------|
| `action` 或 `param` | string | `"resume"` |

| action | 场景 |
|--------|------|
| `resume` | 进料失败后重试；弹窗点继续 |
| `done` | 手动确认已经进到位 |
| `pause` | 暂停换料 |
| `abort` | 取消换料 |
| `reset` | 复位 AMS 状态 |

内部：

```json
{"print":{"command":"ams_control","param":"resume","sequence_id":"20000"}}
```

```json
{ "method": "ams_control", "params": { "dev_id": "SERIAL", "action": "resume" } }
```

### `ams_change_filament`

加载或卸载。内部 `target` / `slot_id` 由 `load` + `ams_id` + `slot_id` 算出。

| 参数 | 类型 | 默认 |
|------|------|------|
| `load` | bool | `true`；`false` 卸载 |
| `ams_id` | number | 单元 id |
| `slot_id` | number | 槽 `0`–`3`；卸载时内部强制 `255` |
| `curr_temp` 或 `old_temp` | number | 当前喷嘴温度 |
| `tar_temp` 或 `new_temp` | number | 目标材料打印温度 |

加载时：

```
tray_id = ams_id < 16 ? ams_id * 4 + slot_id : ams_id
target  = (tray_id == 0) ? ams_id : tray_id
```

`tray_id == 0` 时 `target` 写成 `ams_id`（Orca 兼容，AMS0 槽0 时两者都是 0）。

卸载时：`target=255`，`slot_id=255`。

加载 AMS 0 槽 2（扁平 `target=2`），喷嘴目标 220：

```json
{
  "method": "ams_change_filament",
  "params": {
    "dev_id": "SERIAL",
    "load": true,
    "ams_id": 0,
    "slot_id": 2,
    "curr_temp": 220,
    "tar_temp": 220
  }
}
```

下发：

```json
{
  "print": {
    "command": "ams_change_filament",
    "sequence_id": "20001",
    "ams_id": 0,
    "slot_id": 2,
    "target": 2,
    "curr_temp": 220,
    "tar_temp": 220
  }
}
```

卸载：

```json
{
  "method": "ams_change_filament",
  "params": { "dev_id": "SERIAL", "load": false, "ams_id": 0, "slot_id": 0 }
}
```

外挂盘加载：`ams_id=255`，`slot_id=0`（双头左挤出用 `254`）。旧固件可能把外挂 `target` 写成 `254` 而不是 `255`；封装按 Orca 对 `ams_id≥16` 用 `target=ams_id`。若机型不认，用下面「未封装」自己拼 `json_str`。

温度建议取该槽 `nozzle_temp_min` 与 `nozzle_temp_max` 的中值。缺温度时打印机可能不加热或拒绝换料。

换料是长流程，一次 `wait` 只等到下一条 MQTT，不是等到进料完成。进度看后续 `ams_status` / `tray_now`。

### `ams_user_setting`

| 参数 | 类型 | 默认 |
|------|------|------|
| `ams_id` | number | `-1`（全部 AMS） |
| `startup_read_option` | bool | `true` 开机读 RFID |
| `tray_read_option` | bool | `true` 插入读 RFID |
| `calibrate_remain_flag` | bool | `true` 余料估计 |

```json
{
  "method": "ams_user_setting",
  "params": {
    "dev_id": "SERIAL",
    "startup_read_option": true,
    "tray_read_option": true,
    "calibrate_remain_flag": false
  }
}
```

---

## 未封装命令

`json_str` 必须是**整段 JSON 字符串**（对象再 stringify 一次）。`sequence_id` 可自己填，打印机主要用来对回报。

云端：

```json
{ "method": "send_message", "params": { "dev_id": "SERIAL", "json_str": "{...}" } }
```

局域网：`send_message_to_printer`，或 `printer_command` 加 `"lan": true`。

### 读 RFID

新固件：

```json
{"print":{"command":"ams_get_rfid","ams_id":0,"slot_id":0,"sequence_id":"1"}}
```

旧固件 G-code（扁平槽位）：

```json
{"print":{"command":"gcode_line","param":"M620 R2\n","sequence_id":"1"}}
```

选槽：`M620 P<tray>\n`。校准：`M620 C<ams_id>\n`。也可走封装 `gcode_line`。

### 写槽位耗材

官方耗材改 RFID 会自动更新；第三方或清空槽要自己写。

```json
{
  "print": {
    "command": "ams_filament_setting",
    "sequence_id": "1",
    "ams_id": 0,
    "slot_id": 0,
    "tray_id": 0,
    "tray_info_idx": "GFA00",
    "setting_id": "",
    "tray_color": "FFFFFFFF",
    "tray_type": "PLA",
    "nozzle_temp_min": 190,
    "nozzle_temp_max": 230
  }
}
```

| 字段 | 说明 |
|------|------|
| `tray_info_idx` | 耗材 profile id，如 `GFA00`（PLA Basic）、`GFB99`（ABS） |
| `setting_id` | 切片预设 id，可空 |
| `tray_color` | RRGGBBAA |
| `tray_type` | `PLA` / `PETG` / `ABS` / `TPU` 等 |
| `tray_id` | 普通 AMS 用 `slot_id`；外挂盘 Orca 写成 `254` |

清空槽：`tray_info_idx`、`tray_type` 置空，颜色随意。

### 烘干（AMS 2 Pro / AMS HT）

开始（HA 协议，`mode=1`）：

```json
{
  "print": {
    "command": "ams_filament_drying",
    "sequence_id": "1",
    "ams_id": 0,
    "temp": 55,
    "cooling_temp": 45,
    "duration": 8,
    "humidity": 0,
    "mode": 1,
    "rotate_tray": false,
    "filament": "",
    "close_power_conflict": false
  }
}
```

| 字段 | 说明 |
|------|------|
| `temp` | 烘干温度。AMS 2 Pro 约 45–65℃，AMS HT 可到 85℃ |
| `cooling_temp` | 至少 `45`，否则命令会被忽略 |
| `duration` | 小时，约 1–24 |
| `mode` | `1` 开始，`0` 停止 |
| `rotate_tray` | 是否转盘（看型号） |

停止也可以：

```json
{"print":{"command":"auto_stop_ams_dry","sequence_id":"1"}}
```

（Orca 用这条。旧文档里的 `ams_drying_stop` 打印机不认。）

进度：`ams[].dry_time`、`ams[].dry_setting`。

### 自动续料 / 空打检测

走 `print_option`，不是 `ams_*`：

```json
{"print":{"command":"print_option","auto_switch_filament":true,"sequence_id":"1"}}
{"print":{"command":"print_option","air_print_detect":true,"sequence_id":"1"}}
```

---

## 发打印时的 AMS 映射

`start_print` / `start_local_print` 的 params 对应插件 `PrintParams`（字符串，不是 MQTT 数组）。

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_use_ams` | bool | 默认 `false`。不用 AMS 时不要开 |
| `ams_mapping` | string | v0：JSON 数组，每个切片耗材对应扁平 `tray_id`，外挂为 `-1`。例 `"[0,1,2,-1]"` |
| `ams_mapping2` | string | v1：`[{"ams_id":0,"slot_id":1}, ...]`，外挂 `ams_id=255, slot_id=255` |
| `ams_mapping_info` | string | 带颜色 / 类型的映射明细，Orca 发给云端任务用 |

下标与切片耗材列表对齐：第 0 个元素是耗材 1 用哪槽。

MQTT 直发 `project_file`（不经插件 `start_print`）时字段不同：`use_ams` 为 bool，`ams_mapping` 为**数字数组**而非字符串。需要的话用 `send_message` 自己拼。

---

## 调用示例

```bash
# 状态
curl -s -X POST http://127.0.0.1:8080/api/pushall \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","timeout_ms":15000}'

# 加载 AMS1 槽 0（扁平 target=4）
curl -s -X POST http://127.0.0.1:8080/api/ams_change_filament \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","ams_id":1,"slot_id":0,"curr_temp":220,"tar_temp":220}'

# 卸载
curl -s -X POST http://127.0.0.1:8080/api/ams_change_filament \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","load":false,"ams_id":0}'

# 换料卡住后继续
curl -s -X POST http://127.0.0.1:8080/api/ams_control \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","action":"resume"}'

# 读 RFID（未封装）
curl -s -X POST http://127.0.0.1:8080/api/send_message \
  -H "Content-Type: application/json" \
  -d '{"dev_id":"SERIAL","json_str":"{\"print\":{\"command\":\"ams_get_rfid\",\"ams_id\":0,\"slot_id\":1,\"sequence_id\":\"1\"}}"}'
```

stdio 同样一行一个 JSON。

---

## 注意

- 不是每台机器都认每一条。P1 / A1 / X1 / H2D、普通 AMS / Lite / 2 Pro / HT 字段不同。不支持时 `code` 仍可能是 `0`，看 `payload` 有没有变化。
- 打印中换料可能被拒绝，或走 AMS 自己的换色流程而不是手动 `ams_change_filament`。
- 旧固件没有 `ams_change_filament` / `ams_get_rfid`，Orca 会退回 `M620`。不确定时先 `pushall` 看 `print.ams` 是否存在。
- 封装只覆盖 Orca 的常用路径。双头外挂、AMS HT 烘干、云端任务映射若行为不对，用 `json_str` 对照 Orca `MachineObject::command_ams_*` 或 HA `pybambu/commands.py`。
