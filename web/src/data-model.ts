import { annotate, flatten, isObject, isSensitiveKey, meaning, present, trimNum, type Json, type Leaf } from './logic';
import type { Cache } from './store';

export const CAPACITY = 300;
export const TIMEOUT = 15_000;
export const filters = {
  all: '全部', recent: '近期跳变', unknown: '未收录', motion: '设备与运动', ams: 'AMS 供料',
  external: '双头 / 外挂', process: '2D / 3D 工艺', job: '任务与队列', system: '系统 / 网络',
} as const;
export type Filter = keyof typeof filters;
type Domain = Exclude<Filter, 'all' | 'recent' | 'unknown'>;

function domain(path: string): Domain {
  if (/(^|\.)(vir_slot|vt_tray|external_spool|extruder)(\b|_)/i.test(path)) return 'external';
  if (/(^|\.)(2d|3d|refillinfo)\b/i.test(path)) return 'process';
  if (/(^|\.)(ams|rfid|tray_)/i.test(path)) return 'ams';
  if (/(^|\.)(net|online|upgrade|upload|xcam|ipcam|wifi|camera|hms|sdcard|firmware|version|sequence|command)/i.test(path)) return 'system';
  if (/(^|\.)(job|plate|queue|progress|remain|layer|task|gcode|subtask|project|file|print_type|mc_(percent|remaining|print_stage)|stg|timelapse)/i.test(path)) return 'job';
  return 'motion';
}

// Every consumer, including JSON search/copy/download, sees the same safe tree.
function redact(value: Json): Json {
  if (Array.isArray(value)) return value.map(redact);
  return isObject(value) ? redactObject(value) : value;
}
function redactObject(value: Record<string, Json>): Record<string, Json> {
  const out: Record<string, Json> = Object.create(null);
  for (const [key, child] of Object.entries(value)) out[key] = isSensitiveKey(key) ? '已隐藏' : redact(child);
  return out;
}
export type Field = Leaf & {
  label: string | null; shown: string; raw: string; search: string; domain: Domain; scalar: number | null;
  unit: string; changed: number; delta: string;
};
export type Frame = { at: number; gap: boolean; values: Map<string, number | boolean> };
export type Pin = { path: string; label: string; color: number; unit: string; bool: boolean };
export type Branch = { path: string; name: string; count: number; changed: number; children: Map<string, Branch> };
const branch = (path: string, name: string): Branch => ({ path, name, count: 0, changed: 0, children: new Map() });
function addBranch(root: Branch, field: Field) {
  const parts = field.path.match(/[^.[\]]+|\[\d+\]/g) ?? [];
  let node = root;
  let path = '';
  for (let i = 0; ; ++i) {
    node.count++;
    node.changed = Math.max(node.changed, field.changed);
    if (i >= parts.length - 1) break;
    const part = parts[i];
    path += (path && !part.startsWith('[') ? '.' : '') + part;
    let child = node.children.get(part);
    if (!child) { child = branch(path, part); node.children.set(part, child); }
    node = child;
  }
}
function makeField(leaf: Leaf, old: Field | undefined, at: number): Field {
  const v = leaf.value;
  const raw = leaf.hidden ? '已隐藏' : typeof v === 'string' ? v : JSON.stringify(v);
  if (old && old.raw === raw && typeof old.value === typeof v && old.hidden === leaf.hidden) return old;
  const scalar = typeof v === 'boolean' ? +v : typeof v === 'number' && Number.isFinite(v) ? v : null;
  const annotation = scalar !== null ? annotate(leaf.path, v) : null;
  const unit = annotation?.kind === 'unit' ? annotation.text : '';
  const label = meaning(leaf.path);
  const shown = present(leaf.path, v, leaf.hidden);
  let delta = '';
  if (old && !leaf.hidden) {
    if (typeof old.value === 'number' && typeof v === 'number') {
      const diff = v - old.value;
      delta = `${diff >= 0 ? '+' : ''}${trimNum(diff)}${unit}`;
    } else if (typeof old.value === 'boolean' && typeof v === 'boolean') delta = `${old.value ? '是' : '否'}→${v ? '是' : '否'}`;
    else delta = '已变更';
  }
  return { ...leaf, label, shown, raw, search: `${leaf.path} ${label ?? '未收录'} ${shown} ${raw}`, domain: domain(leaf.path), scalar,
    unit, changed: delta ? at : 0, delta };
}

// One instance per mounted source. A cache object is replaced only on printer_status;
// store chrome updates, theme changes, scrolling and chart inspection never append samples.
export class Telemetry {
  private seen: Cache | null = null;
  private gap = false;
  private attemptedDefaults = new Set<string>();
  safe: Record<string, Json> = {};
  fields = new Map<string, Field>();
  tree = branch('', 'ROOT');
  frames: Frame[] = [];
  pins: Pin[] = [];
  at: number | null = null;
  interval: number | null = null;
  frozen = false;
  lost = false;

  update(cache: Cache | null, connected: boolean, now: number) {
    this.lost = !!cache && (!connected || !cache.confirmed || cache.at == null || now - cache.at > TIMEOUT);
    if (this.lost) this.gap = true;
    if (!cache || this.seen === cache) return false;
    this.seen = cache;
    const at = cache.at;
    this.interval = at != null && this.at != null && at > this.at ? at - this.at : null;
    this.at = at;
    this.safe = redactObject(cache.report);
    const next = new Map<string, Field>();
    const values = new Map<string, boolean | number>();
    this.tree = branch('', 'ROOT');
    for (const leaf of flatten(this.safe)) {
      const field = makeField(leaf, this.fields.get(leaf.path), at ?? 0);
      next.set(field.path, field);
      addBranch(this.tree, field);
      if (!field.hidden && (typeof field.value === 'boolean' || typeof field.value === 'number')) values.set(field.path, field.value);
    }
    this.fields = next;
    for (const [kind, candidates] of [['nozzle', ['nozzle_temper', 'device.nozzle.temp', 'nozzle_temp']], ['bed', ['bed_temper', 'device.bed.temp', 'bed_temp']]] as const) {
      if (this.attemptedDefaults.has(kind)) continue;
      const field = candidates.map(path => next.get(path)).find(item => item && item.scalar !== null);
      if (field) { this.pin(field); this.attemptedDefaults.add(kind); }
    }
    if (at !== null && !this.lost && !this.frozen) {
      if (this.frames.length === CAPACITY) this.frames.shift();
      this.frames.push({ at, values, gap: this.gap || (this.interval ?? 0) > TIMEOUT });
      this.gap = false;
    }
    return true;
  }
  pin(field: Field) {
    if (field.scalar === null || field.hidden || this.pins.length === 6 || this.pins.some(p => p.path === field.path)) return;
    let color = 0;
    while (this.pins.some(p => p.color === color)) color++;
    this.pins.push({ path: field.path, label: field.label ?? field.key, unit: field.unit, bool: typeof field.value === 'boolean', color });
  }
  toggleFreeze() { this.frozen = !this.frozen; this.gap = true; }
}

export function searchPattern(query: string) {
  const text = query.trim();
  const terms = text.split(/\s+/).filter(Boolean).map(s => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
  try {
    const regex = text.length > 1 && text.startsWith('/') && text.endsWith('/');
    const patterns = regex ? [text.slice(1, -1)] : terms;
    const tests = patterns.map(p => new RegExp(p, 'i'));
    return { valid: true, matches: (s: string) => tests.every(test => test.test(s)), mark: patterns.length ? new RegExp(patterns.join('|'), 'gi') : null };
  } catch { return { valid: false, matches: (_: string) => false, mark: null }; }
}
