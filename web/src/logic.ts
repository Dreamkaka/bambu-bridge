export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };

export function isObject(value: Json | null | undefined): value is { [key: string]: Json } {
  return value != null && typeof value === 'object' && !Array.isArray(value);
}

export function obj(value: Json | undefined): { [key: string]: Json } | null {
  return isObject(value) ? value : null;
}

export function num(value: Json | undefined): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'string' && /^-?\d+(\.\d+)?$/.test(value)) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }
  return null;
}

export function str(value: Json | undefined): string | null {
  return typeof value === 'string' ? value : null;
}

export function mergeJson(base: Json, patch: Json): Json {
  if (isObject(base) && isObject(patch)) {
    const out: { [key: string]: Json } = { ...base };
    for (const key of Object.keys(patch)) {
      out[key] = key in base ? mergeJson(base[key], patch[key]) : patch[key];
    }
    return out;
  }
  return patch;
}

const SECRET: Record<string, true> = {
  password: true, passwd: true, pass: true, access_code: true, accesscode: true, token: true,
  secret: true, authkey: true, auth_key: true, credential: true, oss_url: true,
};

export function isSensitiveKey(key: string): boolean {
  return Object.prototype.hasOwnProperty.call(SECRET, key.toLowerCase());
}

export type Leaf = { path: string; key: string; value: Json; hidden: boolean };

function leafKey(path: string): string {
  const clean = path.replace(/\[\d+\]/g, '');
  const dot = clean.lastIndexOf('.');
  return dot >= 0 ? clean.slice(dot + 1) : clean;
}

export function flatten(value: Json, prefix = ''): Leaf[] {
  if (isObject(value)) {
    const keys = Object.keys(value);
    if (!keys.length && prefix) return [{ path: prefix, key: leafKey(prefix), value, hidden: false }];
    return keys.flatMap((key) => {
      const path = prefix ? `${prefix}.${key}` : key;
      if (isSensitiveKey(key)) return [{ path, key, value: null, hidden: true }];
      return flatten(value[key], path);
    });
  }
  if (Array.isArray(value)) {
    if (!value.length && prefix) return [{ path: prefix, key: leafKey(prefix), value, hidden: false }];
    return value.flatMap((item, index) => flatten(item, `${prefix}[${index}]`));
  }
  if (!prefix) return [];
  return [{ path: prefix, key: leafKey(prefix), value, hidden: false }];
}

export function trimNum(value: number): string {
  if (!Number.isFinite(value)) return '—';
  if (Number.isInteger(value) && Math.abs(value) < 1e15) return String(value);
  return value.toFixed(2).replace(/\.?0+$/, '');
}

