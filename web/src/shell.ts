import { askStop, wireDialog } from './dialog';
import { esc } from './html';
import { mountData } from './data';
import { mountFiles } from './files';
import { mountOverview } from './overview';
import { mountSettings } from './settings';
import { store } from './store';

type Route = 'overview' | 'data' | 'files' | 'settings';

const TITLES: Record<Route, string> = {
  overview: '概览',
  data: '数据',
  files: '文件',
  settings: '设置',
};

let current: Route | '' = '';
let destroy = () => {};
let unitSig = '';
let barStopBusy = false;

function routeName(): Route {
  const name = location.hash.replace(/^#\/?/, '');
  if (name === 'data' || name === 'files' || name === 'settings') return name;
  return 'overview';
}

function paintTelemetry() {
  const host = document.querySelector('#telemetry');
  if (!(host instanceof HTMLElement)) return;
  const rows = store.printers.map((printer) => ({ ...printer, local: false }));
  if (store.prefs.devId && !rows.some((printer) => printer.dev_id === store.prefs.devId)) {
    rows.push({
      dev_id: store.prefs.devId,
      dev_name: '',
      dev_online: null,
      dev_model_name: '',
      dev_product_name: '',
      task_status: '',
      local: true,
    });
  }
  const sig = `${store.prefs.devId}|${rows.map((item) => `${item.dev_id}:${item.dev_name}:${item.dev_product_name}:${item.dev_model_name}:${item.dev_online}`).join(',')}`;
  if (sig !== unitSig || !host.querySelector('#unit')) {
    unitSig = sig;
    const options = rows.length
      ? rows.map((printer) => {
          const model = printer.dev_product_name || printer.dev_model_name || (printer.local ? '手动编号' : '机型未知');
          const name = printer.dev_name || printer.dev_id;
          const selected = printer.dev_id === store.prefs.devId ? ' selected' : '';
          return `<option value="${esc(printer.dev_id)}"${selected}>${esc(name)} / ${esc(model)} / ${esc(printer.dev_id.slice(-4))}</option>`;
        }).join('')
      : '<option value="">还没有打印机</option>';
    host.innerHTML = `<select id="unit" aria-label="打印机"${rows.length ? '' : ' disabled'}>${options}</select><span class="ef-state" id="unit-state"></span><span class="ef-field" id="unit-channel"></span><span class="ef-field" id="unit-mqtt"></span><button class="ef-chip" type="button" id="inspect" aria-pressed="false">字段透视</button><button class="ef-btn small" type="button" id="bar-stop">停止作业</button>`;
  }
  const select = host.querySelector('#unit');
  if (select instanceof HTMLSelectElement && select.value !== store.prefs.devId && Array.from(select.options).some((option) => option.value === store.prefs.devId)) {
    select.value = store.prefs.devId;
  }
  const current = rows.find((item) => item.dev_id === store.prefs.devId);
  const state = host.querySelector('#unit-state');
  if (state) {
    const online = current?.dev_online;
    state.textContent = !current ? '未选择' : online === true ? '在线' : online === false ? '离线' : '在线未知';
    state.className = online === false ? 'ef-state is-hidden' : 'ef-state';
  }
  const channel = host.querySelector('#unit-channel');
  if (channel) channel.textContent = store.prefs.source === 'lan' ? '局域网' : '云端';
  const mqtt = host.querySelector('#unit-mqtt');
  if (mqtt) {
    const count = store.status?.mqtt_ready.length;
    mqtt.textContent = `MQTT 设备 ${count ?? '—'}`;
  }
  const inspect = host.querySelector('#inspect');
  if (inspect) inspect.setAttribute('aria-pressed', document.documentElement.dataset.inspect === 'on' ? 'true' : 'false');
  const stop = host.querySelector('#bar-stop');
  if (stop instanceof HTMLButtonElement) stop.disabled = !store.prefs.devId || barStopBusy;
}

function paintChrome() {
  const name = routeName();
  document.querySelectorAll('[data-route]').forEach((el) => {
    if (el.getAttribute('data-route') === name) el.setAttribute('aria-current', 'page');
    else el.removeAttribute('aria-current');
  });
  const count = document.querySelector('[data-route="overview"] .count');
  const alerts = store.hmsCount();
  if (count instanceof HTMLElement) {
    const show = name !== 'overview' && alerts > 0;
    count.hidden = !show;
    count.textContent = String(alerts);
  }
  const link = document.querySelector('#link');
  if (link) {
    link.textContent = store.link === 'live' ? '实时' : store.link === 'retry' ? '正在重连' : '未连接';
  }
  paintTelemetry();
  document.title = `${TITLES[name]} · Bambu Bridge`;
}

function show() {
  const name = routeName();
  const root = document.querySelector('#app');
  if (!(root instanceof HTMLElement)) return;
  if (name !== current) {
    destroy();
    current = name;
    if (name === 'data') destroy = mountData(root);
    else if (name === 'files') destroy = mountFiles(root);
    else if (name === 'settings') destroy = mountSettings(root);
    else destroy = mountOverview(root);
  }
  paintChrome();
}

async function barStop() {
  if (barStopBusy || !store.prefs.devId) return;
  if (!await askStop()) return;
  barStopBusy = true;
  paintTelemetry();
  try {
    await store.command('stop');
  } finally {
    barStopBusy = false;
    paintTelemetry();
  }
}

function onTelemetryClick(ev: Event) {
  const target = ev.target;
  if (!(target instanceof Element)) return;
  if (target.closest('#inspect')) {
    if (document.documentElement.dataset.inspect === 'on') delete document.documentElement.dataset.inspect;
    else document.documentElement.dataset.inspect = 'on';
    paintTelemetry();
    return;
  }
  if (target.closest('#bar-stop')) void barStop();
}

function onTelemetryChange(ev: Event) {
  const target = ev.target;
  if (!(target instanceof HTMLSelectElement) || target.id !== 'unit') return;
  if (target.value) store.save({ devId: target.value });
}

export function boot() {
  wireDialog();
  document.querySelector('#theme')?.addEventListener('click', () => {
    const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
    document.documentElement.dataset.theme = next;
    localStorage.setItem('theme', next);
  });
  document.querySelector('#telemetry')?.addEventListener('click', onTelemetryClick);
  document.querySelector('#telemetry')?.addEventListener('change', onTelemetryChange);
  store.subscribe(() => {
    if (current === routeName()) paintChrome();
  });
  store.start();
  window.addEventListener('hashchange', show);
  if (!location.hash) location.replace('#/overview');
  show();
}
