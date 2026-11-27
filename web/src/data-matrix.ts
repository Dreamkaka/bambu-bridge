import { esc } from './html';
import { clock } from './logic';
import { type Branch, type Field, type Filter, filters, searchPattern, type Telemetry } from './data-model';
import { button, marked, markup } from './data-view';

const ROW = 36;
export const searchBar = `<div class="dt-searchbar"><span aria-hidden="true">//</span><input id="dt-search" type="search" autocomplete="off" aria-label="搜索名称、路径或读数" placeholder="搜索名称、路径或读数（空格分词、/正则/）"><kbd>Ctrl / ⌘ K</kbd>${button('reset-query', '×', '', 'aria-label="清空搜索"')}<small class="dt-measure"></small></div><div class="dt-filters" role="group" aria-label="语义过滤"></div>`;
export const matrixPane = `<aside class="dt-tree" aria-label="命名空间目录"><header class="dt-pane-title"><h2><small>// NAMESPACE</small>命名空间</h2>${button('tree', '◀', '', 'aria-label="折叠目录树"')}</header><div class="dt-nodes"></div><footer>按路径定位字段</footer></aside>
<section class="dt-matrix" aria-label="遥测矩阵"><header class="dt-pane-title"><h1><small>// TELEMETRY MATRIX</small>遥测矩阵</h1><div class="dt-match"></div></header><div class="dt-matrix-scroll" tabindex="0" role="region" aria-label="滚动浏览遥测字段"><div class="dt-table" role="table" aria-label="字段读数"><div class="dt-columns" role="row"><span role="columnheader">路径 <small>PATH</small></span><span role="columnheader">含义</span><span role="columnheader">读数</span><span role="columnheader">变更</span><span role="columnheader">操作</span></div><div class="dt-spacer" role="rowgroup"><div class="dt-window" role="presentation"></div></div></div></div><p class="dt-matrix-empty"></p></section>`;

