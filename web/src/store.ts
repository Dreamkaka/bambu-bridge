import { delJson, errorText, getJson, postJson } from './api';
import { isObject, mergeJson, type Json } from './logic';

export type Source = 'cloud' | 'lan';
export type CameraProto = 'local' | 'rtsp' | 'rtsps';
export type BridgeStatus = {
  loaded: boolean;
  agent_ready: boolean;
  logged_in: boolean;
  server_connected: boolean;
  version: string;
  client_version: string;
  country: string;
  mqtt_ready: string[];
};
export type PrinterInfo = {
  dev_id: string;
  dev_name: string;
  dev_online: boolean | null;
  dev_model_name: string;
  dev_product_name: string;
  task_status: string;
};
export type Cache = { report: { [key: string]: Json }; confirmed: boolean; at: number | null };
export type Prefs = { devId: string; source: Source; lanIp: string; lanPassword: string; cameraProto: CameraProto };
export type LanState = { pending: boolean; status: number | null };

const PREF_KEY = 'bambu-bridge-console';

function blankPrefs(): Prefs {
  return { devId: '', source: 'cloud', lanIp: '', lanPassword: '', cameraProto: 'local' };
}

function loadPrefs(): Prefs {
  const prefs = blankPrefs();
  try {
    const raw = localStorage.getItem(PREF_KEY);
    if (!raw) return prefs;
    const parsed: unknown = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object') return prefs;
    if ('devId' in parsed && typeof parsed.devId === 'string' && /^[A-Za-z0-9_-]{0,64}$/.test(parsed.devId)) prefs.devId = parsed.devId;
    if ('source' in parsed && (parsed.source === 'cloud' || parsed.source === 'lan')) prefs.source = parsed.source;
    if ('lanIp' in parsed && typeof parsed.lanIp === 'string' && parsed.lanIp.length <= 64) prefs.lanIp = parsed.lanIp;
    if ('lanPassword' in parsed && typeof parsed.lanPassword === 'string' && parsed.lanPassword.length <= 128) prefs.lanPassword = parsed.lanPassword;
    if ('cameraProto' in parsed && (parsed.cameraProto === 'local' || parsed.cameraProto === 'rtsp' || parsed.cameraProto === 'rtsps')) {
      prefs.cameraProto = parsed.cameraProto;
    }
  } catch {
    return prefs;
  }
  return prefs;
}

function asJson(value: unknown): Json | undefined {
  if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
  if (typeof value === 'number') return Number.isFinite(value) ? value : undefined;
  if (Array.isArray(value)) {
    const out: Json[] = [];
    for (const item of value) {
      const next = asJson(item);
      if (next === undefined) return undefined;
      out.push(next);
    }
    return out;
  }
  if (typeof value === 'object') {
    const out: { [key: string]: Json } = {};
    for (const [key, item] of Object.entries(value)) {
      const next = asJson(item);
      if (next === undefined) return undefined;
      out[key] = next;
    }
    return out;
  }
  return undefined;
}

