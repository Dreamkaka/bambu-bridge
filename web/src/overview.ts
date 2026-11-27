import { errorText } from './api';
import { bindHtml, bindText } from './bind';
import { mountCamera } from './camera';
import { askStop } from './dialog';
import { esc } from './html';
import {
  clock,
  colorHex,
  gcodeLabel,
  hmsCode,
  minutes,
  num,
  obj,
  percentText,
  present,
  stageLabel,
  str,
  trayPlace,
  type Json,
} from './logic';
import { store } from './store';

let busy = '';
let notice = '';
let flag = '注意';
let rootEl: HTMLElement | null = null;
let sideSig = '';
let stick = true;
let eventFilter: 'all' | 'status' | 'link' = 'all';

function tone(state: string): string {
  const key = state.toUpperCase();
  if (key === 'FAILED') return 'ef-state is-alert';
  if (key === 'PAUSE' || key === 'PREPARE' || key === 'SLICING' || key === 'INIT') return 'ef-state is-hidden';
  return 'ef-state';
}

function inkOn(hex: string): string {
  const r = Number.parseInt(hex.slice(1, 3), 16);
  const g = Number.parseInt(hex.slice(3, 5), 16);
  const b = Number.parseInt(hex.slice(5, 7), 16);
  return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255 > 0.62 ? '#161616' : '#ededed';
}

function slotCard(title: string, type: string, remain: number | null, color: string | null, current: boolean): string {
  const style = color ? ` style="background:${color};color:${inkOn(color)}"` : '';
  const rest = remain == null ? '' : remain < 0 ? '剩余未知' : `剩余 ${remain}%`;
  return `<div class="slot${current ? ' is-now' : ''}"${style}${current ? ' aria-current="true"' : ''}><b>${esc(title)}</b><span>${esc(type || '空槽')}</span><span class="ef-field">${esc(rest)}</span></div>`;
}

function sideMarkup(report: { [key: string]: Json }, now: string): string {
  const cards: string[] = [];
  const ams = obj(report.ams);
  const units = ams && Array.isArray(ams.ams) ? ams.ams : [];
  const notes: string[] = [];
  for (const unit of units) {
    const record = obj(unit);
    if (!record) continue;
    const id = str(record.id) ?? '';
    const temp = num(record.temp);
    const humidity = str(record.humidity) ?? (num(record.humidity) != null ? String(num(record.humidity)) : '');
    if (id) notes.push(`AMS ${id}${temp != null ? ` / ${temp} ℃` : ''}${humidity ? ` / 湿度档 ${humidity}` : ''}`);
    const trays = Array.isArray(record.tray) ? record.tray : [];
    trays.forEach((tray, index) => {
      const item = obj(tray);
      const slot = item ? str(item.id) ?? String(index) : String(index);
      const unitNum = /^\d+$/.test(id) ? Number(id) : Number.NaN;
      const slotNum = /^\d+$/.test(slot) ? Number(slot) : Number.NaN;
      const flat = unitNum >= 0 && unitNum < 16 && slotNum >= 0 && slotNum < 4 ? String(unitNum * 4 + slotNum) : '';
      const type = item ? str(item.tray_type) ?? '' : '';
      const remain = item ? num(item.remain) : null;
      const color = item ? colorHex(item.tray_color ?? null) : null;
      const feeding = now !== '' && now !== '255' && ((flat !== '' && flat === now) || (unitNum >= 128 && unitNum <= 135 && id === now));
      cards.push(slotCard(`AMS ${id || '?'} / 槽 ${slot}`, type, remain, color, feeding));
    });
  }
  const external = obj(report.vt_tray);
  if (external) {
    const id = str(external.id) ?? '254';
    cards.push(slotCard('外挂料盘', str(external.tray_type) ?? '', num(external.remain), colorHex(external.tray_color ?? null), now !== '255' && now === id));
  }
  if (Array.isArray(report.vir_slot)) {
    for (const tray of report.vir_slot) {
      const item = obj(tray);
      if (!item) continue;
      const id = str(item.id) ?? '';
      const type = str(item.tray_type) ?? '';
      if (!type && id !== '254' && id !== '255') continue;
      const title = id === '255' ? '右外挂' : id === '254' ? '左外挂' : `外挂 ${id}`;
      cards.push(slotCard(title, type, num(item.remain), colorHex(item.tray_color ?? null), now !== '255' && now === id));
    }
  }
  if (!cards.length) return '<p class="ef-empty">这台机器还没有上报耗材槽。有 AMS 或外挂料盘时，槽位会列在这里。</p>';
  return `${notes.length ? `<p class="ef-field">${esc(notes.join(' / '))}</p>` : ''}<div class="slots">${cards.join('')}</div>`;
}


