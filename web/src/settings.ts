import { errorText, getJson } from './api';
import { bindText } from './bind';
import { esc } from './html';
import { store, type CameraProto, type Source } from './store';

let rootEl: HTMLElement | null = null;
let notice = '';

function setInput(selector: string, value: string) {
  const input = rootEl?.querySelector(selector);
  if (!(input instanceof HTMLInputElement) || document.activeElement === input || input.value === value) return;
  input.value = value;
}

function lanText(): string {
  const state = store.lan.get(store.prefs.devId);
  if (!state) return '还没有局域网连接回报。';
  if (state.pending) return '正在等待打印机。';
  if (state.status === 0) return '局域网已连接。';
  return state.status == null ? '还没有局域网连接回报。' : `局域网未连通，状态 ${state.status}。`;
}

function paint() {
  const root = rootEl;
  if (!root) return;
  const status = store.status;
  bindText(root, 'version', status?.version || '还没有读到');
  bindText(root, 'client', status?.client_version || '还没有读到');
  bindText(root, 'country', status?.country || '还没有读到');
  bindText(root, 'loaded', status ? (status.loaded ? '已加载' : '未加载') : '未知');
  bindText(root, 'agent', status ? (status.agent_ready ? '就绪' : '未就绪') : '未知');
  bindText(root, 'login', status ? (status.logged_in ? '已登录' : '未登录') : '未知');
  bindText(root, 'cloud', status ? (status.server_connected ? '已连接' : '未连接') : '未知');
  bindText(root, 'mqtt', status ? (status.mqtt_ready.length ? status.mqtt_ready.join(' / ') : '无') : '还没有读到');
  bindText(root, 'lan-state', lanText());
  bindText(root, 'login-note', store.loginNote);
  const note = root.querySelector('[data-bind="notice"]');
  if (note instanceof HTMLElement) {
    const text = notice || store.problem;
    note.hidden = !text;
    note.textContent = text;
  }
  setInput('[name="devId"]', store.prefs.devId);
  setInput('[name="lanIp"]', store.prefs.lanIp);
  setInput('[name="lanPassword"]', store.prefs.lanPassword);
  root.querySelectorAll<HTMLInputElement>('input[name="source"]').forEach((input) => {
    input.checked = input.value === store.prefs.source;
  });
  root.querySelectorAll<HTMLInputElement>('input[name="proto"]').forEach((input) => {
    input.checked = input.value === store.prefs.cameraProto;
  });
  const picks = root.querySelector('[data-bind="picks"]');
  const sig = store.printers.map((printer) => printer.dev_id).join(',');
  if (picks && picks.getAttribute('data-sig') !== `${sig}|${store.prefs.devId}`) {
    picks.setAttribute('data-sig', `${sig}|${store.prefs.devId}`);
    picks.innerHTML = store.printers.length
      ? store.printers.map((printer) => {
          const current = printer.dev_id === store.prefs.devId;
          return `<button type="button" class="ef-tag${current ? ' is-current' : ''}" data-pick="${esc(printer.dev_id)}"${current ? ' aria-current="true"' : ''}>${esc(printer.dev_name || printer.dev_id)}</button>`;
        }).join('')
      : '<p class="ef-empty">登录后，账号里的打印机会出现在这里。局域网设备也可以直接填编号。</p>';
  }
}

async function login() {
  notice = '';
  paint();
  try {
    const result = await getJson<unknown>('/api/v1/session/login-url');
    const url = result && typeof result === 'object' && 'url' in result && typeof result.url === 'string' ? result.url : '';
    if (!url) {
      notice = '没有拿到登录地址。';
      paint();
      return;
    }
    window.open(url, '_blank', 'noopener');
    notice = '登录窗口已打开。完成后回到这一页即可。';
  } catch (err) {
    notice = errorText(err);
  }
  paint();
}

async function connect() {
  notice = '';
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(store.prefs.devId)) {
    notice = '设备编号需要 1 到 64 位字母、数字、下划线或连字符。';
    paint();
    return;
  }
  if (!/^(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)$/.test(store.prefs.lanIp)) {
    notice = '局域网地址要写成 IPv4，例如 192.168.1.10。';
    paint();
    return;
  }
  if (!store.prefs.lanPassword) {
    notice = '还没有填写访问码。';
    paint();
    return;
  }
  try {
    await store.connectLan();
    notice = '已提交局域网连接。等打印机回报之后，上面的状态才会变成已连接。';
  } catch (err) {
    notice = errorText(err);
  }
  paint();
}

async function disconnect() {
  notice = '';
  try {
    await store.disconnectLan();
    notice = '已提交断开。';
  } catch (err) {
    notice = errorText(err);
  }
  paint();
}

function onClick(ev: Event) {
  const target = ev.target;
  if (!(target instanceof Element)) return;
  if (target.closest('[data-bind="login"]')) {
    void login();
    return;
  }
  if (target.closest('[data-bind="disconnect"]')) {
    void disconnect();
    return;
  }
  const pick = target.closest('[data-pick]');
  if (pick instanceof HTMLElement && pick.dataset.pick) store.save({ devId: pick.dataset.pick });
}

