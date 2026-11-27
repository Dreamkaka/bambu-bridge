import { clock, type Json } from './logic';
import { esc } from './html';
import { button, marked } from './data-view';
import type { Telemetry } from './data-model';

export const drawerPane = `<dialog class="dt-drawer" aria-labelledby="dt-drawer-title"><div class="dt-drawer-body"><header class="dt-pane-title"><h2 id="dt-drawer-title"><small>// RAW_JSON</small>原始上报</h2>${button('close-json', '×', '', 'aria-label="关闭原始 JSON"')}</header><div class="dt-drawer-meta"></div><label>查找键名或读数<input class="dt-json-search" type="search" autocomplete="off" placeholder="输入关键词" autofocus></label><div class="dt-json-content"></div><footer><span>敏感值已隐藏</span>${button('copy-json', '复制整包 JSON')}</footer><output class="dt-json-note" role="status"></output></div></dialog>`;

// Inspect a timestamped snapshot; incoming pushes must not collapse branches or move keyboard focus.
export class Drawer {
  readonly dialog: HTMLDialogElement;
  private safe: Record<string, Json> = {};
  private input: HTMLInputElement;
  constructor(host: HTMLElement, signal: AbortSignal) {
    this.dialog = host.querySelector('.dt-drawer')!;
    this.input = this.dialog.querySelector('input')!;
    this.input.addEventListener('input', () => this.draw(), { signal });
    this.dialog.addEventListener('click', ev => { if (ev.target === this.dialog) this.dialog.close(); }, { signal });
    this.dialog.addEventListener('close', () => {
      this.safe = {}; this.dialog.querySelector('.dt-json-content')!.replaceChildren();
    }, { signal });
  }
  open(model: Telemetry) {
    this.safe = model.safe;
    this.input.value = '';
    this.dialog.querySelector('.dt-json-note')!.textContent = '';
    this.dialog.querySelector('.dt-drawer-meta')!.textContent = `${model.at === null ? '尚未上报' : clock(model.at)} / ${model.fields.size} 项`;
    this.draw(); this.dialog.showModal(); this.input.focus();
  }
  serialize() { return JSON.stringify(this.safe, null, 2); }
  private draw() {
    const q = this.input.value.trim().toLowerCase();
    const pattern = q ? new RegExp(q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'gi') : null;
    const walk = (key: string, value: Json, include: boolean, depth: number): string => {
      const keyHit = !!q && key.toLowerCase().includes(q);
      if (value !== null && typeof value === 'object') {
        let children = '';
        for (const [k, v] of Object.entries(value)) children += walk(Array.isArray(value) ? `[${k}]` : k, v, include || keyHit, depth + 1);
        if (q && !include && !keyHit && !children) return '';
        const n = Object.keys(value).length;
        return `<details${depth === 0 || q ? ' open' : ''}><summary>${marked(key, pattern)} <small>${Array.isArray(value) ? `[${n}]` : `{${n}}`}</small></summary><div>${children}</div></details>`;
      }
      const raw = JSON.stringify(value);
      if (q && !include && !keyHit && !raw.toLowerCase().includes(q)) return '';
      return `<p><b>${marked(key, pattern)}</b>: <span>${marked(raw, pattern)}</span></p>`;
    };
    this.dialog.querySelector('.dt-json-content')!.innerHTML = walk('ROOT', this.safe, false, 0) || `<p>没有找到「${esc(this.input.value)}」。</p>`;
  }
}