function linkLabel(): string {
  if (store.link === 'live') return '实时';
  if (store.link === 'retry') return '正在重连';
  return '未连接';
}

function lightOf(report: { [key: string]: Json }): { node: string; mode: string } | null {
  const raw = report.lights_report;
  const items = Array.isArray(raw) ? raw : raw == null ? [] : [raw];
  for (const item of items) {
    const record = obj(item);
    if (!record) continue;
    const mode = str(record.mode);
    if (!mode) continue;
    return { node: str(record.node) || 'chamber_light', mode };
  }
  return null;
}

function lightLabel(mode: string): string {
  const key = mode.trim().toLowerCase();
  if (key === 'on') return '开';
  if (key === 'off') return '关';
  if (key === 'flashing') return '闪';
  return mode;
}

function unpackTemp(value: Json | undefined) {
  const word = num(value);
  return word != null && Number.isInteger(word) && word >= 0 && word <= 0xffffffff
    ? { current: word & 0xffff, target: word >>> 16 } : null;
}

function toolMarkup(report: { [key: string]: Json }): string {
  const device = obj(report.device);
  for (const group of ['nozzle', 'extruder']) {
    const info = obj(device?.[group])?.info;
    if (!Array.isArray(info)) continue;
    const slots = info.flatMap((raw, index) => {
      const item = obj(raw);
      if (!item) return [];
      const addr = num(item.addr_id);
      const label = addr != null ? `T${String(addr + 1).padStart(2, '0')}` : `${group === 'nozzle' ? '喷嘴' : '挤出机'} ${num(item.id) ?? index}`;
      const temp = unpackTemp(item.temp);
      const size = num(item.diameter);
      const color = colorHex(item.color_m ?? null);
      const path = `device.${group}.info[${index}].temp`;
      return [`<div class="tool-cell"${color ? ` style="--material:${color}"` : ''}><b>${label}</b><strong>${temp?.current ?? '—'} ℃</strong><span>${size == null ? '—' : `${size} mm`}</span><small>${esc(str(item.type) || '')}</small><button class="inspect" type="button" data-path="${path}">字段</button></div>`];
    });
    if (slots.length) return slots.join('');
  }
  const t = num(report.nozzle_temper), size = num(report.nozzle_diameter), target = num(report.nozzle_target_temper);
  return `<div class="tool-cell"><b>喷嘴</b><strong>${t ?? '—'} ℃</strong><span>${size == null ? '—' : `${size} mm`}</span><small>目标 ${target ?? '—'} ℃</small><button class="inspect" type="button" data-path="nozzle_temper">字段</button></div>`;
}

function fanMarkup(report: { [key: string]: Json }): string {
  const fans: [string, Json | undefined][] = [
    ['喉管风扇', report.heatbreak_fan_speed],
    ['部件冷却', report.cooling_fan_speed],
    ['辅助风扇', report.big_fan1_speed],
    ['腔体风扇', report.big_fan2_speed],
  ];
  const rows = fans.filter(([, value]) => value != null).map(([name, value]) => `<li><span>${name}</span><span class="ef-field">${esc(String(value))}</span></li>`);
  const parts = obj(obj(report.device)?.airduct)?.parts;
  if (Array.isArray(parts)) for (const part of parts) {
    const item = obj(part);
    if (!item || num(item.id) == null) continue;
    rows.push(`<li><span>风道 ${num(item.id)}</span><span class="ef-field">状态 ${num(item.state) ?? '—'} / 目标 ${num(item.tar_state) ?? '—'}</span></li>`);
  }
  return rows.length ? `<ul class="ef-ledger">${rows.join('')}</ul>` : '<p class="ef-empty">风扇转速还没有上报。</p>';
}