function normalizeStatus(value: unknown): BridgeStatus {
  const status: BridgeStatus = {
    loaded: false,
    agent_ready: false,
    logged_in: false,
    server_connected: false,
    version: '',
    client_version: '',
    country: '',
    mqtt_ready: [],
  };
  if (!value || typeof value !== 'object') return status;
  if ('loaded' in value && value.loaded === true) status.loaded = true;
  if ('agent_ready' in value && value.agent_ready === true) status.agent_ready = true;
  if ('logged_in' in value && value.logged_in === true) status.logged_in = true;
  if ('server_connected' in value && value.server_connected === true) status.server_connected = true;
  if ('version' in value && typeof value.version === 'string') status.version = value.version;
  if ('client_version' in value && typeof value.client_version === 'string') status.client_version = value.client_version;
  if ('country' in value && typeof value.country === 'string') status.country = value.country;
  if ('mqtt_ready' in value && Array.isArray(value.mqtt_ready)) {
    status.mqtt_ready = value.mqtt_ready.filter((item): item is string => typeof item === 'string');
  }
  return status;
}
function printerText(item: object, key: 'dev_name' | 'dev_model_name' | 'dev_product_name' | 'task_status'): string {
  if (key === 'dev_name' && 'dev_name' in item && typeof item.dev_name === 'string') return item.dev_name;
  if (key === 'dev_model_name' && 'dev_model_name' in item && typeof item.dev_model_name === 'string') return item.dev_model_name;
  if (key === 'dev_product_name' && 'dev_product_name' in item && typeof item.dev_product_name === 'string') return item.dev_product_name;
  if (key === 'task_status' && 'task_status' in item && typeof item.task_status === 'string') return item.task_status;
  return '';
}
function normalizePrinters(value: unknown): PrinterInfo[] {
  if (!value || typeof value !== 'object' || !('printers' in value) || !Array.isArray(value.printers)) return [];
  const out: PrinterInfo[] = [];
  const seen: Record<string, true> = {};
  for (const item of value.printers) {
    if (!item || typeof item !== 'object' || !('dev_id' in item) || typeof item.dev_id !== 'string') continue;
    if (!/^[A-Za-z0-9_-]{1,64}$/.test(item.dev_id) || seen[item.dev_id]) continue;
    seen[item.dev_id] = true;
    const online = 'dev_online' in item && typeof item.dev_online === 'boolean' ? item.dev_online : null;
    out.push({
      dev_id: item.dev_id,
      dev_name: printerText(item, 'dev_name'),
      dev_online: online,
      dev_model_name: printerText(item, 'dev_model_name'),
      dev_product_name: printerText(item, 'dev_product_name'),
      task_status: printerText(item, 'task_status'),
    });
  }
  return out;
}

function textOf(value: Json | undefined): string {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  return '';
}

class Store {
  status: BridgeStatus | null = null;
  printers: PrinterInfo[] = [];
  prefs: Prefs = blankPrefs();
  caches = new Map<string, Cache>();
  lan = new Map<string, LanState>();
  link: 'off' | 'live' | 'retry' = 'off';
  events: { at: number; name: string; detail: string }[] = [];
  problem = '';
  loginNote = '';
  private listeners = new Set<() => void>();
  private ws: WebSocket | null = null;
  private timer = 0;
  private backoff = 500;
  private scheduled = false;
  private reading = false;

  start() {
    this.prefs = loadPrefs();
    this.connect();
  }

  subscribe(fn: () => void) {
    this.listeners.add(fn);
    return () => {
      this.listeners.delete(fn);
    };
  }

  private emit() {
    if (this.scheduled) return;
    this.scheduled = true;
    requestAnimationFrame(() => {
      this.scheduled = false;
      for (const fn of this.listeners) {
        try {
          fn();
        } catch (err) {
          console.error(err);
        }
      }
    });
  }

  save(partial: Partial<Prefs>) {
    this.prefs = { ...this.prefs, ...partial };
    localStorage.setItem(PREF_KEY, JSON.stringify(this.prefs));
    this.emit();
  }

  cache(id = this.prefs.devId, source = this.prefs.source): Cache | null {
    return this.caches.get(`${id}:${source}`) ?? null;
  }

  report(): { [key: string]: Json } {
    return this.cache()?.report ?? {};
  }

  hmsCount(): number {
    const hms = this.report().hms;
    return Array.isArray(hms) ? hms.length : 0;
  }

  private pushEvent(name: string, detail: string) {
    this.events.push({ at: Date.now(), name, detail });
    if (this.events.length > 40) this.events.shift();
  }

