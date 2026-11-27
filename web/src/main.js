import '@fontsource-variable/saira/standard.css';
import '@fontsource-variable/noto-sans-sc/wght.css';
import './ef/endfield.css';
import './style.css';
import mpegts from 'mpegts.js';
import { flashDone, toast, wireDialogs, wireTheme } from './ef/fx.js';
import * as api from './api.js';
import {
  applyEvent, canCommand, createModel, markUnconfirmed, pushEvent, validDeviceId, validIpv4, validPassword,
} from './model.js';
import { createView, stopCopy } from './view.js';

const ACCEPTED = '已接受。等机器回报，先不要重发。';
const model = createModel();
const views = new Set(['job', 'printers', 'camera']);

function viewFromHash() {
  const name = location.hash.slice(1);
  return views.has(name) ? name : 'job';
}

try {
  const savedId = localStorage.getItem('bambu-bridge.device');
  const savedSource = localStorage.getItem('bambu-bridge.source');
  if (validDeviceId(savedId)) model.selectedId = savedId;
  if (savedSource === 'lan' || savedSource === 'cloud') model.source = savedSource;
} catch { /* private mode */ }

model.view = viewFromHash();
const view = createView(document.querySelector('#app'), {
  onSource,
  onSelect,
  onLogin,
  onRefresh: () => refreshSelected(true),
  onPause: () => command('pause'),
  onResume: () => command('resume'),
  onStop: () => command('stop'),
  onLan,
  onLanOff,
  onSnap: snap,
  onLive: startLive,
  onLiveStop: () => stopLive(),
  stopCopy: () => stopCopy(model),
});

wireTheme();
wireDialogs();

function paint() {
  view.paint(model);
}

function persist() {
  try {
    if (model.selectedId) localStorage.setItem('bambu-bridge.device', model.selectedId);
    else localStorage.removeItem('bambu-bridge.device');
    localStorage.setItem('bambu-bridge.source', model.source);
  } catch { /* private mode */ }
}

let shotUrl = '';
function clearShot() {
  if (shotUrl) URL.revokeObjectURL(shotUrl);
  shotUrl = '';
  model.shotUrl = '';
}

let player = null;
let liveGen = 0;
function stopLive(shouldPaint = true) {
  liveGen += 1;
  if (player) {
    try {
      player.pause();
      player.unload();
      player.detachMediaElement();
      player.destroy();
    } catch { /* already gone */ }
    player = null;
  }
  model.live = false;
  if (shouldPaint) paint();
}

function cameraParams() {
  if (!validDeviceId(model.selectedId)) {
    model.fault = { flag: '注意', text: '先选定一台打印机。' };
    return null;
  }
  const params = new URLSearchParams();
  params.set('dev_id', model.selectedId);
  if (model.source === 'lan') {
    const form = view.readLan();
    if (form.dev_id !== model.selectedId || !validIpv4(form.dev_ip) || !validPassword(form.password)) {
      model.fault = { flag: '出错', text: '局域网画面要使用这台机器在设备页填写的地址和访问码。' };
      return null;
    }
    params.set('ip', form.dev_ip);
    params.set('password', form.password);
    params.set('proto', view.readProto());
  }
  return params;
}

async function send(name, button, fn, { flash, toastText } = {}) {
  if (model.pending) return;
  model.pending = name;
  if (button) button.setAttribute('aria-busy', 'true');
  paint();
  try {
    const response = await fn();
    if (response?.result?.accepted === false) throw new Error('桥接没有确认接受。');
    model.fault = null;
    const text = toastText === undefined ? (response?.status === 202 ? ACCEPTED : '') : toastText;
    if (text) toast(text);
    if (button && flash) flashDone(button, flash);
    else if (button) button.removeAttribute('aria-busy');
  } catch (error) {
    if (button) button.removeAttribute('aria-busy');
    model.fault = { flag: '出错', text: api.explain(error) };
  } finally {
    model.pending = null;
    paint();
  }
}

let directoryRunning = false;
let directoryRefresh = false;
function loadDirectory(refresh) {
  if (refresh) directoryRefresh = true;
  if (directoryRunning) return;
  directoryRunning = true;
  (async () => {
    try {
      do {
        const refreshNow = directoryRefresh;
        directoryRefresh = false;
        await loadOnce(refreshNow);
      } while (directoryRefresh);
    } catch (error) {
      model.banner = { flag: '出错', text: api.explain(error) };
      paint();
    } finally {
      directoryRunning = false;
      if (directoryRefresh) loadDirectory(false);
    }
  })();
}

async function loadOnce(refresh) {
  const status = await api.status();
  model.status = status.result;
  model.banner = null;
  try {
    const listed = await api.printers();
    model.printers = Array.isArray(listed.result?.printers) ? listed.result.printers : [];
  } catch (error) {
    if (error instanceof api.ApiError && error.code === 'login_required') model.printers = [];
    else throw error;
  }
  paint();
  if (refresh && canCommand(model)) await refreshSelected(false);
}

