const BLOCKED = new Set(['__proto__', 'constructor', 'prototype']);

const STATE_LABELS = {
  IDLE: '空闲',
  RUNNING: '打印中',
  PAUSE: '已暂停',
  FINISH: '已完成',
  FAILED: '失败',
  PREPARE: '准备中',
  SLICING: '切片中',
};

const SPEED = { 1: '静音', 2: '标准', 3: '运动', 4: '狂暴' };

export function createModel() {
  return {
    status: null,
    printers: null,
    selectedId: null,
    source: 'cloud',
    reports: {},
    lanLink: null,
    lanPending: null,
    cloud: null,
    ws: 'connecting',
    wsEverOpen: false,
    view: 'job',
    fault: null,
    banner: null,
    pending: null,
    loginHref: '',
    shotUrl: '',
    live: false,
    events: [],
    traces: {},
  };
}

export function asNumber(value) {
  if (typeof value === 'number') return Number.isFinite(value) ? value : null;
  if (typeof value !== 'string') return null;
  const text = value.trim();
  if (!/^[+-]?(?:\d+\.?\d*|\.\d+)$/.test(text)) return null;
  const n = Number(text);
  return Number.isFinite(n) ? n : null;
}

export function validDeviceId(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value);
}

export function validIpv4(value) {
  if (typeof value !== 'string') return false;
  const parts = value.split('.');
  if (parts.length !== 4) return false;
  return parts.every((part) => {
    if (!/^\d{1,3}$/.test(part)) return false;
    if (part.length > 1 && part.startsWith('0')) return false;
    return Number(part) <= 255;
  });
}

export function validPassword(value) {
  if (typeof value !== 'string' || value.length < 1 || value.length > 128) return false;
  for (let i = 0; i < value.length; i += 1) {
    const code = value.charCodeAt(i);
    if (code < 32 || code > 126) return false;
  }
  return true;
}