function bridgeMarkup(wifi: string, when: string): string {
  const bridge = store.status;
  const rows: [string, string][] = [
    ['HTTP REST', location.host],
    ['WebSocket', linkLabel()],
    ['MQTT 就绪设备', bridge ? `${bridge.mqtt_ready.length} 台` : '—'],
    ['版本', bridge?.version || '还没有读到'],
    ['区域', bridge?.country || '还没有读到'],
    ['客户端', bridge?.client_version || '还没有读到'],
    ['插件', bridge ? (bridge.loaded ? '已加载' : '未加载') : '还没有读到'],
    ['登录', bridge ? (bridge.logged_in ? '已登录' : '未登录') : '还没有读到'],
    ['服务器', bridge ? (bridge.server_connected ? '已连接' : '未连接') : '还没有读到'],
    ['代理', bridge ? (bridge.agent_ready ? '就绪' : '未就绪') : '还没有读到'],
    ['无线', wifi || '还没有上报'],
    ['上报时间', when],
  ];
  const ids = (bridge?.mqtt_ready ?? []).slice(0, 4).map((id) => `<li><span>设备</span><span class="ef-field">${esc(id)}</span></li>`).join('');
  const more = bridge && bridge.mqtt_ready.length > 4 ? `<li><span>其余</span><span class="ef-field">${bridge.mqtt_ready.length - 4} 台</span></li>` : '';
  const line = ([name, value]: [string, string]) => `<li><span>${esc(name)}</span><span class="ef-field">${esc(value)}</span></li>`;
  return `<ul class="ef-ledger">${rows.slice(0, 3).map(line).join('')}</ul><details><summary>连接详情</summary><ul class="ef-ledger">${rows.slice(3).map(line).join('')}${ids}${more}</ul></details>`;
}

function eventMarkup(): string {
  const rows = store.events.filter((item) => {
    if (eventFilter === 'status') return item.name === 'printer_status';
    if (eventFilter === 'link') return item.name !== 'printer_status';
    return true;
  });
  if (!rows.length) return '<p class="ef-empty">这里还没有事件。连接、登录和打印状态变化后，会按时间列在这里。</p>';
  const names: Record<string, string> = {
    link: '连接',
    ready: '就绪',
    printer_status: '状态',
    login_result: '登录',
    account_changed: '账号',
    cloud_connection: '云端',
    lan_connection: '局域网',
  };
  return `<ul class="ef-ledger">${rows.slice(-16).map((item) => `<li><span class="ef-field">${esc(clock(item.at).slice(11))} ${esc(names[item.name] || item.name)}</span><span>${esc(item.detail)}</span></li>`).join('')}</ul>`;
}


