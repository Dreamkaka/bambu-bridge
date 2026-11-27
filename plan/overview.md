# Bambu Bridge WebUI — 概览页面（Overview）设计文稿

&gt; \*\*文档版本\*\*：v1.0  

&gt; \*\*适用对象\*\*：Bambu Bridge 插件前端 / 后端开发  

&gt; \*\*核心定位\*\*：融合《明日方舟：终末地》工业机能风视觉，打造兼具\*\*多喷嘴/换头硬件监控（适配 H3C / H2C / H2D / X2D）\*\*与\*\*API 网关可观测性\*\*的一屏式战术控制台。

---

## 1. 设计目标与核心原则

1. \*\*一屏总览，零无效滚动\*\*：桌面端（`≥1440px`）采用 12 栅格 Bento 战术面板，关键遥测、实时图传、工具头矩阵、AMS 供料与 Bridge 接口流同屏呈现。

2. \*\*能力驱动UI（Capability-Driven UI）\*\*：根据拉取的数据字段，动态判断机型并切换 \*\*H3C（9 工具头 SwapTek + AMS 3H）\*\*、\*\*H2C（1+6 Vortek）\*\*、\*\*H2D/X2D（双喷嘴 L/R）\*\* 与 \*\*其他传统单头机器\*\* 的面板形态。

3. \*\*秩序感高于装饰性\*\*：借鉴终末地“高对比度工业终端”语言，所有网格线、45° 切角、微标签均服务于信息分组；动态动效仅用于表达\*\*真实硬件状态\*\*（如 H3C 后台预热脉冲、活跃喷嘴高亮、HMS 故障警戒）。

4. \*\*网关原生体验\*\*：将 Bridge 的接口健康度、消费端接入状态及字段路径透视直接融入主面板，服务于插件接入方与开发者调试。

---

## 2. 视觉规范（Design Tokens）

### 2.1 色彩系统（暗色战术终端默认主题）

**下面给你的颜色仅供参考，实际以skill为准**


| Token 名称 | 色值 | 用途定义 |

| :--- | :--- | :--- |

| `--bg-canvas` | `#0E1012` | 页面底层画布，叠加 `24px` 1px 暗线工程网格（`rgba(255,255,255,0.03)`） |

| `--bg-panel` | `#16191D` | 主模块卡片底色 |

| `--bg-elevated` | `#1E2228` | 卡片内嵌子槽位（如工具头格、AMS 槽位）底色 |

| `--border-struct` | `#2B3038` | 1px 结构线框、分割线 |

| `--accent-endfield` | `#FFDE00` | \*\*工程警示黄\*\*：最高优先级引导（当前活跃工具头、进度条、关键开关、DEV 模式高亮） |

| `--accent-amber` | `#F59E0B` | \*\*热力琥珀橙\*\*：预热中状态、85°C 主动烘干区、65°C 腔体加热风道 |

| `--accent-cyan` | `#38BDF8` | \*\*冷流冰蓝\*\*：冷却风道模式、网络/WebSocket 遥测链路 |

| `--status-ok` | `#22C55E` | 在线、API 200 OK、MQTT 连接正常 |

| `--status-danger` | `#EF4444` | 离线、急停（E-STOP）、HMS 严重报错、API 异常 |

| `--text-primary` | `#F4F4F5` | 核心数据数值、主标题 |

| `--text-secondary` | `#8E95A2` | 英文微标签（Micro-labels）、次要描述、单位 |

### 2.2 几何与工业装饰语言



\* \*\*45° 切角（Chamfer）\*\*：主容器卡片采用左上/右下 `10px` 45° 斜切角；内部子槽位保持 `0px`\~`2px` 锐利直角。

\* \*\*四角准星锚点（`+`）\*\*：用于实时光学监控视窗四角与空置状态区，强化“设备终端”视觉。

\* \*\*斜向警示斑马纹（Hazard Stripe）\*\*：`-45deg` 黑黄交替条纹，\*\*严格限定\*\*于 HMS 故障告警横幅、急停确认条与暂停状态提示。

以上仅供参考，若skill有提到相关方案以skill为准



### 2.3 字体排印（Typography）

\* \*\*中文正文\*\*：`HarmonyOS Sans SC`, `PingFang SC`, `Noto Sans SC`, sans-serif

\* \*\*遥测数值 / 状态码\*\*：`JetBrains Mono`, `DIN Alternate`, monospace（全局强制 `font-variant-numeric: tabular-nums;`，杜绝数字跳动引起的布局抖动）