function plain(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export function mergeReport(prev, patch) {
  const out = plain(prev) ? { ...prev } : {};
  if (!plain(patch)) return out;
  for (const [key, value] of Object.entries(patch)) {
    if (BLOCKED.has(key)) continue;
    out[key] = plain(value) && plain(out[key]) ? mergeReport(out[key], value) : value;
  }
  return out;
}

export function noteReport(model, id, source, report) {
  const prev = model.reports[id]?.[source]?.data || {};
  const slot = model.reports[id] || (model.reports[id] = {});
  slot[source] = { data: mergeReport(prev, report), confirmed: true };
  recordTrace(model, id, slot[source].data);
}

export function markUnconfirmed(model) {
  for (const slot of Object.values(model.reports)) {
    for (const source of ['cloud', 'lan']) {
      if (slot[source]) slot[source].confirmed = false;
    }
  }
}

export function currentReport(model) {
  if (!model.selectedId) return null;
  return model.reports[model.selectedId]?.[model.source] || null;
}

const TRACE_LIMIT = 48;
const TRACE_GAP_MS = 15000;

export function recordTrace(model, id, data, now = Date.now()) {
  if (!validDeviceId(id) || !plain(data)) return false;
  const point = {
    t: now,
    nozzle: asNumber(data.nozzle_temper),
    bed: asNumber(data.bed_temper),
    percent: asNumber(data.mc_percent),
  };
  if (point.nozzle == null && point.bed == null && point.percent == null) return false;
  const list = model.traces[id] || (model.traces[id] = []);
  const last = list[list.length - 1];
  if (
    last && last.nozzle === point.nozzle && last.bed === point.bed &&
    last.percent === point.percent && now - last.t < TRACE_GAP_MS
  ) return false;
  list.push(point);
  if (list.length > TRACE_LIMIT) list.splice(0, list.length - TRACE_LIMIT);
  return true;
}

export function heatGauge(current, target) {
  const now = asNumber(current);
  const goal = asNumber(target);
  if (now == null || goal == null || goal <= 0) return null;
  return {
    share: Math.min(1, Math.max(0, now / goal)),
    alert: now > goal + 15,
  };
}

function remainParts(value) {
  const n = asNumber(value);
  if (n == null || n < 0) return null;
  const total = Math.round(n);
  const hours = Math.floor(total / 60);
  const minutes = total % 60;
  if (hours === 0) return { big: String(minutes), unit: '分钟', sub: '' };
  return { big: String(hours), unit: '小时', sub: `${minutes} 分钟` };
}

export function stateLabel(raw) {
  if (typeof raw !== 'string' || raw === '') return null;
  return STATE_LABELS[raw] || raw;
}

export function stateTone(raw) {
  if (raw === 'FAILED') return 'is-alert';
  if (raw === 'PAUSE') return 'is-draft';
  return '';
}

export function formatTemp(value) {
  const n = asNumber(value);
  if (n == null) return null;
  return `${Number.isInteger(n) ? String(n) : n.toFixed(1)}°C`;
}

export function formatRemain(value) {
  const n = asNumber(value);
  if (n == null || n < 0) return null;
  const total = Math.round(n);
  const hours = Math.floor(total / 60);
  const minutes = total % 60;
  if (hours === 0) return `${minutes} 分钟`;
  return `${hours} 小时 ${minutes} 分钟`;
}

export function formatPercent(value) {
  const n = asNumber(value);
  if (n == null) return null;
  const rounded = Math.round(n);
  const padded = rounded >= 0 && rounded < 1000 ? String(rounded).padStart(3, '0') : String(rounded);
  return {
    value: n,
    text: `${rounded}%`,
    padded: `${padded}%`,
    bar: Math.min(1, Math.max(0, n / 100)),
  };
}

function formatFile(data) {
  const raw = typeof data.subtask_name === 'string' && data.subtask_name
    ? data.subtask_name
    : typeof data.gcode_file === 'string' ? data.gcode_file : '';
  if (!raw) return null;
  const base = raw.split(/[/\\]/).pop();
  return base || raw;
}

function formatLayer(data) {
  const current = asNumber(data.layer_num);
  const total = asNumber(data.total_layer_num);
  if (current == null && total == null) return null;
  if (current != null && total != null) return `${Math.round(current)} / ${Math.round(total)}`;
  if (current != null) return String(Math.round(current));
  return `共 ${Math.round(total)}`;
}

function formatSpeed(data) {
  const level = asNumber(data.spd_lvl);
  if (level != null) {
    const name = SPEED[level];
    return name ? `${level} ${name}` : String(level);
  }
  const mag = asNumber(data.spd_mag);
  return mag == null ? null : `${Math.round(mag)}%`;
}

function formatWifi(value) {
  if (typeof value === 'number' && Number.isFinite(value)) return `${value} dBm`;
  if (typeof value === 'string' && value.trim()) return value.trim();
  return null;
}

function formatError(value) {
  if (value == null || value === '' || value === 0 || value === '0') return null;
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  if (typeof value === 'string') return value;
  return null;
}

function colorOf(value) {
  if (typeof value !== 'string') return null;
  const match = /^([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?$/.exec(value.trim());
  if (!match || /^0+$/.test(match[1])) return null;
  return `#${match[1].toUpperCase()}`;
}

function traysOf(data) {
  const units = data.ams && Array.isArray(data.ams.ams) ? data.ams.ams : [];
  const trays = [];
  for (const unit of units) {
    if (!plain(unit) || !Array.isArray(unit.tray)) continue;
    for (const tray of unit.tray) {
      if (!plain(tray)) continue;
      const type = typeof tray.tray_type === 'string' ? tray.tray_type.trim() : '';
      const color = colorOf(tray.tray_color);
      const remain = asNumber(tray.remain);
      if (!type && !color && remain == null) continue;
      trays.push({ type, color, remain: remain == null ? null : Math.round(remain) });
      if (trays.length === 16) return trays;
    }
  }
  return trays;
}

function formatHumidity(data) {
  const unit = data.ams && Array.isArray(data.ams.ams) ? data.ams.ams[0] : null;
  if (!plain(unit)) return null;
  const humidity = asNumber(unit.humidity);
  return humidity == null ? null : `${Math.round(humidity)}%`;
}

export function displayJob(data) {
  if (!plain(data)) return null;
  const state = typeof data.gcode_state === 'string' && data.gcode_state ? data.gcode_state : null;
  return {
    percent: formatPercent(data.mc_percent),
    nozzle: formatTemp(data.nozzle_temper),
    nozzleValue: asNumber(data.nozzle_temper),
    nozzleTarget: formatTemp(data.nozzle_target_temper),
    nozzleTargetValue: asNumber(data.nozzle_target_temper),
    bed: formatTemp(data.bed_temper),
    bedValue: asNumber(data.bed_temper),
    bedTarget: formatTemp(data.bed_target_temper),
    bedTargetValue: asNumber(data.bed_target_temper),
    chamber: formatTemp(data.chamber_temper),
    remain: formatRemain(data.mc_remaining_time),
    remainParts: remainParts(data.mc_remaining_time),
    state,
    stateLabel: stateLabel(state),
    tone: stateTone(state),
    file: formatFile(data),
    layer: formatLayer(data),
    speed: formatSpeed(data),
    wifi: formatWifi(data.wifi_signal),
    error: formatError(data.print_error),
    humidity: formatHumidity(data),
    trays: traysOf(data),
  };
}

export function jobHasReadings(job) {
  if (!job) return false;
  return Boolean(
    job.percent || job.nozzle || job.bed || job.chamber || job.remain || job.state ||
    job.file || job.layer || job.speed || job.wifi || job.error || job.humidity || job.trays.length,
  );
}

export function actionGates(report) {
  const open = { pause: true, resume: true, stop: true };
  if (!report?.confirmed) return open;
  const state = report.data?.gcode_state;
  if (typeof state !== 'string' || state === '') return open;
  if (state === 'PAUSE') return { pause: false, resume: true, stop: true };
  if (state === 'RUNNING' || state === 'PREPARE') return { pause: true, resume: false, stop: true };
  if (state === 'FAILED') return { pause: false, resume: false, stop: true };
  if (state === 'IDLE' || state === 'FINISH') return { pause: false, resume: false, stop: false };
  return open;
}

export function canCommand(model) {
  if (!validDeviceId(model.selectedId) || model.status?.agent_ready !== true) return false;
  if (model.source === 'cloud') return model.status.logged_in === true;
  return model.lanLink?.status === 0 && model.lanLink.devId === model.selectedId;
}

export function pushEvent(model, text) {
  const now = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  model.events.unshift({ time: `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`, text });
  if (model.events.length > 8) model.events.length = 8;
}

export function applyEvent(model, message) {
  if (!message || typeof message.event !== 'string' || !plain(message.payload)) return null;
  const payload = message.payload;
  if (message.event === 'printer_status') {
    const source = payload.source === 'lan' ? 'lan' : payload.source === 'cloud' ? 'cloud' : null;
    if (!validDeviceId(payload.dev_id) || !source || !plain(payload.report)) return null;
    noteReport(model, payload.dev_id, source, payload.report);
    const via = source === 'lan' ? '局域网' : '云端';
    const label = stateLabel(payload.report.gcode_state);
    return label ? `${payload.dev_id} 经${via}上报：${label}` : `${payload.dev_id} 经${via}上报了新的状态`;
  }
  if (message.event === 'account_changed') {
    if (typeof payload.logged_in !== 'boolean') return null;
    if (model.status) model.status = { ...model.status, logged_in: payload.logged_in };
    return payload.logged_in ? '账号已登录' : '账号已退出';
  }
  if (message.event === 'login_result') {
    if (typeof payload.success !== 'boolean') return null;
    return payload.success ? '登录成功' : '登录没有完成';
  }
  if (message.event === 'cloud_connection') {
    if (!Number.isInteger(payload.return_code) || !Number.isInteger(payload.reason_code)) return null;
    model.cloud = { return_code: payload.return_code, reason_code: payload.reason_code };
    return payload.return_code === 5
      ? '云端连接回报 5，登录已过期'
      : `云端连接回报 ${payload.return_code} / ${payload.reason_code}`;
  }
  if (message.event === 'lan_connection') {
    if (!validDeviceId(payload.dev_id) || !Number.isInteger(payload.status)) return null;
    model.lanLink = { devId: payload.dev_id, status: payload.status };
    model.lanPending = null;
    return payload.status === 0
      ? `局域网 ${payload.dev_id} 已连接`
      : `局域网 ${payload.dev_id} 连接失败，状态 ${payload.status}`;
  }
  return null;
}

export function noticeText(model) {
  if (model.fault?.text) return { flag: model.fault.flag || '出错', text: model.fault.text };
  if (model.banner?.text) return { flag: model.banner.flag || '注意', text: model.banner.text };
  if (model.wsEverOpen && model.ws !== 'open') {
    return { flag: '注意', text: '事件通道断开了。旧的读数先标成未确认，重新连上之后会再读一次。' };
  }
  if (model.ws !== 'open' && !model.wsEverOpen && model.status) {
    return { flag: '注意', text: '页面在，事件通道还没有连上。温度和进度要等通道连上之后才会更新。' };
  }
  if (model.status && model.status.agent_ready === false) return { flag: '注意', text: '桥接还没有就绪。' };
  if (model.cloud?.return_code === 5) return { flag: '注意', text: '登录已过期，需要重新登录。' };
  if (model.status && model.status.logged_in === false && model.source === 'cloud') {
    return { flag: '注意', text: '账号还没有登录。登录完成之后，这里会列出绑定的打印机。' };
  }
  if (!model.status && model.ws === 'connecting') return { flag: '注意', text: '正在连接桥接。' };
  if (!model.status && model.ws === 'closed') {
    return { flag: '注意', text: '没有连上桥接。确认本机已经用 --web-api 启动，并且页面和桥接在同一地址。' };
  }
  return null;
}

export function linkState(model) {
  if (model.ws === 'connecting') return { text: '连接中', tone: 'is-draft' };
  if (model.ws !== 'open') return { text: '事件中断', tone: 'is-alert' };
  if (model.status?.logged_in === false) return { text: '未登录', tone: 'is-alert' };
  if (model.status?.server_connected === false) return { text: '云端未连接', tone: 'is-draft' };
  if (model.status?.server_connected === true) return { text: '云端已连接', tone: '' };
  return { text: '已连接', tone: '' };
}