function paint() {
  const root = rootEl;
  if (!root) return;
  const cache = store.cache();
  const report = cache?.report ?? {};
  const state = str(report.gcode_state) ?? '';
  const stateLabel = state ? gcodeLabel(state) ?? state : cache ? '状态未知' : '还没有上报';
  const percent = num(report.mc_percent);
  const layer = num(report.layer_num);
  const total = num(report.total_layer_num);
  const bed = num(report.bed_temper);
  const bedTarget = num(report.bed_target_temper);
  const device = obj(report.device);
  const ctc = unpackTemp(obj(obj(device?.ctc)?.info)?.temp);
  const chamber = ctc?.current ?? num(report.chamber_temper);
  const chamberTarget = ctc?.target ?? num(report.chamber_target_temper);
  const remain = num(report.mc_remaining_time);
  const job = str(report.subtask_name) || (str(report.gcode_file) ? (str(report.gcode_file) ?? '').split('/').pop() || '' : '');
  const stage = stageLabel(num(report.mc_print_stage));
  const speed = num(report.spd_lvl);
  const speedName = speed === 1 ? '静音' : speed === 2 ? '标准' : speed === 3 ? '运动' : speed === 4 ? '狂暴' : '';
  const wifi = str(report.wifi_signal) ?? '';
  const tray = str(obj(report.ams)?.tray_now) ?? (num(obj(report.ams)?.tray_now) != null ? String(num(obj(report.ams)?.tray_now)) : '');

  bindText(root, 'job', job || (cache ? '没有任务名' : '还没有上报'));
  bindText(root, 'state', stateLabel);
  const stateEl = root.querySelector('[data-bind="state"]');
  if (stateEl) stateEl.className = tone(state);
  const percentLabel = percent == null ? '—' : percentText(percent);
  bindText(root, 'percent', percentLabel);
  const bar = root.querySelector('[data-bind="progress"]');
  if (bar instanceof HTMLElement) {
    if (percent == null) {
      bar.hidden = true;
      bar.style.removeProperty('--p');
      bar.removeAttribute('aria-valuenow');
      bar.setAttribute('aria-valuetext', '还没有百分比');
    } else {
      const clamped = Math.max(0, Math.min(100, percent));
      bar.hidden = false;
      bar.style.setProperty('--p', String(clamped / 100));
      bar.setAttribute('aria-valuenow', String(clamped));
      bar.removeAttribute('aria-valuetext');
    }
  }
  bindText(root, 'remain', remain == null ? '—' : minutes(remain));
  bindText(root, 'layers', layer == null && total == null ? '—' : `${layer ?? '—'} / ${total ?? '—'}`);
  bindText(root, 'stage', stage || '—');
  bindText(root, 'speed', speedName ? `${speedName}${num(report.spd_mag) != null ? ` / ${num(report.spd_mag)}%` : ''}` : '—');
  bindHtml(root, 'tools', toolMarkup(report));
  bindText(root, 'bed', bed == null ? '无' : String(Math.round(bed * 10) / 10));
  bindText(root, 'bed-target', bedTarget == null ? '目标还没有上报' : `目标 ${bedTarget} ℃`);
  const chamberBlock = root.querySelector('[data-bind="chamber-block"]');
  if (chamberBlock instanceof HTMLElement) chamberBlock.hidden = chamber == null && chamberTarget == null;
  bindText(root, 'chamber', chamber == null ? '无' : String(Math.round(chamber * 10) / 10));
  bindText(root, 'chamber-target', chamberTarget == null ? '目标还没有上报' : `目标 ${chamberTarget} ℃`);
  const chamberPath = root.querySelector<HTMLButtonElement>('[data-bind="chamber-path"]');
  if (chamberPath) chamberPath.textContent = chamberPath.dataset.path = ctc ? 'device.ctc.info.temp' : 'chamber_temper';
  const duct = num(obj(device?.airduct)?.modeCur);
  const ductEl = root.querySelector('[data-bind="airduct"]');
  if (ductEl instanceof HTMLElement) {
    ductEl.hidden = duct == null;
    if (duct != null) ductEl.textContent = `风道 / ${present('device.airduct.modeCur', duct)}`;
  }
  bindText(root, 'tray', tray ? trayPlace(tray) ?? tray : '当前槽还没有上报');
  bindHtml(root, 'fans', fanMarkup(report));
  const bridgePanel = root.querySelector('[data-bind="bridge"]');
  const detailsOpen = bridgePanel?.querySelector('details')?.open;
  const bridgeHtml = bridgeMarkup(wifi, cache?.at ? clock(cache.at) : '还没有上报时间');
  bindHtml(root, 'bridge', bridgeHtml.replace('<details>', detailsOpen ? '<details open="">' : '<details>'));
  bindHtml(root, 'events', eventMarkup());
  const stream = root.querySelector('[data-bind="events"]');
  if (stick && stream instanceof HTMLElement) stream.scrollTop = stream.scrollHeight;
  root.querySelectorAll<HTMLButtonElement>('[data-filter]').forEach((button) => {
    button.setAttribute('aria-pressed', button.dataset.filter === eventFilter ? 'true' : 'false');
  });
  const stickBtn = root.querySelector('[data-act="stick"]');
  if (stickBtn) stickBtn.textContent = stick ? '暂停滚动' : '继续滚动';
  const nextSide = JSON.stringify({ ams: report.ams ?? null, vt: report.vt_tray ?? null, vir: report.vir_slot ?? null, tray });
  if (nextSide !== sideSig) {
    sideSig = nextSide;
    bindHtml(root, 'side', sideMarkup(report, tray));
  }

  const codes = (Array.isArray(report.hms) ? report.hms : []).map(hmsCode).filter((code): code is string => !!code);
  const hms = root.querySelector('[data-bind="hms"]');
  if (hms instanceof HTMLElement) {
    hms.hidden = codes.length === 0;
    hms.textContent = codes.length
      ? `${codes.slice(0, 6).join(' / ')}${codes.length > 6 ? `，其余 ${codes.length - 6} 条` : ''}`
      : '';
  }
  const stale = root.querySelector('[data-bind="stale"]');
  if (stale instanceof HTMLElement) stale.hidden = !cache || cache.confirmed;
  const note = root.querySelector('[data-bind="notice"]');
  if (note instanceof HTMLElement) {
    const text = notice || store.problem;
    note.hidden = !text;
    note.dataset.flag = notice ? flag : '出错';
    note.textContent = text;
  }

  const light = lightOf(report);
  const lightBtn = root.querySelector('[data-act="light"]');
  if (lightBtn instanceof HTMLButtonElement) {
    lightBtn.hidden = !light;
    lightBtn.textContent = light && light.mode.trim().toLowerCase() === 'on' ? '关灯' : '开灯';
    lightBtn.dataset.mode = light?.mode ?? '';
    lightBtn.dataset.node = light?.node ?? '';
    const lamp = root.querySelector('[data-bind="lamp"]');
    if (lamp instanceof HTMLElement) {
      lamp.hidden = !light;
      lamp.textContent = light ? `灯 / ${lightLabel(light.mode)}` : '';
    }
  }
  root.querySelectorAll<HTMLButtonElement>('[data-speed]').forEach((button) => {
    const level = Number(button.dataset.speed);
    const on = speedName !== '' && level === speed;
    button.setAttribute('aria-pressed', on ? 'true' : 'false');
    button.disabled = !store.prefs.devId || (!!busy && busy !== `speed:${level}`);
    button.setAttribute('aria-busy', busy === `speed:${level}` ? 'true' : 'false');
  });
  root.querySelectorAll<HTMLButtonElement>('[data-act]').forEach((button) => {
    const act = button.dataset.act || '';
    if (act === 'fullscreen' || act === 'stick' || act === 'copy') return;
    if (act === 'light') {
      button.disabled = !store.prefs.devId || (!!busy && busy !== 'light');
      button.setAttribute('aria-busy', busy === 'light' ? 'true' : 'false');
      return;
    }
    button.disabled = !store.prefs.devId || (!!busy && busy !== act);
    button.setAttribute('aria-busy', busy === act ? 'true' : 'false');
  });
}

