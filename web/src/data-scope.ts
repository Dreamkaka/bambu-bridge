import { clock, trimNum } from './logic';
import { esc } from './html';
import { CAPACITY, type Telemetry } from './data-model';
import { button, download, markup } from './data-view';

export const scopePane = `<aside class="dt-scope" aria-label="信号示波器"><header class="dt-pane-title"><h2><small>// SIGNAL_SCOPE</small>信号示波器</h2><span class="dt-channel-count"></span>${button('scope', '▶', '', 'aria-label="折叠示波器"')}</header><div class="dt-scope-tools"></div><div class="dt-pins"></div><div class="dt-chart" tabindex="0" role="group" aria-label="波形画布，使用左右方向键逐帧查看"><canvas aria-hidden="true"></canvas><div class="dt-chart-status"></div><p class="dt-chart-empty"></p><i class="dt-cursor" hidden></i><output class="dt-tip" hidden></output></div><div class="dt-axis"></div><footer><div><small>RING_BUFFER</small><strong class="dt-frame-count"></strong></div>${button('csv', '导出 CSV')}</footer></aside>`;
const shortTime = (t: number) => clock(t).slice(-8);
function csvHeader(path: string) {
  // A device controls the column names; spreadsheets must not execute a header as a formula.
  const safe = /^[\s]*[=+@-]|^[\t\r\n]/.test(path) ? `'${path}` : path;
  return `"${safe.replace(/"/g, '""')}"`;
}