\* \*\*工业微标签\*\*：全大写英文 + `letter-spacing: 0.08em`，字号 `10px`\~`11px`（如 `SYS_BRIDGE // ONLINE`、`SWAPTEK_RACK`）。

---

## 3. 空间布局与线框图（12 栅格 Bento 架构）

### 3.1 桌面端标准骨架（以用户的 H3C 为主展示态）

\`\`\`text

+---------------------------------------------------------------------------------------------+

| [UNIT_01: H3C ▼]  ● ONLINE | MQTT: 11ms | API: 32 req/s | CLIENTS: 3 | [DEV_INSPECT] [E-STOP]| &lt;- Header (52px)

+--------------------------------+-----------------------------------------+------------------+

| [ CAM_01 // LIVE_RTSP ]        | [ SWAPTEK_MATRIX // 9-TOOL_RACK ]       | [ BRIDGE_CORE ]  |

| +----------------------------+ |  [T01][T02][T03★][T04⚡][T05]...[T09]   | ● REST   :8080   |

| | +                        + | |   0.4  0.4  0.4   0.6  0.2      FLEX    | ● WS     3 Conn  |

| |        (光学监控画面)       | |  26°  28°  220°  175°  25°      24°     | ● MQTT   TLS     |

| | +   [补光灯] [截图] [全屏] + | |  [▲ RACK_UP] [▼ RACK_DOWN]            |------------------|

| +----------------------------+ |-----------------------------------------| [ CONSUMERS ]    |

|                                | [ THERMAL & AIRDUCT // 65°C_MAX ]       | > HomeAssistant  |

| [ ACTIVE_JOB // 作业进度 ]      |  热床: 60.0/60°C [曲线] | 腔温: 54/55°C   |   POST /light OK |

|  [缩略图]  bracket_v3.3mf      |  风道: [HEATING_LOOP]                   | > Spoolman_Sync  |

|  68.4%   [████████████░░░░░░]  |  风扇: PART 80% | L-AUX 30% | R-AUX 30% |   WS_EVT (4ms)   |

|  LAYER: 142/310  ETA: 14:22    |        EXHAUST 0% | CHAMBER 65%         |------------------|

|  [静音] [标准★] [运动] [狂暴]   |-----------------------------------------| [ LIVE_STREAM ]  |

|  [ 暂停作业 ]     [ 终止任务 ]  | [ AMS_3H // DUAL-ZONE_SUPPLY ]          | 03:12:01 [EVT]   |

|                                |  ZONE_A: 24°C/18% | ZONE_B(DRY): 75°C   | TOOL_CHG 01-&gt;03  |

|                                |  [S6][S7]        [DRY_8★][DRY_9]        | 03:12:04 [TX]    |

|                                |  [S1][S2][S3★][S4][S5]                  | SET_NOZZLE 220°C |

+--------------------------------+-----------------------------------------+------------------+

          左栏 (约 34%)                           中栏 (约 41%)                 右栏 (约 25%)

\`\`\`

### 3.2 响应式断点策略

\* `≥1440px`：完整三栏布局（左 4 栅格 / 中 5 栅格 / 右 3 栅格），一屏铺满。

\* `1024px ~ 1439px`：左（图传+作业）与中（硬件+AMS）保持两栏并排，右栏（Bridge 网关与日志）折叠为底部通栏或右侧滑出抽屉。

\* `<1024px`（平板/手机应急查看）：单列垂直流，优先序为：\*\*顶部状态 → 作业进度 → 工具头矩阵 → 图传 → AMS → Bridge 日志\*\*。

---

## 4. 模块级详细设计规格

### 4.1 顶部战术状态栏（Top Telemetry Bar，固定高度 `52px`）

\* \*\*左区 — 设备与固件上下文\*\*：

  \* 多机切换下拉器：显示 `[机型图标] UNIT_ID // 机型名`（如 `UNIT_01 // BBL-H3C`）、SN 后四位与连接状态点。

  \* \*\*控制权限徽章\*\*：若检测到新固件 MQTT 签密限制且未开启局域网开发者权限，常驻黄色线框徽章 `MODE // TELEMETRY_ONLY (SIGNED)`，明确告知用户当前为只读安全态。

\* \*\*中区 — Bridge 网关脉搏\*\*：

  \* 3 个紧凑遥测胶囊：`LOCAL_MQTT: 11ms`、`THROUGHPUT: 32 msg/s`、`CLIENTS: 3`。