export function clock(ms: number): string {
  const date = new Date(ms);
  const pad = (part: number) => String(part).padStart(2, '0');
  return `${date.getFullYear()}.${pad(date.getMonth() + 1)}.${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

export function unixClock(seconds: number): string | null {
  if (!Number.isInteger(seconds) || seconds < 1_000_000_000 || seconds > 10_000_000_000) return null;
  return clock(seconds * 1000);
}

export function byteText(value: number): string {
  if (!Number.isFinite(value) || value < 0) return '—';
  if (value < 1024) return `${trimNum(value)} B`;
  if (value < 1024 * 1024) return `${trimNum(value / 1024)} KB`;
  if (value < 1024 * 1024 * 1024) return `${trimNum(value / 1024 / 1024)} MB`;
  return `${trimNum(value / 1024 / 1024 / 1024)} GB`;
}

export function minutes(value: number): string {
  const rounded = Math.round(value);
  const sign = rounded < 0 ? '−' : '';
  const abs = Math.abs(rounded);
  const hours = Math.floor(abs / 60);
  const rest = abs % 60;
  return hours ? `${sign}${hours} 小时 ${rest} 分` : `${sign}${rest} 分`;
}

export function percentText(value: number): string {
  if (!Number.isFinite(value)) return '—';
  if (Number.isInteger(value) && value >= 0 && value <= 100) return `${String(value).padStart(3, '0')}%`;
  return `${trimNum(value)}%`;
}

export function ipv4Little(value: number): string | null {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) return null;
  const bits = value >>> 0;
  return [bits & 255, (bits >>> 8) & 255, (bits >>> 16) & 255, bits >>> 24].join('.');
}


export type Sample = { t: number; v: number };
export type Point = { x: number; y: number };

export function pushSample(samples: Sample[], value: number, now: number): void {
  const last = samples[samples.length - 1];
  if (last && last.v === value && now - last.t < 5000) return;
  samples.push({ t: now, v: value });
  if (samples.length > 90) samples.shift();
}

export function seriesPoints(samples: Sample[]): Point[] | null {
  if (samples.length < 2) return null;
  const start = samples[0].t;
  const span = Math.max(1, samples[samples.length - 1].t - start);
  const values = samples.map((sample) => sample.v);
  let min = Math.min(...values);
  let max = Math.max(...values);
  if (max - min < 0.5) {
    min -= 0.5;
    max += 0.5;
  }
  return samples.map((sample) => ({
    x: ((sample.t - start) / span) * 300 + 8,
    y: 8 + (1 - (sample.v - min) / (max - min)) * 100,
  }));
}

const GCODE: Record<string, string> = {
  IDLE: '空闲',
  INIT: '初始化',
  PREPARE: '准备',
  SLICING: '切片',
  RUNNING: '打印中',
  PAUSE: '已暂停',
  FINISH: '已完成',
  FAILED: '失败',
  OFFLINE: '离线',
  UNKNOWN: '未知',
};

const STAGES: Record<number, string> = {
  [-1]: '空闲',
  0: '正在打印',
  1: '自动调平',
  2: '热床预热',
  3: '振动补偿',
  4: '正在换料',
  5: '暂停等待',
  6: '缺料暂停',
  7: '喷嘴加热',
  8: '挤出校准',
  9: '扫描热床',
  10: '检查首层',
  11: '识别打印板',
  12: '激光雷达校准',
  13: '回中',
  14: '清理喷嘴',
  15: '检查挤出温度',
  16: '手动暂停',
  17: '前门脱落暂停',
  18: '激光雷达校准',
  19: '流量校准',
  20: '喷嘴温度异常暂停',
  21: '热床温度异常暂停',
  22: '正在退料',
  23: '丢步暂停',
  24: '正在进料',
  25: '电机噪声校准',
  26: 'AMS 断开暂停',
  27: '喉管风扇转速低暂停',
  28: '腔温控制异常暂停',
  29: '腔室冷却',
  30: '用户指令暂停',
  31: '电机噪声展示',
  32: '喷嘴裹料暂停',
  33: '切刀异常暂停',
  34: '首层异常暂停',
  35: '喷嘴堵塞暂停',
  36: '校准前绝对精度检查',
  37: '绝对精度校准',
  38: '校准后绝对精度检查',
  39: '喷嘴偏移校准',
  40: '高温热床调平',
  41: '检查快拆',
  42: '检查门盖',
  43: '激光校准',
  44: '检查打印板',
  45: '检查俯视相机位置',
  46: '校准俯视相机',
  47: '调平第一段',
  48: '调平第二段',
  49: '腔室加热',
  50: '热床冷却',
  51: '打印校准线',
  52: '检查耗材',
  53: '校准实况相机',
  54: '等待热床温度',
  55: '检查耗材位置',
  56: '校准切刀偏移',
  57: '测量表面',
  58: '热预调',
  59: '刀架回中',
  60: '校准相机偏移',
  61: '校准刀架位置',
  62: '热端取放测试',
  63: '等待腔温均匀',
  64: '准备热端',
  65: '校准裹头检测',
  66: '净化腔室空气',
  67: '测量旋转附件',
  68: '喷头移到吐料槽上方',
  69: '喷嘴冷却',
  70: '喷头移到热床中心',
  71: '主动圆弧拟合',
  72: '热端类型检测',
  73: '打印板对齐检测',
  74: '热床表面异物检测',
  75: '热床底部异物检测',
  76: '打印前预挤出',
  77: '准备 AMS',
  82: '热床稳定',
  255: '空闲',
};

const SPEED: Record<number, string> = { 1: '静音', 2: '标准', 3: '运动', 4: '狂暴' };
const AIR: Record<number, string> = { 0: '冷却', 1: '加热', 2: '激光' };
const HUMID: Record<number, string> = { 1: '最干', 2: '较干', 3: '适中', 4: '较湿', 5: '最湿' };

const HOME: [number, string][] = [
  [0x00000001, 'X 已归零'],
  [0x00000002, 'Y 已归零'],
  [0x00000004, 'Z 已归零'],
  [0x00000008, '220V'],
  [0x00000010, '丢步自动恢复'],
  [0x00000020, '正在录像'],
  [0x00000080, '余料估计'],
  [0x00000100, '有存储卡'],
  [0x00000200, '存储卡异常'],
  [0x00000400, '自动续料'],
  [0x00020000, '提示音'],
  [0x00040000, '有线网络'],
  [0x00080000, '支持缠料检测'],
  [0x00100000, '检测到缠料'],
  [0x00200000, '支持电机校准'],
  [0x00800000, '门开着'],
  [0x04000000, '已装扩展'],
  [0x08000000, '支持扩展'],
];

const PATHS: Record<string, string> = {
  'ams.ams.id': 'AMS 编号',
  'ams.ams.temp': '仓温',
  'ams.ams.humidity': '湿度档',
  'ams.ams.humidity_raw': '相对湿度',
  'ams.ams.dry_time': '剩余烘干时间',
  'ams.ams.info': 'AMS 类型信息',
  'ams.version': 'AMS 协议版本',
  'ams.ams.tray.id': '槽号',
  'ams.ams.tray.k': '流量校准 K',
  'ams.ams.tray.n': '流量校准 N',
  'ams.ams.tray.state': '料槽状态',
  'vt_tray.id': '外挂料盘编号',
  'vir_slot.id': '外挂料盘编号',
  'lights_report.mode': '灯光',
  'lights_report.node': '灯位',
  'ipcam.liveview.local': '局域网画面协议',
  'ipcam.liveview.remote': '云端画面协议',
  'ipcam.resolution_supported': '支持的分辨率',
  'ipcam.current_liveview': '当前画面',
  'ipcam.support_liveview_cams': '可用相机',
  'upgrade_state.status': '升级状态',
  'upgrade_state.progress': '升级进度',
  'upgrade_state.message': '升级消息',
  'upgrade_state.err_code': '升级错误码',
  'upgrade_state.module': '升级模块',
  'upgrade_state.ota_new_version_number': '新固件版本',
  'upgrade_state.ahb_new_version_number': '新应用板版本',
  'upgrade_state.ams_new_version_number': '新 AMS 版本',
  'upgrade_state.new_version_state': '新版本状态',
  'upgrade_state.force_upgrade': '强制升级',
  'upgrade_state.consistency_request': '一致性确认',
  'upgrade_state.dis_state': '升级显示状态',
  'upgrade_state.sequence_id': '升级序号',
  'upload.status': '上传状态',
  'upload.progress': '上传进度',
  'upload.message': '上传消息',
  'upload.speed': '上传速度',
  'upload.file_size': '上传文件大小',
  'upload.finish_size': '已上传大小',
  'upload.time_remaining': '上传剩余时间',
  'upload.task_id': '上传任务编号',
  'upload.sequence_id': '上传序号',
  'upload.trouble_id': '上传故障编号',
  'net.conf': '网络配置',
  'net.info.ip': '地址',
  'net.info.mask': '掩码',
  'online.ahb': 'AHB 在线',
  'online.rfid': 'RFID 在线',
  'online.version': '在线信息版本',
  'xcam.allow_skip_parts': '允许跳过零件',
  'xcam.buildplate_marker_detector': '打印板标记检测',
  'xcam.first_layer_inspector': '首层检查',
  'xcam.halt_print_sensitivity': '停打灵敏度',
  'xcam.print_halt': '异常停打',
  'xcam.printing_monitor': '打印监控',
  'xcam.spaghetti_detector': '炒面检测',
  'hms.attr': '健康管理属性',
  'hms.code': '健康管理码',
  'hms.action': '健康管理动作',
  'hms.timestamp': '健康管理时间',
};

const LEAVES: Record<string, string> = {
  command: '命令',
  msg: '消息码',
  t_utc: '上报时间',
  sequence_id: '序号',
  gcode_state: '任务状态',
  mc_percent: '进度',
  mc_remaining_time: '剩余时间',
  layer_num: '当前层',
  total_layer_num: '总层数',
  subtask_name: '任务名',
  gcode_file: '切片文件',
  gcode_file_prepare_percent: '文件准备进度',
  gcode_start_time: '任务开始时间',
  project_id: '项目编号',
  profile_id: '配置编号',
  task_id: '任务编号',
  subtask_id: '子任务编号',
  print_type: '打印来源',
  print_error: '打印错误',
  fail_reason: '失败原因',
  mc_print_error_code: '阶段错误码',
  mc_print_stage: '打印阶段',
  mc_print_sub_stage: '打印子阶段',
  stg: '阶段队列',
  stg_cur: '当前阶段序号',
  queue_number: '队列序号',
  wifi_signal: '无线信号',
  nozzle_diameter: '喷嘴直径',
  nozzle_type: '喷嘴类型',
  nozzle_temper: '喷嘴温度',
  nozzle_target_temper: '喷嘴目标温度',
  bed_temper: '热床温度',
  bed_target_temper: '热床目标温度',
  chamber_temper: '腔温',
  chamber_target_temper: '腔温目标',
  heatbreak_fan_speed: '喉管风扇',
  cooling_fan_speed: '部件冷却风扇',
  big_fan1_speed: '辅助风扇',
  big_fan2_speed: '腔体风扇',
  fan_gear: '风扇档位',
  aux_part_fan: '辅助风扇已安装',
  home_flag: '归零与开关标记',
  flag3: '授权控制标记',
  sdcard: '存储卡',
  lifecycle: '生命周期',
  force_upgrade: '强制升级',
  ams_status: 'AMS 状态',
  ams_rfid_status: 'AMS RFID 状态',
  ams_exist_bits: '已连接 AMS',
  tray_exist_bits: '有料槽位',
  tray_is_bbl_bits: '官方料槽位',
  tray_now: '当前槽',
  tray_tar: '目标槽',
  tray_pre: '上一槽',
  tray_read_done_bits: '已读槽位',
  tray_reading_bits: '正在读的槽位',
  insert_flag: '插入时读芯片',
  power_on_flag: '开机读芯片',
  ipcam_dev: '摄像头',
  ipcam_record: '录像',
  timelapse: '延时摄影',
  resolution: '分辨率',
  tutk_server: 'TUTK',
  agora_service: 'Agora',
  brtc_service: 'BRTC',
  rtsp_url: 'RTSP 地址',
  xcam_status: '相机检测状态',
  filam_bak: '耗材备份',
  vt_tray: '外挂料盘',
  vir_slot: '双头外挂料盘',
  tray_type: '材料',
  tray_color: '颜色',
  tray_sub_brands: '子品牌',
  tray_id_name: '耗材名称',
  tray_info_idx: '耗材编号',
  tray_weight: '重量',
  tray_diameter: '线径',
  tray_uuid: '耗材标识',
  tag_uid: '芯片标识',
  remain: '剩余',
  nozzle_temp_min: '喷嘴下限',
  nozzle_temp_max: '喷嘴上限',
  bed_temp: '建议热床温度',
  bed_temp_type: '热床温度类型',
  drying_temp: '烘干温度',
  drying_time: '烘干时长',
  cols: '多色',
  xcam_info: '相机标定',
  result: '结果',
  reason: '原因',
};

function own(map: Record<string, string>, key: string): string | null {
  return Object.prototype.hasOwnProperty.call(map, key) ? map[key] : null;
}

export function meaning(path: string): string | null {
  const parts = path.replace(/\[\d+\]/g, '').split('.').filter(Boolean);
  for (let index = 0; index < parts.length; index += 1) {
    const label = own(PATHS, parts.slice(index).join('.'));
    if (label) return label;
  }
  return own(LEAVES, parts[parts.length - 1] || '');
}

export function gcodeLabel(value: string): string | null {
  return GCODE[value.trim().toUpperCase()] ?? null;
}

export function stageLabel(value: number | null): string | null {
  if (value == null || !Number.isInteger(value)) return null;
  return STAGES[value] ?? null;
}

export function trayPlace(raw: string): string | null {
  if (!/^-?\d+$/.test(raw)) return null;
  const value = Number(raw);
  if (value === 255) return '无料';
  if (value === 254) return '外挂料盘';
  if (value >= 128 && value <= 135) return `AMS HT ${value}`;
  if (value >= 0 && value <= 15) return `AMS ${value >> 2} / 槽 ${value & 3}`;
  return null;
}

export function amsStatusText(value: number): string | null {
  if (!Number.isInteger(value)) return null;
  const main = (value >> 8) & 0xff;
  const sub = value & 0xff;
  const mains: Record<number, string> = {
    0x00: '空闲',
    0x01: '换料',
    0x02: '读 RFID',
    0x03: '辅助进料',
    0x04: '校准',
    0x07: '冷拔',
    0x10: '自检',
    0x20: '调试',
    0xff: '未知',
  };
  const subs: Record<number, string> = {
    0x02: '加热喷嘴',
    0x03: '剪断',
    0x04: '抽回旧料',
    0x05: '送入新料',
    0x06: '送入新料',
    0x07: '冲刷',
    0x08: '确认位置',
    0x0b: '确认位置',
  };
  const head = mains[main] ?? `主状态 ${main}`;
  if (main === 0x01 && subs[sub]) return `${head} / ${subs[sub]}`;
  if (sub) return `${head} / 子步骤 ${sub}`;
  return head;
}

function hexBits(value: string): string | null {
  if (!/^[0-9a-fA-F]+$/.test(value) || value.length > 8) return null;
  const parsed = Number.parseInt(value, 16);
  if (!Number.isFinite(parsed)) return null;
  const slots: number[] = [];
  for (let bit = 0; bit < 32; bit += 1) if (parsed & (1 << bit)) slots.push(bit + 1);
  return slots.length ? slots.join(' ') : '无';
}

export function hmsCode(value: Json): string | null {
  const record = obj(value);
  if (!record) return null;
  const attr = num(record.attr);
  const code = num(record.code);
  if (attr == null || code == null || !Number.isInteger(attr) || !Number.isInteger(code)) return null;
  const group = (part: number) => (part >>> 0 & 0xffff).toString(16).toUpperCase().padStart(4, '0');
  const high = (part: number) => ((part >>> 0) >>> 16).toString(16).toUpperCase().padStart(4, '0');
  return `${high(attr)}_${group(attr)}_${high(code)}_${group(code)}`;
}

export function colorHex(value: Json): string | null {
  if (typeof value !== 'string' || !/^[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$/.test(value)) return null;
  if (/^0+$/.test(value)) return null;
  return `#${value.slice(0, 6)}`;
}