function onInput(ev: Event) {
  const target = ev.target;
  if (!(target instanceof HTMLInputElement)) return;
  if (target.name === 'devId') store.save({ devId: target.value.trim() });
  if (target.name === 'lanIp') store.save({ lanIp: target.value.trim() });
  if (target.name === 'lanPassword') store.save({ lanPassword: target.value });
  if (target.name === 'source' && (target.value === 'cloud' || target.value === 'lan')) {
    const source: Source = target.value;
    store.save({ source });
  }
  if (target.name === 'proto' && (target.value === 'local' || target.value === 'rtsp' || target.value === 'rtsps')) {
    const cameraProto: CameraProto = target.value;
    store.save({ cameraProto });
  }
}

function onSubmit(ev: Event) {
  ev.preventDefault();
  void connect();
}

export function mountSettings(root: HTMLElement): () => void {
  rootEl = root;
  notice = '';
  root.innerHTML = `
    <div class="page reading">
      <header class="ef-head"><h1>设置 <small>Bridge</small></h1><p>登录、选择云端或局域网，并填写画面要用的地址。区域和插件目录在启动命令里，这里只能查看。</p></header>
      <p class="ef-notice" data-bind="notice" data-flag="注意" hidden></p>
      <section class="ef-panel">
        <h2><span class="ef-pair"><span class="ef-eyebrow">Process</span><span>桥接</span></span></h2>
        <ul class="ef-ledger">
          <li><span>版本</span><span class="ef-field" data-bind="version"></span></li>
          <li><span>客户端版本</span><span class="ef-field" data-bind="client"></span></li>
          <li><span>区域</span><span class="ef-field" data-bind="country"></span></li>
          <li><span>插件</span><span class="ef-field" data-bind="loaded"></span></li>
          <li><span>代理</span><span class="ef-field" data-bind="agent"></span></li>
          <li><span>账号</span><span class="ef-field" data-bind="login"></span></li>
          <li><span>云端</span><span class="ef-field" data-bind="cloud"></span></li>
          <li><span>MQTT 就绪</span><span class="ef-field" data-bind="mqtt"></span></li>
        </ul>
      </section>
      <section>
        <h2 class="ef-pair"><span class="ef-eyebrow">Account</span><span>账号</span></h2>
        <p class="ef-lede ef-read">登录会打开新窗口。完成后这个页面自己更新。</p>
        <p class="ef-field" data-bind="login-note"></p>
        <p class="ef-actions"><button class="ef-btn go" type="button" data-bind="login">登录</button></p>
      </section>
      <section>
        <h2 class="ef-pair"><span class="ef-eyebrow">Channel</span><span>通道</span></h2>
        <p class="ef-lede ef-read">上报、暂停和文件都走这里选中的通道。</p>
        <div class="ef-seg" role="radiogroup" aria-label="通道">
          <label><input type="radio" name="source" value="cloud"><span>云端</span></label>
          <label><input type="radio" name="source" value="lan"><span>局域网</span></label>
        </div>
      </section>
      <form class="ef-form">
        <h2 class="ef-pair"><span class="ef-eyebrow">Printer</span><span>打印机</span></h2>
        <div class="printer-row" data-bind="picks"></div>
        <label class="field"><span>设备编号</span><input name="devId" autocomplete="off" maxlength="64"><small>账号列表里没有的局域网机器，把序列号填在这里。</small></label>
        <label class="field"><span>局域网地址</span><input name="lanIp" inputmode="decimal" autocomplete="off" placeholder="192.168.1.10"></label>
        <label class="field"><span>访问码</span><input name="lanPassword" type="password" autocomplete="off" maxlength="128"><small>只保存在这台浏览器里。画面和文件请求会把它发给本机桥接。</small></label>
        <p class="ef-field" data-bind="lan-state"></p>
        <p class="ef-actions">
          <button class="ef-btn go" type="submit">连接局域网</button>
          <button class="ef-btn" type="button" data-bind="disconnect">断开局域网</button>
        </p>
      </form>
      <section>
        <h2 class="ef-pair"><span class="ef-eyebrow">Camera</span><span>画面</span></h2>
        <p class="ef-lede ef-read">局域网画面协议。云端不使用这项。</p>
        <div class="ef-seg" role="radiogroup" aria-label="画面协议">
          <label><input type="radio" name="proto" value="local"><span>本地</span></label>
          <label><input type="radio" name="proto" value="rtsps"><span>RTSPS</span></label>
          <label><input type="radio" name="proto" value="rtsp"><span>RTSP</span></label>
        </div>
      </section>
    </div>`;
  root.addEventListener('click', onClick);
  root.addEventListener('input', onInput);
  root.addEventListener('change', onInput);
  root.addEventListener('submit', onSubmit);
  const unsub = store.subscribe(paint);
  paint();
  return () => {
    unsub();
    root.removeEventListener('click', onClick);
    root.removeEventListener('input', onInput);
    root.removeEventListener('change', onInput);
    root.removeEventListener('submit', onSubmit);
    rootEl = null;
  };
}