\* \*\*右区 — 全局开关与急停\*\*：

  \* \*\*`[ DEV_INSPECT ]` 切换按钮\*\*：开启后触发全局 API 路径透视（详见第 5 节）。

  \* \*\*`[ E-STOP / 急停 ]`\*\*：黑底红框斜纹边框按钮，点击触发二次确认弹窗（防误触），发送暂停/急停指令。

---

### 4.2 左栏：光学视窗与作业中枢（占宽 \~34%）

#### A. 光学监控视窗（Camera HUD）

\* \*\*16:9 监控画面\*\*：四角叠加 L 型准星框，左上角微字显示 `CAM_01 // LIVE`、`1080P · 15FPS`，右下角集成 `[腔灯]`、`[抓拍]`、`[全屏]` 图标按钮。

\* \*\*断流/未开启引导态\*\*：当检测到 RTSP 握手超时（H2/H3C/X2D 常见默认关闭情况），画面区显示终末地扫描线底纹 + 居中引导文案：

  &gt; `NO_VIDEO_SIGNAL // RTSP_UNAVAILABLE`  

  &gt; 请在打印机屏幕 `[设置 > 局域网]` 中开启 \*\*"仅局域网实时监控"\*\*。

#### B. 作业进度卡（Active Job）

\* \*\*任务头信息\*\*：`.3mf` 文件预览缩略图 + 文件名 + 混径/多工具头校验标签（如 `MULTI_TOOL: 4 USED`）。

\* \*\*双层进度呈现\*\*：超大号等宽百分比（如 `68.4%`）+ 20 格分段式进度条（高亮填充 `#FFDE00`）。

\* \*\*4 格核心数据\*\*：`ELAPSED`（已用时长）、`ETA`（预计完成时刻 + 剩余时间）、`LAYER`（`142 / 310`）、`EST_FILAMENT`（总耗材克数）。

\* \*\*速度档位切换器\*\*：4 档分段按钮 `[SILENT]` `[STD]` `[SPORT]` `[LUDICROUS]`，当前激活态采用\*\*黑字黄底\*\*。

\* \*\*任务操作\*\*：`[ 暂停作业 ]` / `[ 终止任务 ]`（只读模式下自动置灰并附加锁图标）。

---

### 4.3 中栏：能力驱动的硬件与供料矩阵（占宽 \~41%）

中栏根据 `/capabilities` 返回的拓扑自动呈现对应结构：

#### A. 热端 / 工具头矩阵（Toolhead Matrix）

\* \*\*形态 1：H3C 模式（`tool_architecture === "swaptek_rack"`）\*\*

  \* \*\*9 槽横向战术条（`T01` ~ `T09`）\*\*：每格宽度均分，包含顶部 3px 耗材色条、槽位编号、喷嘴直径（如 `0.4`）、实时温度大字、底部状态标。

  \* \*\*三态视觉规范\*\*：

    \* `ENGAGED`（正在机头打印）：\*\*`2px` 工程黄实线框 + 顶部黑底黄字 `ACTIVE`\*\*。

    \* `PRE-HEAT`（机架后台预热中）：\*\*`2px` 工程黄虚线呼吸框 + 温度旁上升箭头 `↑` + `PREHEAT`\*\*。

    \* `DOCKED / EMPTY`（待机/空槽）：暗灰线框，数据弱化显示。

  \* \*\*异构工具头标识\*\*：`T09` 若接入 `2.85mm FlexFeed` 或激光/切割头，自动替换图标并打上 `FLEX_2.85` / `LASER` 徽章。

  \* \*\*机架控制条\*\*：标题栏右侧集成 `[▲ RACK_UP]`、`[▼ RACK_DOWN]` 按钮及霍尔归位状态灯（仅空闲状态可点击）。

\* \*\*形态 2：H2C 模式（`vortek_1_6`）\*\*

  \* 左侧 1/3 展示 `FIXED_LEFT` 固定主热端；右侧 2/3 以 `2×3` 紧凑格展示 `VORTEK_RACK [01~06]`，被机械臂取走的槽位显示虚线镂空态 `IN_CARRIAGE`。

\* \*\*形态 3：H2D / X2D 模式（`dual_lr`）\*\*

  \* 左右对称双联卡片：`L-NOZZLE` 与 `R-NOZZLE`，各自带独立温控 Sparkline 曲线、孔径标注、以及来自 `Filament Track Switch` 的路由微标（如 `← AMS_1` / `← EXT_R`）。激活侧亮工程黄框。

