import './data.css';
import { clock, gcodeLabel } from './logic';
import { store } from './store';
import { Drawer, drawerPane } from './data-drawer';
import { Matrix, matrixPane, searchBar } from './data-matrix';
import { Telemetry, TIMEOUT } from './data-model';
import { Scope, scopePane } from './data-scope';
import { button, copyText, download } from './data-view';

export function mountData(root: HTMLElement): () => void {
  root.innerHTML = `<div class="dt-page"><header class="dt-context"><a href="#/overview" aria-label="返回概览">‹ 概览</a><strong class="dt-device"></strong><span class="dt-source"></span><div class="dt-summary"></div><time class="dt-sync"></time>${button('raw', 'RAW_JSON')}${button('snapshot', '导出快照')}</header>${searchBar}<div class="dt-workspace"><div class="dt-tree-rail">${button('tree', '▸ 目录', '', 'aria-label="展开目录树"')}</div>${matrixPane}${scopePane}<div class="dt-scope-rail">${button('scope', '◂ 走势', '', 'aria-label="展开示波器"')}</div></div><nav class="dt-tabs" aria-label="工作台视图">${button('tab', '数据表', 'matrix')}${button('tab', '示波器', 'scope')}</nav><output class="dt-note" role="status"></output>${drawerPane}</div>`;
  const host = root.querySelector<HTMLElement>('.dt-page')!;
  const abort = new AbortController();
  const { signal } = abort;
  const matrix = new Matrix(host, signal), scope = new Scope(host, signal), drawer = new Drawer(host, signal);
  const wide = matchMedia('(min-width: 1440px)'), mobile = matchMedia('(max-width: 1023px)');
  const matrixTab = host.querySelector<HTMLButtonElement>('[data-action="tab"][data-value="matrix"]')!;
  const scopeTab = host.querySelector<HTMLButtonElement>('[data-action="tab"][data-value="scope"]')!;
  let treeOpen = wide.matches, scopeOpen = true, tab = 'matrix';
  let model = new Telemetry(), sourceKey = '', messageTimer = 0;
  function layout() {
    host.classList.toggle('dt-tree-open', treeOpen);
    host.classList.toggle('dt-scope-open', scopeOpen);
    host.classList.toggle('dt-view-scope', tab === 'scope');
    matrixTab.setAttribute('aria-pressed', String(tab === 'matrix'));
    scopeTab.setAttribute('aria-pressed', String(tab === 'scope'));
    host.querySelectorAll('[data-action="tree"]').forEach(b => b.setAttribute('aria-expanded', String(treeOpen)));
    host.querySelectorAll('[data-action="scope"]').forEach(b => b.setAttribute('aria-expanded', String(scopeOpen)));
  }
  const resetLayout = () => { treeOpen = wide.matches; scopeOpen = true; tab = 'matrix'; layout(); };
  wide.addEventListener('change', resetLayout, { signal }); mobile.addEventListener('change', resetLayout, { signal });
  layout();
  function panels() {
    matrix.paint(model); scope.paint(model);
    scopeTab.textContent = `示波器 (${model.pins.length})`;
  }
  function context() {
    const { devId, source } = store.prefs;
    const printer = store.printers.find(p => p.dev_id === devId);
    host.querySelector('.dt-device')!.textContent = printer?.dev_name || devId || '未选择设备';
    host.querySelector('.dt-source')!.textContent = source === 'lan' ? '// 局域网' : '// 云端';
    const rawState = model.safe.gcode_state;
    const state = typeof rawState === 'string' ? gcodeLabel(rawState) ?? rawState : '—';
    const pct = model.fields.get('mc_percent')?.shown;
    const nozzle = model.fields.get('nozzle_temper') ?? model.fields.get('device.nozzle.temp');
    const bed = model.fields.get('bed_temper') ?? model.fields.get('device.bed.temp');
    host.querySelector('.dt-summary')!.textContent = `任务 ${state}${pct ? ' ' + pct : ''}　│　NOZ ${nozzle?.shown ?? '—'}　│　BED ${bed?.shown ?? '—'}`;
    const sync = host.querySelector<HTMLElement>('.dt-sync')!;
    sync.textContent = model.at == null ? '待上报' : `${model.lost ? '上报中断' : '同步'} ${clock(model.at).slice(-8)}`;
    sync.title = model.at == null ? '' : clock(model.at);
    sync.classList.toggle('dt-stale', model.lost);
    host.querySelector<HTMLButtonElement>('[data-action="snapshot"]')!.disabled = model.at == null;
  }
  function onStore() {
    const key = `${store.prefs.devId}:${store.prefs.source}`;
    if (sourceKey !== key) {
      if (drawer.dialog.open) drawer.dialog.close();
      sourceKey = key; model = new Telemetry(); matrix.reset();
      host.querySelector('.dt-note')!.textContent = '';
    }
    model.update(store.cache(), store.link === 'live', Date.now());
    context(); panels();
  }
  const unsubscribe = store.subscribe(onStore);
  onStore();
  // Wall time only ages deltas and breaks a silent stream; this timer never reads or samples a report.
  const ageClock = window.setInterval(() => {
    if (model.at !== null && !model.lost && Date.now() - model.at > TIMEOUT) {
      model.lost = true; context(); scope.paint(model);
    }
    if (model.tree.changed) matrix.paint(model);
  }, 1000);
  async function copy(text: string, inDrawer = false) {
    const target = inDrawer ? drawer.dialog : host;
    const note = target.querySelector(inDrawer ? '.dt-json-note' : '.dt-note')!;
    try { note.textContent = await copyText(text, target) ? '已复制到剪贴板。' : '浏览器未允许复制。'; }
    catch { note.textContent = '浏览器未允许复制。'; }
    if (signal.aborted) return;
    window.clearTimeout(messageTimer); messageTimer = window.setTimeout(() => { note.textContent = ''; }, 2000);
  }
  host.addEventListener('click', ev => {
    const item = ev.target instanceof Element ? ev.target.closest<HTMLButtonElement>('[data-action]') : null;
    if (!item || item.disabled) return;
    const action = item.dataset.action!, value = item.dataset.value ?? '';
    if (action === 'tree') {
      treeOpen = !treeOpen; layout();
      host.querySelector<HTMLButtonElement>(`${treeOpen ? '.dt-tree' : '.dt-tree-rail'} [data-action="tree"]`)?.focus();
    } else if (action === 'scope') {
      scopeOpen = !scopeOpen; layout();
      host.querySelector<HTMLButtonElement>(`${scopeOpen ? '.dt-scope' : '.dt-scope-rail'} [data-action="scope"]`)?.focus();
    } else if (action === 'tab') { tab = value; treeOpen = false; layout(); }
    else if (action === 'raw') drawer.open(model);
    else if (action === 'close-json') drawer.dialog.close();
    else if (action === 'copy-json') void copy(drawer.serialize(), true);
    else if (action === 'snapshot' && model.at !== null) download(JSON.stringify(model.safe, null, 2), 'json', 'telemetry-snapshot', model.at);
    else if (action === 'pin') {
      const index = model.pins.findIndex(p => p.path === value);
      const field = model.fields.get(value);
      if (index >= 0) model.pins.splice(index, 1); else if (field) model.pin(field);
      panels();
    } else if (action === 'copy-path') void copy(value);
    else if (action === 'copy-value') { const f = model.fields.get(value); if (f && !f.hidden) void copy(f.raw); }
    else if (!scope.action(action, value) && matrix.action(action, value) && action === 'prefix' && mobile.matches) {
      treeOpen = false; layout(); host.querySelector<HTMLElement>('.dt-matrix-scroll')?.focus();
    }
  }, { signal });
  window.addEventListener('keydown', ev => {
    if (drawer.dialog.open) {
      if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'k') { ev.preventDefault(); drawer.dialog.querySelector('input')?.focus(); }
      return;
    }
    if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'k') { ev.preventDefault(); tab = 'matrix'; layout(); matrix.focus(); }
    else if (ev.key === 'Escape' && host.contains(ev.target as Node)) {
      ev.preventDefault(); if (mobile.matches) { treeOpen = false; layout(); } matrix.clear();
    }
  }, { signal });
  return () => {
    unsubscribe(); abort.abort(); clearInterval(ageClock); clearTimeout(messageTimer);
    if (drawer.dialog.open) drawer.dialog.close();
  };
}