export class Matrix {
  private host: HTMLElement;
  private scroll: HTMLElement;
  private spacer: HTMLElement;
  private slice: HTMLElement;
  private input: HTMLInputElement;
  private observer: ResizeObserver;
  private raf = 0;
  private filter: Filter = 'all';
  private prefix = '';
  private expanded = new Set(['']);
  private search = searchPattern('');
  private visible: Field[] = [];
  private model!: Telemetry;
  constructor(host: HTMLElement, signal: AbortSignal) {
    this.host = host;
    this.scroll = host.querySelector('.dt-matrix-scroll')!;
    this.spacer = host.querySelector('.dt-spacer')!;
    this.slice = host.querySelector('.dt-window')!;
    this.input = host.querySelector('#dt-search')!;
    this.input.addEventListener('input', () => this.queryChanged(), { signal });
    const schedule = () => { if (!this.raf) this.raf = requestAnimationFrame(() => { this.raf = 0; this.drawRows(); }); };
    this.scroll.addEventListener('scroll', schedule, { passive: true, signal });
    this.observer = new ResizeObserver(schedule);
    this.observer.observe(this.scroll);
    signal.addEventListener('abort', () => { this.observer.disconnect(); cancelAnimationFrame(this.raf); });
  }
  reset() { this.prefix = ''; this.expanded = new Set(['']); this.scroll.scrollTop = 0; }
  focus() { this.input.focus(); this.input.select(); }
  clear() { this.input.value = ''; this.queryChanged(); this.focus(); }
  private queryChanged() { this.search = searchPattern(this.input.value); this.scroll.scrollTop = 0; this.paint(this.model); }
  action(action: string, value: string) {
    if (action === 'reset-query') { this.clear(); return true; }
    if (action === 'filter' && Object.hasOwn(filters, value)) this.filter = value as Filter;
    else if (action === 'prefix') { this.prefix = value; this.expanded.add(value); }
    else if (action === 'expand') {
      if (this.expanded.has(value)) this.expanded.delete(value); else this.expanded.add(value);
    } else return false;
    if (action !== 'expand') this.scroll.scrollTop = 0;
    this.paint(this.model);
    return true;
  }
  paint(model: Telemetry) {
    this.model = model;
    const now = Date.now();
    const counts = { all: 0, recent: 0, unknown: 0, motion: 0, ams: 0, external: 0, process: 0, job: 0, system: 0 };
    this.visible = [];
    for (const f of model.fields.values()) {
      const recent = f.changed > 0 && now - f.changed < 30000;
      counts.all++; counts[f.domain]++; if (!f.label) counts.unknown++; if (recent) counts.recent++;
      if (this.prefix && f.path !== this.prefix && !f.path.startsWith(this.prefix + '.') && !f.path.startsWith(this.prefix + '[')) continue;
      if (this.filter !== 'all' && this.filter !== f.domain && !(this.filter === 'recent' && recent) && !(this.filter === 'unknown' && !f.label)) continue;
      if (this.search.matches(f.search)) this.visible.push(f);
    }
    if (this.filter === 'recent') this.visible.sort((a, b) => b.changed - a.changed);
    markup(this.host.querySelector('.dt-filters')!, (Object.keys(filters) as Filter[]).map(key => button('filter', `${filters[key]} <b>${counts[key]}</b>`, key, `aria-pressed="${this.filter === key}"`)).join(''));
    this.host.querySelector('.dt-measure')!.textContent = model.interval ? `上报间隔 ${(model.interval / 1000).toFixed(1)} s` : '按上报采样';
    this.host.querySelector('.dt-match')!.textContent = `${this.prefix || 'ROOT'} / ${this.visible.length} 项`;
    const nodes: string[] = [];
    const walk = (b: Branch, depth: number) => {
      const open = this.expanded.has(b.path);
      nodes.push(`<div class="dt-node" style="--depth:${depth}">${b.children.size ? button('expand', open ? '▾' : '▸', b.path, `aria-label="展开或折叠 ${esc(b.name)}" aria-expanded="${open}"`) : '<i></i>'}${button('prefix', `<span>${esc(b.name)}</span><small>${b.changed && now - b.changed < 30000 ? 'ϟ ' : ''}${b.count}</small>`, b.path, `aria-pressed="${this.prefix === b.path}" title="${esc(b.path || 'ROOT')}"`)}</div>`);
      if (open) for (const child of b.children.values()) walk(child, depth + 1);
    };
    walk(model.tree, 0);
    markup(this.host.querySelector('.dt-nodes')!, nodes.join(''));
    const empty = this.host.querySelector<HTMLElement>('.dt-matrix-empty')!;
    empty.hidden = !!this.visible.length;
    empty.textContent = !this.search.valid ? '正则表达式无效，请检查括号和转义字符。' : model.fields.size ? '没有匹配的字段。请调整搜索、目录或过滤条件。' : '设备上报后，这里会列出可检索的遥测字段。';
    this.input.setAttribute('aria-invalid', String(!this.search.valid));
    this.spacer.style.height = `${this.visible.length * ROW}px`;
    this.host.querySelector('.dt-table')!.setAttribute('aria-rowcount', String(this.visible.length + 1));
    this.drawRows();
  }
  private drawRows() {
    if (!this.model || !this.scroll.clientHeight) return;
    const start = Math.max(0, Math.floor(this.scroll.scrollTop / ROW) - 4);
    const stop = Math.min(this.visible.length, start + Math.ceil(this.scroll.clientHeight / ROW) + 9);
    this.slice.style.transform = `translateY(${start * ROW}px)`;
    const now = Date.now();
    const rows: string[] = [];
    for (let i = start; i < stop; i++) {
      const f = this.visible[i];
      const pinned = this.model.pins.some(p => p.path === f.path);
      const split = Math.max(f.path.lastIndexOf('.') + 1, f.path.endsWith(']') ? f.path.lastIndexOf('[') : 0);
      const h = (s: string) => marked(s, this.search.mark);
      const age = now - f.changed;
      const delta = f.changed ? `${f.delta} / ${Math.floor(age / 1000)}s` : '—';
      const scope = button('pin', pinned ? '● 监视中' : '+ 走势', f.path, `aria-pressed="${pinned}" aria-label="${pinned ? '移除走势' : '添加走势'} ${esc(f.path)}" ${f.scalar === null || (!pinned && this.model.pins.length >= 6) ? 'disabled' : ''}`);
      rows.push(`<div class="dt-row${f.changed && age < 800 ? ' dt-flash' : ''}" role="row" aria-rowindex="${i + 2}"><div role="cell" class="dt-path" title="${esc(f.path)}"><span><em>${h(f.path.slice(0, split))}</em><b>${h(f.path.slice(split))}</b></span>${button('copy-path', '⧉', f.path, `aria-label="复制路径 ${esc(f.path)}" title="复制路径"`)}</div><div role="cell" class="dt-label" title="${esc(f.label ?? '未收录')}">${f.label ? h(f.label) : `<span>${h('未收录')}</span>`}</div><div role="cell" class="dt-value dt-${typeof f.value}" title="${esc(f.shown)}"><span>${h(f.shown)}</span></div><div role="cell" class="dt-delta" title="${f.changed ? esc(`${clock(f.changed)} / ${f.delta}`) : '尚未变更'}">${esc(delta)}</div><div role="cell" class="dt-row-actions">${scope}${button('copy-value', '⧉', f.path, `aria-label="复制值 ${esc(f.path)}" title="复制值" ${f.hidden ? 'disabled' : ''}`)}</div></div>`);
    }
    markup(this.slice, rows.join(''));
  }
}