  private connect() {
    if (this.ws) return;
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/v1/ws`;
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => {
      this.backoff = 500;
      this.link = 'live';
      this.pushEvent('link', '实时');
      this.emit();
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return;
      let msg: unknown;
      try {
        msg = JSON.parse(ev.data);
      } catch {
        return;
      }
      this.onEvent(msg);
    };
    ws.onclose = () => {
      if (this.ws === ws) this.ws = null;
      this.link = 'retry';
      for (const cache of this.caches.values()) cache.confirmed = false;
      this.pushEvent('link', '正在重连');
      this.emit();
      window.clearTimeout(this.timer);
      this.timer = window.setTimeout(() => this.connect(), this.backoff);
      this.backoff = Math.min(8000, Math.max(500, this.backoff * 2));
    };
  }

  private onEvent(msg: unknown) {
    if (!msg || typeof msg !== 'object' || !('event' in msg) || typeof msg.event !== 'string') return;
    const payload = 'payload' in msg ? asJson(msg.payload) : {};
    if (msg.event === 'ready') {
      this.pushEvent('ready', '桥接已就绪');
      void this.reload(true);
      return;
    }
    if (!payload || !isObject(payload)) return;
    if (msg.event === 'printer_status') {
      const id = typeof payload.dev_id === 'string' ? payload.dev_id : '';
      const source = payload.source === 'lan' || payload.source === 'cloud' ? payload.source : '';
      if (!id || !source || !isObject(payload.report)) return;
      const key = `${id}:${source}`;
      const prev = this.caches.get(key);
      const report = mergeJson(prev?.report ?? {}, payload.report);
      if (!isObject(report)) return;
      this.caches.set(key, { report, confirmed: true, at: Date.now() });
      const before = prev?.report;
      const stateWas = before && isObject(before) ? textOf(before.gcode_state) : '';
      const pctWas = before && isObject(before) ? textOf(before.mc_percent) : '';
      const stateNow = textOf(report.gcode_state);
      const pctNow = textOf(report.mc_percent);
      if (!prev || stateWas !== stateNow || pctWas !== pctNow) {
        const bits = [id, source === 'lan' ? '局域网' : '云端'];
        if (stateNow) bits.push(stateNow);
        if (pctNow) bits.push(`${pctNow}%`);
        this.pushEvent('printer_status', bits.join(' / '));
      }
      this.emit();
      return;
    }
    if (msg.event === 'login_result') {
      this.loginNote = payload.success === true ? '已登录。' : '登录没有完成。';
      this.pushEvent('login_result', this.loginNote);
    }
    if (msg.event === 'account_changed') this.pushEvent('account_changed', payload.logged_in === true ? '已登录' : '已退出');
    if (msg.event === 'cloud_connection') {
      const code = typeof payload.return_code === 'number' ? String(payload.return_code) : '';
      this.pushEvent('cloud_connection', code ? `返回 ${code}` : '连接变化');
    }
    if (msg.event === 'account_changed' || msg.event === 'login_result' || msg.event === 'cloud_connection') {
      void this.reload(false);
      return;
    }
    if (msg.event === 'lan_connection' && typeof payload.dev_id === 'string' && typeof payload.status === 'number') {
      this.lan.set(payload.dev_id, { pending: false, status: payload.status });
      this.pushEvent('lan_connection', `${payload.dev_id} / ${payload.status === 0 ? '已连上' : String(payload.status)}`);
      this.emit();
    }
  }

  async reload(refresh: boolean) {
    if (this.reading) return;
    this.reading = true;
    try {
      this.status = normalizeStatus(await getJson<unknown>('/api/v1/status'));
      this.printers = this.status.logged_in ? normalizePrinters(await getJson<unknown>('/api/v1/printers')) : [];
      if (!this.prefs.devId && this.printers[0]) {
        this.prefs = { ...this.prefs, devId: this.printers[0].dev_id };
        localStorage.setItem(PREF_KEY, JSON.stringify(this.prefs));
      }
      this.problem = '';
      this.emit();
      if (refresh && this.prefs.devId) {
        try {
          await this.command('refresh');
        } catch (err) {
          this.problem = errorText(err);
          this.emit();
        }
      }
    } catch (err) {
      this.problem = errorText(err);
      this.emit();
    } finally {
      this.reading = false;
    }
  }

  command(action: 'pause' | 'resume' | 'stop' | 'refresh') {
    const id = encodeURIComponent(this.prefs.devId);
    const path = action === 'refresh'
      ? `/api/v1/printers/${id}/status/refresh`
      : `/api/v1/printers/${id}/actions/${action}`;
    return postJson<{ dev_id?: string; accepted?: boolean }>(path, { source: this.prefs.source });
  }

  rpc(method: string, params: { [key: string]: Json }) {
    return postJson<Json>('/rpc', { method, params });
  }

  async connectLan() {
    const id = this.prefs.devId;
    this.lan.set(id, { pending: true, status: this.lan.get(id)?.status ?? null });
    this.emit();
    await postJson('/api/v1/lan/connection', {
      dev_id: id,
      dev_ip: this.prefs.lanIp,
      password: this.prefs.lanPassword,
    });
  }

  async disconnectLan() {
    const id = this.prefs.devId;
    this.lan.set(id, { pending: true, status: this.lan.get(id)?.status ?? null });
    this.emit();
    await delJson('/api/v1/lan/connection');
  }
}

export const store = new Store();