async function submitted(run: () => Promise<{ status: number }>, hold: string) {
  if (busy || !store.prefs.devId) return;
  busy = hold;
  paint();
  try {
    const result = await run();
    notice = result.status === 202
      ? '已提交，等打印机回报。不会因为这次回应就改打印状态。'
      : '已提交，等打印机回报。';
    flag = '注意';
  } catch (err) {
    notice = errorText(err);
    flag = '出错';
  } finally {
    busy = '';
    paint();
  }
}

async function act(action: 'pause' | 'resume' | 'stop' | 'refresh') {
  if (busy || !store.prefs.devId) return;
  if (action === 'stop' && !await askStop()) return;
  await submitted(() => store.command(action), action);
}

async function setSpeed(level: string) {
  if (level !== '1' && level !== '2' && level !== '3' && level !== '4') return;
  await submitted(() => store.rpc('print_speed', {
    dev_id: store.prefs.devId,
    level,
    lan: store.prefs.source === 'lan',
  }), `speed:${level}`);
}

async function setLight(node: string, mode: string) {
  const next = mode.trim().toLowerCase() === 'on' ? 'off' : 'on';
  await submitted(() => store.rpc('set_light', {
    dev_id: store.prefs.devId,
    lan: store.prefs.source === 'lan',
    led_node: node || 'chamber_light',
    led_mode: next,
    loop_times: 0,
    interval_time: 0,
  }), 'light');
}

