import { ApiError } from './api';
import { byteText, clock } from './logic';
import type { Source } from './store';

export const kinds = ['model', 'timelapse', 'video'] as const;
export type Kind = typeof kinds[number];
export type Filter = Kind | 'all';
export type Storage = 'external' | 'internal';
export type Sort = 'new' | 'old' | 'large' | 'name';
export type Context = { devId: string; source: Source; storage: Storage; lanIp: string; lanPassword: string };
export type Entry = { name: string; path?: string; dir: string; kind: Kind; size?: number; modified?: number; date?: string; format: string };
export const category: Record<Filter, string> = { all: '全部', model: '模型', timelapse: '延时摄影', video: '视频' };
export const volume: Record<Storage, string> = { external: '外部存储', internal: '内部存储' };

export function base(c: Context) { return `/api/v1/printers/${encodeURIComponent(c.devId)}/files`; }
export function payload(c: Context, type: Kind, file?: Entry, redact = false) {
  const body: Record<string, string> = { source: c.source, storage: c.storage, type };
  if (c.source === 'lan') {
    body.dev_ip = redact ? '<PRINTER_IP>' : c.lanIp;
    body.password = redact ? '<ACCESS_CODE>' : c.lanPassword;
  }
  if (file) body[file.path ? 'path' : 'name'] = file.path || file.name;
  return body;
}
export function curl(c: Context, file: Entry, action: 'download' | 'delete') {
  const quote = (s: string) => `'${s.replaceAll("'", "'\\''")}'`;
  return `curl --fail-with-body -X POST ${quote(location.origin + base(c) + '/' + action)} -H 'Content-Type: application/json' --data-raw ${quote(JSON.stringify(payload(c, file.kind, file, true)))}${action === 'download' ? ' --output downloaded-file' : ''}`;
}
export function entries(raw: unknown, kind: Kind): Entry[] {
  if (!raw || typeof raw !== 'object' || !('files' in raw) || !Array.isArray(raw.files)) throw new ApiError(200, 'bad_response');
  const out: Entry[] = [];
  for (const item of raw.files) {
    if (!item || typeof item.name !== 'string' || !item.name) continue;
    const path = typeof item.path === 'string' && item.path ? item.path : undefined;
    const date = typeof item.date === 'string' ? item.date : undefined;
    const stamp = typeof item.time === 'number' ? item.time * 1000 : date ? Date.parse(date) : NaN;
    const format = /\.([a-z\d]{1,8})$/i.exec(item.name)?.[1].toUpperCase() ?? 'FILE';
    out.push({ name: item.name, path, kind, date, format,
      dir: path ? path.slice(0, path.lastIndexOf('/')) || '/' : '目录未上报',
      size: Number.isSafeInteger(item.size) && item.size >= 0 ? item.size : undefined,
      modified: Number.isFinite(new Date(stamp).getTime()) ? stamp : undefined,
    });
  }
  return out;
}
export function when(f: Entry) { return f.modified === undefined ? f.date || '—' : clock(f.modified).slice(0, 16).replaceAll('.', '-'); }
export function measured(files: Entry[]) {
  let n = 0, bytes = 0;
  for (const f of files) if (f.size !== undefined) { n++; bytes += f.size; }
  if (!n && files.length) return '大小未上报';
  return `${byteText(bytes)}${n < files.length ? `（${files.length - n} 项大小未知）` : ''}`;
}
const names = new Intl.Collator('zh-CN', { numeric: true });
export function order(a: Entry, b: Entry, sort: Sort) {
  if (sort !== 'name') {
    const key = sort === 'large' ? 'size' : 'modified';
    const x = a[key], y = b[key];
    if (x === undefined && y !== undefined) return 1;
    if (y === undefined && x !== undefined) return -1;
    if (x !== undefined && y !== undefined && x !== y) return sort === 'old' ? x - y : y - x;
  }
  return names.compare(a.name, b.name) || names.compare(a.dir, b.dir);
}