function formatRaw(value: Json): string {
  if (value === null) return '—';
  if (typeof value === 'boolean') return value ? '是' : '否';
  if (typeof value === 'number') return trimNum(value);
  if (typeof value === 'string') return value === '' ? '空' : value;
  if (Array.isArray(value)) return value.length ? JSON.stringify(value) : '空列表';
  return Object.keys(value).length ? JSON.stringify(value) : '空';
}

function homeFlagText(value: number): string | null {
  if (!Number.isInteger(value)) return null;
  const names = HOME.filter(([bit]) => (value & bit) !== 0).map(([, name]) => name);
  return names.length ? names.join(' / ') : '没有这些标记';
}

function flag3Text(value: number): string | null {
  if (!Number.isInteger(value)) return null;
  const names: string[] = [];
  if (value & 0x10) names.push('授权控制已启用');
  if (value & 0x10000) names.push('支持授权控制');
  return names.length ? names.join(' / ') : null;
}

export function annotate(path: string, value: Json): { kind: 'unit' | 'text'; text: string } | null {
  const norm = path.replace(/\[\d+\]/g, '');
  const leaf = norm.slice(norm.lastIndexOf('.') + 1);
  const numeric = num(value);
  if (leaf === 'gcode_state' && typeof value === 'string') {
    const label = gcodeLabel(value);
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'mc_print_stage') {
    const label = stageLabel(numeric);
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'spd_lvl') {
    const label = numeric != null ? SPEED[numeric] : undefined;
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'ams_status' && numeric != null) {
    const label = amsStatusText(numeric);
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'home_flag' && numeric != null) {
    const label = homeFlagText(numeric);
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'flag3' && numeric != null) {
    const label = flag3Text(numeric);
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'tray_now' || leaf === 'tray_tar' || leaf === 'tray_pre') {
    const label = trayPlace(String(value));
    return label ? { kind: 'text', text: label } : null;
  }
  if (leaf === 'humidity' && numeric != null && HUMID[numeric]) return { kind: 'text', text: HUMID[numeric] };
  if (leaf === 'mc_remaining_time' && numeric != null) return { kind: 'text', text: minutes(numeric) };
  if (leaf === 't_utc' && numeric != null && numeric > 1e12) return { kind: 'text', text: clock(numeric) };
  if (leaf === 'dry_time' || leaf === 'drying_time') return numeric != null ? { kind: 'unit', text: '秒' } : null;
  if (/(^|_)temper$/.test(leaf) || leaf === 'temp' || leaf.endsWith('_temp') || leaf === 'chamber_temper') {
    return numeric != null ? { kind: 'unit', text: '℃' } : null;
  }
  if (leaf === 'mc_percent' || leaf === 'spd_mag' || leaf.endsWith('_percent') || leaf === 'remain' || leaf === 'progress') {
    if (leaf === 'remain' && numeric === -1) return { kind: 'text', text: '未知' };
    return numeric != null ? { kind: 'unit', text: '%' } : null;
  }
  if ((leaf === 'print_error' || leaf === 'mc_print_error_code' || leaf === 'err_code') && numeric != null && Number.isInteger(numeric)) {
    return { kind: 'text', text: (numeric >>> 0).toString(16).toUpperCase().padStart(8, '0') };
  }
  if (leaf === 'print_type' && typeof value === 'string') {
    const labels: Record<string, string> = { cloud: '云端', local: '本地', idle: '空闲', system: '系统' };
    const label = labels[value.trim().toLowerCase()];
    return label ? { kind: 'text', text: label } : null;
  }
  if ((leaf === 'ip' || leaf === 'mask') && numeric != null) {
    const address = ipv4Little(numeric);
    return address ? { kind: 'text', text: address } : null;
  }
  if ((leaf === 'gcode_start_time' || leaf === 'timestamp' || leaf === 'time') && numeric != null) {
    if (numeric === 0 && leaf === 'gcode_start_time') return { kind: 'text', text: '未开始' };
    const label = unixClock(numeric);
    return label ? { kind: 'text', text: label } : null;
  }
  if (/_bits$/.test(leaf) && typeof value === 'string') {
    const label = hexBits(value);
    return label ? { kind: 'text', text: label } : null;
  }
  if (norm.includes('lights_report') && leaf === 'mode' && typeof value === 'string') {
    const labels: Record<string, string> = { on: '开', off: '关', flashing: '闪' };
    const label = labels[value.trim().toLowerCase()];
    return label ? { kind: 'text', text: label } : null;
  }
  if ((norm.includes('ipcam') || leaf === 'ipcam_record' || leaf === 'timelapse' || leaf === 'tutk_server' || leaf === 'agora_service' || leaf === 'brtc_service') && typeof value === 'string') {
    const token = value.trim().toLowerCase();
    if (token === 'enable' || token === 'enabled') return { kind: 'text', text: '开' };
    if (token === 'disable' || token === 'disabled') return { kind: 'text', text: '关' };
    if (leaf === 'ipcam_dev' && value === '1') return { kind: 'text', text: '有' };
    if (leaf === 'ipcam_dev' && value === '0') return { kind: 'text', text: '无' };
  }
  if (norm.includes('airduct') && numeric != null && AIR[numeric]) return { kind: 'text', text: AIR[numeric] };
  if (leaf === 'current_liveview' && numeric != null) {
    const cameras: Record<number, string> = { 4: '主相机', 5: '俯视相机' };
    return cameras[numeric] ? { kind: 'text', text: cameras[numeric] } : null;
  }
  if (leaf === 'sdcard' && typeof value === 'boolean') return { kind: 'text', text: value ? '已插入' : '未插入' };
  if ((leaf === 'status' && (norm.startsWith('upgrade_state') || norm.startsWith('upload'))) && typeof value === 'string') {
    const labels: Record<string, string> = { IDLE: '空闲', UPGRADING: '升级中', idle: '空闲' };
    const label = labels[value.trim()] ?? labels[value.trim().toUpperCase()];
    return label ? { kind: 'text', text: label } : null;
  }
  return null;
}

export function present(path: string, value: Json, hidden = false): string {
  if (hidden) return '已隐藏';
  const raw = formatRaw(value);
  const extra = annotate(path, value);
  if (!extra || extra.text === raw) return raw;
  return extra.kind === 'unit' ? `${raw} ${extra.text}` : `${raw} / ${extra.text}`;
}