function onClick(ev: Event) {
  const target = ev.target;
  if (!(target instanceof Element)) return;
  const pathEl = target.closest('.inspect');
  if (pathEl instanceof HTMLElement && pathEl.dataset.path) {
    void navigator.clipboard.writeText(pathEl.dataset.path);
    notice = `已复制 ${pathEl.dataset.path}`;
    flag = '注意';
    paint();
    return;
  }
  const filterBtn = target.closest('[data-filter]');
  if (filterBtn instanceof HTMLButtonElement && (filterBtn.dataset.filter === 'all' || filterBtn.dataset.filter === 'status' || filterBtn.dataset.filter === 'link')) {
    eventFilter = filterBtn.dataset.filter;
    paint();
    return;
  }
  const speedBtn = target.closest('[data-speed]');
  if (speedBtn instanceof HTMLButtonElement && speedBtn.dataset.speed) {
    void setSpeed(speedBtn.dataset.speed);
    return;
  }
  const button = target.closest('[data-act]');
  if (!(button instanceof HTMLButtonElement) || !button.dataset.act) return;
  const action = button.dataset.act;
  if (action === 'pause' || action === 'resume' || action === 'stop' || action === 'refresh') void act(action);
  if (action === 'light') void setLight(button.dataset.node || '', button.dataset.mode || '');
  if (action === 'stick') {
    stick = !stick;
    paint();
  }
  if (action === 'copy') {
    void navigator.clipboard.writeText(JSON.stringify(store.report()));
    notice = '已复制当前上报。';
    flag = '注意';
    paint();
  }
  if (action === 'fullscreen') {
    const frame = rootEl?.querySelector('[data-bind="frame"]');
    if (frame instanceof HTMLElement && frame.requestFullscreen) void frame.requestFullscreen();
  }
}