export class Scope {
  private host: HTMLElement;
  private plot: HTMLElement;
  private canvas: HTMLCanvasElement;
  private mode: 'split' | 'overlay' = 'split';
  private index: number | null = null;
  private model!: Telemetry;
  constructor(host: HTMLElement, signal: AbortSignal) {
    this.host = host;
    this.plot = host.querySelector('.dt-chart')!;
    this.canvas = this.plot.querySelector('canvas')!;
    const resize = new ResizeObserver(() => this.draw());
    resize.observe(this.plot);
    const theme = new MutationObserver(() => this.draw());
    theme.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    signal.addEventListener('abort', () => { resize.disconnect(); theme.disconnect(); });
    const inspect = (ev: PointerEvent) => {
      const fs = this.model.frames;
      if (!fs.length) return;
      const r = this.canvas.getBoundingClientRect();
      const fraction = Math.max(0, Math.min(1, (ev.clientX - r.left - 8) / Math.max(1, r.width - 16)));
      const t = fs[0].at + fraction * (fs[fs.length - 1].at - fs[0].at);
      let nearest = 0;
      for (let i = 1; i < fs.length; i++) if (Math.abs(fs[i].at - t) < Math.abs(fs[nearest].at - t)) nearest = i;
      this.index = nearest; this.readout();
    };
    this.plot.addEventListener('pointermove', inspect, { signal });
    this.plot.addEventListener('pointerdown', inspect, { signal });
    const dismiss = () => { this.index = null; this.readout(); };
    this.plot.addEventListener('pointerleave', dismiss, { signal });
    this.plot.addEventListener('blur', dismiss, { signal });
    this.plot.addEventListener('keydown', ev => {
      if (ev.key === 'Escape') { ev.stopPropagation(); dismiss(); return; }
      if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(ev.key) || !this.model.frames.length) return;
      ev.preventDefault(); const last = this.model.frames.length - 1;
      this.index = ev.key === 'Home' ? 0 : ev.key === 'End' ? last : Math.max(0, Math.min(last, (this.index ?? last) + (ev.key === 'ArrowLeft' ? -1 : 1)));
      this.readout();
    }, { signal });
  }
  action(action: string, value: string) {
    if (action === 'mode') this.mode = value === 'overlay' ? 'overlay' : 'split';
    else if (action === 'freeze') this.model.toggleFreeze();
    else if (action === 'clear') { this.model.frames.length = 0; this.index = null; }
    else if (action === 'csv') {
      const { frames, pins } = this.model;
      if (!frames.length || !pins.length) return true;
      const rows = ['timestamp,' + pins.map(p => csvHeader(p.path)).join(',')];
      for (const f of frames) rows.push([new Date(f.at).toISOString(), ...pins.map(p => { const v = f.values.get(p.path); return v === undefined ? '' : +v; })].join(','));
      download('\uFEFF' + rows.join('\r\n'), 'csv', 'telemetry-scope', frames[frames.length - 1].at);
    } else return false;
    this.paint(this.model); return true;
  }
  paint(model: Telemetry) {
    if (this.model !== model) this.index = null;
    this.model = model;
    const { pins, frames, frozen, lost } = model;
    const frame = frames.at(-1);
    this.host.querySelector('.dt-channel-count')!.textContent = `${pins.length} / 6 CH`;
    markup(this.host.querySelector('.dt-scope-tools')!, button('mode', '分轨', 'split', `aria-pressed="${this.mode === 'split'}"`) + button('mode', '叠加', 'overlay', `aria-pressed="${this.mode === 'overlay'}"`) + button('freeze', frozen ? 'Ⅱ HOLD' : lost ? '断流' : frame ? '● LIVE' : '待上报', '', `aria-label="${frozen ? '恢复采样' : '冻结波形'}" aria-pressed="${frozen}"`) + button('clear', '清空', '', `title="清空本页采样缓冲" ${frame ? '' : 'disabled'}`));
    markup(this.host.querySelector('.dt-pins')!, pins.map(p => {
      let min = Infinity, max = -Infinity, sum = 0, n = 0;
      for (const f of frames) { const v = f.values.get(p.path); if (v !== undefined) { min = Math.min(min, +v); max = Math.max(max, +v); sum += +v; n++; } }
      const value = frame?.values.get(p.path);
      const live = !frozen ? model.fields.get(p.path)?.scalar : null;
      const shown = live ?? (value === undefined ? null : +value);
      return `<div class="dt-pin dt-ch${p.color}" title="${esc(p.path)}"><i></i><b>${esc(p.label)}</b><strong>${shown == null ? '—' : trimNum(shown)}<small>${esc(p.unit)}</small></strong>${button('pin', '×', p.path, `aria-label="移除通道 ${esc(p.path)}"`)}<small>MIN <b>${n ? trimNum(min) : '—'}</b>　MAX <b>${n ? trimNum(max) : '—'}</b>　AVG <b>${n ? trimNum(sum / n) : '—'}</b></small></div>`;
    }).join(''));
    this.host.querySelector('.dt-frame-count')!.textContent = `${frames.length} / ${CAPACITY} 帧`;
    this.host.querySelector<HTMLButtonElement>('[data-action="csv"]')!.disabled = !frame || !pins.length;
    const empty = this.host.querySelector<HTMLElement>('.dt-chart-empty')!;
    empty.hidden = !!frame && pins.length > 0;
    empty.textContent = !pins.length ? '在字段行选择「+ 走势」添加通道。' : '收到新的设备上报后开始绘图。';
    this.host.querySelector('.dt-chart-status')!.textContent = lost ? 'SIGNAL_LOST / 上报中断' : frozen ? 'HOLD / 波形已冻结' : '';
    markup(this.host.querySelector('.dt-axis')!, `<time>${frames.length ? shortTime(frames[0].at) : '—'}</time><span>TIME / 本页采样</span><time>${frame ? shortTime(frame.at) : '—'}</time>`);
    this.draw();
  }
  private draw() {
    if (!this.model) return;
    const w = this.canvas.clientWidth, h = this.canvas.clientHeight;
    if (!w || !h) return;
    const ratio = window.devicePixelRatio || 1;
    if (this.canvas.width !== Math.round(w * ratio)) this.canvas.width = Math.round(w * ratio);
    if (this.canvas.height !== Math.round(h * ratio)) this.canvas.height = Math.round(h * ratio);
    const ctx = this.canvas.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0); ctx.clearRect(0, 0, w, h);
    const style = getComputedStyle(this.plot);
    const grid = style.getPropertyValue('--ef-rule').trim(), ink = style.getPropertyValue('--ef-ink-2').trim();
    const { frames, pins } = this.model;
    const lanes = this.mode === 'split' ? Math.max(1, pins.length) : 1;
    const lane = h / lanes;
    ctx.lineWidth = 1; ctx.strokeStyle = grid; ctx.setLineDash([2, 5]); ctx.beginPath();
    for (let i = 0; i <= 6; i++) { const x = 8 + (w - 16) * i / 6; ctx.moveTo(x, 0); ctx.lineTo(x, h); }
    for (let i = 0; i < lanes * 4; i++) { const y = i * lane / 4; ctx.moveTo(0, y); ctx.lineTo(w, y); }
    ctx.stroke(); ctx.setLineDash([]);
    if (!frames.length || !pins.length) { this.readout(); return; }
    const start = frames[0].at, span = frames[frames.length - 1].at - start;
    const scales = pins.map(p => {
      let lo = Infinity, hi = -Infinity;
      for (const f of frames) { const v = f.values.get(p.path); if (v !== undefined) { lo = Math.min(lo, +v); hi = Math.max(hi, +v); } }
      return { lo: p.bool ? 0 : lo, hi: p.bool ? 1 : hi };
    });
    const shared = { lo: Math.min(...scales.map(s => s.lo)), hi: Math.max(...scales.map(s => s.hi)) };
    pins.forEach((p, idx) => {
      let { lo, hi } = this.mode === 'split' ? scales[idx] : shared;
      if (!Number.isFinite(lo) || !Number.isFinite(hi)) return;
      if (lo === hi) { const pad = Math.max(1, Math.abs(lo) * .04); lo -= pad; hi += pad; }
      const top = this.mode === 'split' ? idx * lane : 0;
      const pad = Math.min(18, lane / 4);
      if (this.mode === 'split' || idx === 0) {
        ctx.fillStyle = ink; ctx.font = '10px monospace';
        if (lane > 40) { ctx.fillText(`${trimNum(hi)}${this.mode === 'split' ? p.unit : ''}`, 9, top + 12); ctx.fillText(trimNum(lo), 9, top + lane - 5); }
        ctx.fillStyle = grid; ctx.fillRect(0, top, w, 1);
      }
      const color = style.getPropertyValue(`--dt-ch${p.color}`).trim();
      ctx.strokeStyle = color; ctx.fillStyle = color; ctx.lineWidth = 1.7; ctx.beginPath();
      let prev: number | null = null;
      for (const f of frames) {
        const value = f.values.get(p.path);
        if (value === undefined) { prev = null; continue; }
        const x = 8 + (span ? (f.at - start) / span : 0) * (w - 16);
        const y = top + pad + (1 - (+value - lo) / (hi - lo)) * (lane - pad * 2);
        if (prev === null || f.gap) { ctx.moveTo(x, y); ctx.fillRect(x - 1.5, y - 1.5, 3, 3); }
        else { if (typeof value === 'boolean') ctx.lineTo(x, prev); ctx.lineTo(x, y); }
        prev = y;
      }
      ctx.stroke();
    });
    if (this.model.lost) {
      ctx.strokeStyle = ink; ctx.setLineDash([4, 4]); ctx.beginPath(); ctx.moveTo(span ? w - 8 : 8, 0); ctx.lineTo(span ? w - 8 : 8, h); ctx.stroke(); ctx.setLineDash([]);
    }
    this.readout();
  }
  private readout() {
    const cursor = this.plot.querySelector<HTMLElement>('.dt-cursor')!, tip = this.plot.querySelector<HTMLElement>('.dt-tip')!;
    const frames = this.model?.frames;
    const frame = this.index !== null && frames ? frames[Math.min(frames.length - 1, this.index)] : undefined;
    cursor.hidden = tip.hidden = !frame || !this.model.pins.length;
    if (!frame || !frames?.length || tip.hidden) return;
    const span = frames[frames.length - 1].at - frames[0].at;
    const frac = span ? (frame.at - frames[0].at) / span : 0;
    cursor.style.left = `${8 + frac * (this.plot.clientWidth - 16)}px`;
    tip.classList.toggle('dt-tip-left', frac > .5);
    markup(tip, `<b>${shortTime(frame.at)}</b>` + this.model.pins.map(p => {
      const v = frame.values.get(p.path);
      return `<span>${esc(p.label)} <strong>${v === undefined ? '—' : trimNum(+v) + esc(p.unit)}</strong></span>`;
    }).join(''));
  }
}