\* \*\*形态 4：单头模式（`single`）\*\*

  \* 还原为标准单卡，放大展示喷嘴实时温度曲线、K 值与流速信息。

#### B. 温控、风道与动态风扇（Thermal & Airflow）

\* \*\*热床与腔温双栏\*\*：各自展示 `当前 / 目标温度` + 近 5 分钟迷你折线图（Sparkline）。

\* \*\*风道模式切换器\*\*（当 `chamber.has_airduct_mode === true` 时显示）：

  \* 嵌于腔温卡片右侧，显示当前风道挡板物理状态：`[ HEATING_LOOP // 内循环 ]`（琥珀色）或 `[ COOLING_VENT // 外排 ]`（冷蓝色），支持一键切换。

\* \*\*动态风扇列表\*\*：遍历 [`capabilities.fans`](http://capabilities.fans) 渲染水平能量条，支持点击微调滑块：

  \* H3C 展示 5 路：`PART`、`L-AUX`、`R-AUX`、`EXHAUST`、`CHAMBER`。

  \* X2D 展示 4 路：`PART`、`L-AUX`、`R-AUX`、`EXHAUST`。

#### C. 供料系统矩阵（AMS & Spools）

\* \*\*形态 1：H3C 专属 AMS 3H（`ams_3h_dual_zone`）\*\*

  \* \*\*分区环境栏\*\*：顶部拆分出 \*\*`ZONE_A`（常规区温湿度）\*\* 与 \*\*`ZONE_B_DRY`（85°C 干燥区温湿度 + 烘干剩余倒计时）\*\*。

  \* \*\*物理映射排布\*\*：下排 5 个常规槽位（`S1`\~`S5`） + 上排左侧 2 个常规槽位（`S6`\~`S7`） + 上排右侧独立琥珀色虚线框包裹的 \*\*2 个主动烘干核心位（`D8`、`D9`）\*\*。

  \* 若 `T01` 挂载了外部 AMS，卡片右上角出现 `[AMS_3H]` / `[EXT_AMS]` 切换 Tab。

\* \*\*形态 2：标准多 AMS + 双外挂料盘（H2D / X2D / H2C）\*\*

  \* 顶部支持 `[AMS-1]` `[AMS-2]` 切换（4 槽横向排布），末尾动态追加 \*\*`EXT_L`\*\* 与 \*\*`EXT_R`\*\*（根据 `external_spool_count` 决定渲染 1 个还是 2 个）。

  \* 每个槽位统一展示：真实 RGB 耗材色带、材质（`PLA / PETG / PA-CF / TPU`）、剩余估算条，当前供料中的槽位外框亮工程黄。

---

### 4.4 右栏：Bridge 接口桥接与终端日志流（占宽 \~25%）

\* \*\*服务端口健康卡（Service Endpoints）\*\*：

  \* 展示插件暴露的协议入口：`HTTP REST (:8080)`、`WebSocket (/ws)`、`MQTT Broker (:1883)`，附带运行状态点与 QPS。

\* \*\*活跃消费端列表（Connected Consumers）\*\*：

  \* 列出当前接入 Bridge 的第三方应用（如 `HomeAssistant`、`Spoolman_Bridge`、`Discord_Bot`）。

  \* 显示每个应用的最近一次交互摘要与延迟：`POST /api/v1/printer/light -> 200 OK (4ms)`。

\* \*\*实时指令与事件流（Live Event Console）\*\*：

  \* 底部占右栏约 45% 高度的等宽终端框。

  \* 支持类型过滤胶囊：`[ALL]` `[EVT]` `[TX_CMD]` `[ERR]`。

  \* 高亮展示标准化语义事件（如 `[EVT] TOOL_CHANGED: T01 -> T03 (Tray #3)`），便于排查多喷嘴换头逻辑。

  \* 底部操作条：`[暂停滚动]`、`[复制当前完整 Snapshot JSON]`。

---

## 5. 专属微交互与异常状态规范

1. \*\*`DEV_INSPECT` 字段透视模式（开发者神器）\*\*

   \* 点击顶部 `[DEV_INSPECT]` 开关后，页面所有数据卡片右上角浮现极小的黄色等宽浮标（如 `tools[2].temp`、`ams.zones[1].humidity`）。

   \* 鼠标悬停浮标显示对应的 `GET / POST` 接口路径与 `curl` 示例，点击直接复制到剪贴板，实现“所见即接口文档”。

2. \*\*HMS 故障警示横幅（Hazard Banner）\*\*

   \* 平时不占高度。一旦收到打印机 HMS 告警或 Bridge 混径切片校验冲突警告，在顶部状态栏正下方推开一条 `36px` 高的\*\*黑黄斜纹警戒带\*\*，显示错误码、中文译文与 `[查看原始报文]` / `[忽略]` 按钮。

3. \*\*原地战术浮窗（Quick Control Popover）\*\*

   \* 点击任意工具头槽位、温度卡或 AMS 槽位时，不跳转路由，原地弹出 45° 切角浮窗，支持直接键入目标温度回车下发、触发 `Pre-Heat` 预热或执行退/进料。

---

## 6. 前后端数据契约（TypeScript 接口参考）

以下接口可直接作为 Bridge 后端输出与前端 Store 的核心类型定义：

\`\`\`typescript

export type ToolArchitecture = 'single' | 'dual_lr' | 'vortek_1_6' | 'swaptek_rack';

export type AmsTopology = 'none' | 'standard_4' | 'ams_3h_dual_zone';

export type FanType = 'part' | 'aux_left' | 'aux_right' | 'exhaust' | 'chamber';

export interface PrinterCapabilities {

  model: string;                          // e.g., "H3C", "H2C", "H2D", "X2D"

  tool_architecture: ToolArchitecture;

  tool_count: number;                     // H3C=9, H2C=7(1+6), H2D/X2D=2, Single=1

  supports_preheat: boolean;              // H3C = true

  supports_mixed_nozzle: boolean;

  supports_rack_control: boolean;         // H3C = true

  chamber: {

    has\_sensor: boolean;

    has\_heater: boolean;

    max\_temp: number;                     // 65 for H2/H3C/X2D

    has\_airduct\_mode: boolean;

  };

  fans: FanType[];

  ams_topology: AmsTopology;

  external_spool_count: number;           // 2 for H2D/X2D, 0 or 1 for others

  control_writable: boolean;              // false when locked by Signed MQTT

}

export interface ToolheadState {

  id: number;                             // 0-indexed logical id

  slot_label: string;                     // "T01"\~"T09" | "L" | "R" | "FIXED" | "V01"\~"V06"

  state: 'engaged' | 'preheating' | 'idle' | 'docked' | 'empty';

  tool_type: 'extruder_175' | 'flex_285' | 'laser' | 'cutter';

  temp_current: number;

  temp_target: number;

  nozzle_diameter: number;                // 0.2 | 0.4 | 0.6 | 0.8

  nozzle_material?: string;               // "HARDENED_STEEL" | "BRASS"

  mapped_tray_id?: string | null;         // e.g., "ams3h_slot_3" or "ext_right"

  filament_color?: string;                // Hex RGB

}

export interface AmsZoneState {

  zone_id: string;                        // "zone_a" | "zone_b_dry"

  label: string;

  temp: number;

  humidity: number;

  drying_active: boolean;

  drying_target_temp?: number;

  drying_remain_min?: number;

}

export interface BridgeGatewayStatus {

  mqtt_latency_ms: number;

  api_rps: number;

  endpoints: {

    rest: { active: boolean; port: number };

    ws: { active: boolean; connections: number };

    mqtt\_broker: { active: boolean; tls: boolean };

  };

  consumers: Array&lt;{

    name: string;

    last\_action: string;

    status\_code: number;

    latency\_ms: number;

    last\_seen\_ts: number;

  }&gt;;

}

\`\`\`

---

## 7. 推荐技术栈与实施顺序

\* \*\*前端推荐栈\*\*：`Vue 3 / React` + `Tailwind CSS`（配置上述 Design Tokens 为自定义变量） + `Canvas / Lightweight SVG`（绘制迷你温控 Sparkline，避免引入过重的图表库拖慢高频 MQTT 渲染）。

\* \*\*落地四步走\*\*：

  1. \*\*Step 1\*\*：搭建 `12 栅格画布 + 45° 切角卡片组件库 + 等宽字体排版规范`。

  2. \*\*Step 2\*\*：在 Bridge 后端输出 `/capabilities` 与归一化后的 `tools[]`、`ams_zones[]` Mock 数据（优先以你的 \*\*H3C\*\* 9 槽全满态调试极限布局密度）。

  3. \*\*Step 3\*\*：接入真实 WebSocket 遥测流与 `DEV_INSPECT` 字段透视浮标。

  4. \*\*Step 4\*\*：用 \*\*H2D / X2D / H2C\*\* 的 Mock 数据切换测试 UI 自适应变形与只读权限（Signed MQTT）降级交互。