export function mountOverview(root: HTMLElement): () => void {
  rootEl = root;
  sideSig = '';
  notice = '';
  root.innerHTML = `
    <div class="hud">
      <p class="ef-notice" data-bind="hms" data-flag="注意" hidden></p>
      <p class="ef-notice" data-bind="stale" data-flag="注意" hidden>连接中断，这些读数还没重新确认。</p>
      <p class="ef-notice" data-bind="notice" hidden></p>
      <div class="hud-stage">
        <div class="hud-column jobs">
        <section class="ef-panel" data-block="camera">
          <h2 class="ef-pair"><span class="ef-eyebrow">Camera</span><span>画面</span></h2>
          <figure class="hud-shot" data-bind="frame">
            <video data-camera aria-label="腔体实时画面" muted playsinline hidden></video>
            <figcaption class="ef-empty" data-camera-note></figcaption>
          <p class="ef-field" data-bind="lamp" hidden></p>
          <p class="ef-actions">
            <button class="ef-btn small" type="button" data-act="light" hidden>开灯</button>
            <button class="ef-btn small" type="button" data-camera-toggle>播放监控</button>
            <button class="ef-btn small" type="button" data-act="fullscreen" hidden>全屏</button>
          </p>
          </figure>
        </section>
        <section class="ef-panel" data-block="job">
          <h2 class="ef-pair"><span class="ef-eyebrow">Job</span><span>作业</span><span class="ef-state" data-bind="state">还没有上报</span></h2>
          <p class="job-name" data-bind="job">还没有上报</p>
          <div class="job-progress"><strong class="mega" data-bind="percent">—</strong>
            <div class="ef-progress" data-bind="progress" role="progressbar" aria-label="作业进度" aria-valuemin="0" aria-valuemax="100"><i></i></div>
          </div>
          <dl class="job-facts">
            <div><dt>当前层 / 总层数</dt><dd data-bind="layers"></dd></div>
            <div><dt>剩余时间</dt><dd data-bind="remain"></dd></div>
            <div><dt>打印阶段</dt><dd data-bind="stage"></dd></div>
            <div><dt>速度档</dt><dd data-bind="speed"></dd></div>
          </dl>
          <p class="ef-actions">
            <button class="ef-chip" type="button" data-speed="1" aria-pressed="false">静音</button>
            <button class="ef-chip" type="button" data-speed="2" aria-pressed="false">标准</button>
            <button class="ef-chip" type="button" data-speed="3" aria-pressed="false">运动</button>
            <button class="ef-chip" type="button" data-speed="4" aria-pressed="false">狂暴</button>
          </p>
          <p class="ef-actions">
            <button class="ef-btn go" type="button" data-act="refresh">请求上报</button>
            <button class="ef-btn" type="button" data-act="pause">暂停</button>
            <button class="ef-btn" type="button" data-act="resume">继续</button>
            <button class="ef-btn" type="button" data-act="stop">停止</button>
          </p>
        </section>
        </div>
        <div class="hud-column hardware">
        <section class="ef-panel" data-block="tools">
          <h2 class="ef-pair"><span class="ef-eyebrow">Tools</span><span>工具头</span></h2>
          <div class="tool-rack" data-bind="tools"></div>
        </section>
        <section class="ef-panel" data-block="thermal">
          <h2 class="ef-pair"><span class="ef-eyebrow">Thermal &amp; airflow</span><span>温控与风道</span></h2>
          <div class="ef-figs">
            <p>
              <span>热床 / ℃</span>
              <b data-bind="bed">—</b>
              <span class="ef-field" data-bind="bed-target"></span>
              <button class="inspect" type="button" data-path="bed_temper">bed_temper</button>
            </p>
            <p data-bind="chamber-block">
              <b data-bind="chamber">无</b>
              <span>腔温 / ℃</span>
              <span class="ef-field" data-bind="chamber-target"></span>
              <button class="inspect" type="button" data-bind="chamber-path">字段</button>
            </p>
          </div>
          <p class="ef-field" data-bind="airduct" hidden></p>
          <div data-bind="fans"></div>
        </section>
        <section class="ef-panel" data-block="spools">
          <h2 class="ef-pair"><span class="ef-eyebrow">Spools</span><span>耗材</span></h2>
          <p class="ef-field" data-bind="tray"></p>
          <div class="side-scroll" data-bind="side"></div>
        </section>
        </div>
        <div class="hud-column gateway">
        <section class="ef-panel" data-block="bridge">
          <h2 class="ef-pair"><span class="ef-eyebrow">Bridge</span><span>桥接</span><a href="#/data">全部字段</a></h2>
          <div data-bind="bridge"></div>
        </section>
        <section class="ef-panel" data-block="consumers">
          <h2 class="ef-pair"><span class="ef-eyebrow">Callers</span><span>调用方</span></h2>
          <p class="ef-empty">当前接口不提供调用方列表。</p>
        </section>
        <section class="ef-panel" data-block="stream">
          <h2 class="ef-pair"><span class="ef-eyebrow">Events</span><span>事件</span></h2>
          <p class="ef-actions">
            <button class="ef-chip" type="button" data-filter="all" aria-pressed="true">全部</button>
            <button class="ef-chip" type="button" data-filter="status" aria-pressed="false">状态</button>
            <button class="ef-chip" type="button" data-filter="link" aria-pressed="false">连接</button>
          </p>
          <div data-bind="events"></div>
          <p class="ef-actions">
            <button class="ef-btn small" type="button" data-act="stick">暂停滚动</button>
            <button class="ef-btn small" type="button" data-act="copy">复制上报</button>
          </p>
        </section>
        </div>
      </div>
    </div>`;
  root.addEventListener('click', onClick);
  const releaseCamera = mountCamera(root);
  const unsub = store.subscribe(paint);
  paint();
  return () => {
    unsub();
    releaseCamera();
    root.removeEventListener('click', onClick);
    rootEl = null;
  };
}