async function onLogin() {
  const popup = window.open('about:blank', 'bambu-login');
  try {
    const { result } = await api.loginUrl();
    const url = safeHttpUrl(result && result.url);
    if (!url) throw new Error('桥接没有返回登录地址。');
    if (popup) popup.location = url;
    else {
      model.loginHref = url;
      paint();
    }
  } catch (error) {
    if (popup) popup.close();
    model.fault = { flag: '出错', text: api.explain(error) };
    paint();
  }
}

function safeHttpUrl(value) {
  if (typeof value !== 'string') return '';
  try {
    const url = new URL(value);
    return url.protocol === 'https:' || url.protocol === 'http:' ? url.href : '';
  } catch {
    return '';
  }
}

function refreshSelected(announce) {
  if (!canCommand(model)) {
    model.fault = {
      flag: '注意',
      text: model.source === 'lan' ? '局域网通道要先连上这台机器。' : '云端刷新需要先登录。',
    };
    paint();
    return Promise.resolve();
  }
  return send('refresh', announce ? view.button('refresh') : null, () => api.refresh(model.selectedId, model.source), {
    flash: announce ? '已发出' : undefined,
    toastText: announce ? undefined : '',
  });
}

function command(name) {
  if (!canCommand(model)) return;
  send(name, view.button(name), () => api.action(model.selectedId, name, model.source), { flash: '已发出' });
}

function onSource(source) {
  if ((source !== 'cloud' && source !== 'lan') || source === model.source) return;
  model.source = source;
  clearShot();
  stopLive(false);
  persist();
  paint();
}

function onSelect(id) {
  if (!validDeviceId(id) || id === model.selectedId) return;
  model.selectedId = id;
  clearShot();
  stopLive(false);
  persist();
  paint();
  if (canCommand(model)) refreshSelected(false);
}


function onLan(fields) {
  if (!validDeviceId(fields.dev_id)) {
    model.fault = { flag: '出错', text: '序列号只能包含字母、数字、下划线和连字符。' };
    paint();
    return;
  }
  if (!validIpv4(fields.dev_ip)) {
    model.fault = { flag: '出错', text: '地址要写成四段数字的 IPv4，不能带前导 0。' };
    paint();
    return;
  }
  if (!validPassword(fields.password)) {
    model.fault = { flag: '出错', text: '访问码只能是 1 到 128 个可见英文字符。' };
    paint();
    return;
  }
  send('lan', view.button('lan-connect'), async () => {
    const response = await api.connectLan({
      dev_id: fields.dev_id,
      dev_ip: fields.dev_ip,
      password: fields.password,
    });
    model.lanPending = fields.dev_id;
    model.selectedId = fields.dev_id;
    model.source = 'lan';
    persist();
    return response;
  }, { toastText: '连接请求已接受。等局域网回报，先不要重发。' });
}

function onLanOff() {
  send('lan-off', view.button('lan-off'), async () => {
    const response = await api.disconnectLan();
    model.lanLink = null;
    model.lanPending = null;
    return response;
  }, { flash: '已断开', toastText: '已断开局域网。' });
}

async function snap() {
  const params = cameraParams();
  if (!params) {
    paint();
    return;
  }
  await send('snap', view.button('snap'), async () => {
    const blob = await api.snapshot(params);
    if (shotUrl) URL.revokeObjectURL(shotUrl);
    shotUrl = URL.createObjectURL(blob);
    model.shotUrl = shotUrl;
    return { status: 200, result: { accepted: true } };
  }, { toastText: '' });
}

function startLive() {
  const params = cameraParams();
  if (!params) {
    paint();
    return;
  }
  stopLive(false);
  if (!mpegts.isSupported()) {
    model.fault = { flag: '注意', text: '这个浏览器不能播直播。可以拍一张，或用 VLC 打开同一地址。' };
    paint();
    return;
  }
  const gen = liveGen;
  const video = document.getElementById('live');
  player = mpegts.createPlayer(
    { type: 'mse', isLive: true, url: `/camera.ts?${params.toString()}` },
    { enableWorker: false, lazyLoad: false },
  );
  player.attachMediaElement(video);
  player.on(mpegts.Events.ERROR, () => {
    if (gen !== liveGen) return;
    model.fault = { flag: '出错', text: '直播没有连上。' };
    stopLive();
  });
  player.load();
  player.play();
  model.live = true;
  model.fault = null;
  paint();
}

function onSocketState(state) {
  if (state === 'open') {
    model.wsEverOpen = true;
    pushEvent(model, '事件通道已连上');
  } else if (state === 'closed' && model.ws === 'open') {
    markUnconfirmed(model);
    pushEvent(model, '事件通道断开');
  } else if (state === 'closed') {
    markUnconfirmed(model);
  }
  model.ws = state;
  paint();
}

function onSocketEvent(message) {
  if (message.event === 'ready') {
    loadDirectory(true);
    return;
  }
  const text = applyEvent(model, message);
  if (text) pushEvent(model, text);
  if (message.event === 'login_result' || message.event === 'account_changed') loadDirectory(true);
  paint();
}

window.addEventListener('hashchange', () => {
  const next = viewFromHash();
  if (model.view === 'camera' && next !== 'camera') stopLive(false);
  model.view = next;
  paint();
});

paint();
api.openEvents(onSocketEvent, onSocketState);
