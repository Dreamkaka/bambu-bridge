import { drawDial } from './ef/dial.js';
import {
  actionGates, canCommand, currentReport, displayJob, heatGauge, jobHasReadings, linkState, noticeText, stateLabel,
} from './model.js';

const SVG = 'http://www.w3.org/2000/svg';

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text != null) node.textContent = text;
  return node;
}

function svg(tag, attrs, text) {
  const node = document.createElementNS(SVG, tag);
  for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, String(value));
  if (text != null) node.textContent = text;
  return node;
}

function printerName(printer) {
  for (const key of ['dev_name', 'dev_product_name', 'dev_model_name']) {
    if (typeof printer[key] === 'string' && printer[key]) return printer[key];
  }
  return '未命名';
}

function onlineText(printer) {
  if (printer.dev_online === true) return '在线';
  if (printer.dev_online === false) return '离线';
  return '未提供';
}

function rowsOf(model) {
  const printers = model.printers || [];
  const rows = printers.map((printer, index) => ({
    id: printer.dev_id,
    no: `No.${String(index + 1).padStart(3, '0')}`,
    name: printerName(printer),
    serial: printer.dev_id,
    status: stateLabel(printer.task_status) || onlineText(printer),
    offline: printer.dev_online === false,
  }));
  if (model.selectedId && !rows.some((row) => row.id === model.selectedId)) {
    rows.push({
      id: model.selectedId,
      no: `No.${String(rows.length + 1).padStart(3, '0')}`,
      name: '未命名',
      serial: model.selectedId,
      status: '未在账号列表',
      offline: false,
    });
  }
  return rows;
}

function listEmpty(model, rows) {
  if (rows.length) return '';
  if (model.printers == null) return model.status?.logged_in ? '正在读取打印机。' : '';
  if (model.status?.logged_in === false) return '登录之后，这个账号绑定的打印机会列在这里。';
  if (model.status?.logged_in === true) return '这个账号下还没有绑定打印机。';
  return '';
}

function selectedName(model) {
  const printer = (model.printers || []).find((item) => item.dev_id === model.selectedId);
  if (printer) return printerName(printer);
  return model.selectedId ? '未命名' : '';
}

function factItems(model) {
  const now = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  const items = [`${now.getFullYear()}.${pad(now.getMonth() + 1)}.${pad(now.getDate())}`];
  if (model.printers) items.push(`${model.printers.length} 台`);
  if (model.status?.country) items.push(model.status.country);
  if (model.status?.version) items.push(model.status.version);
  return items;
}

function targetLabel(name, target) {
  return target ? `目标 ${target}` : name;
}

function clip(text, max) {
  if (!text || text.length <= max) return text || '';
  return `${text.slice(0, max - 1)}…`;
}

function clock(time) {
  const date = new Date(time);
  const pad = (n) => String(n).padStart(2, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function fillLedger(node, rows) {
  node.replaceChildren();
  for (const [label, value, warn] of rows) {
    const item = el('li');
    item.append(el('span', null, label), el('span', warn ? 'ef-field warn' : 'ef-field', value));
    node.append(item);
  }
  node.hidden = rows.length === 0;
}

function dialSpec(job) {
  const percent = job?.percent;
  const remain = job?.remainParts;
  const nozzleGauge = heatGauge(job?.nozzleValue, job?.nozzleTargetValue);
  const bedGauge = heatGauge(job?.bedValue, job?.bedTargetValue);
  const gauges = [];
  if (percent) gauges.push({ label: '进度', share: Math.round(percent.bar * 1000) / 1000, alert: false });
  if (nozzleGauge) gauges.push({ label: '喷嘴', share: Math.round(nozzleGauge.share * 1000) / 1000, alert: nozzleGauge.alert });
  if (bedGauge) gauges.push({ label: '热床', share: Math.round(bedGauge.share * 1000) / 1000, alert: bedGauge.alert });
  const file = clip(job?.file, 18);
  return {
    idle: {
      label: job?.stateLabel || '进度',
      big: percent ? String(Math.round(percent.value)) : '—',
      unit: percent ? '%' : '',
      sub: file || '还没有上报',
    },
    gauges,
    sections: [
      {
        zh: '进度', en: 'Progress', href: '#job', here: true, alert: Boolean(job?.error),
        big: percent ? String(Math.round(percent.value)) : '—',
        unit: percent ? '%' : '',
        sub: file,
      },
      {
        zh: '喷嘴', en: 'Nozzle', href: '#job', alert: Boolean(nozzleGauge?.alert),
        big: job?.nozzleValue != null ? String(Math.round(job.nozzleValue)) : '—',
        unit: job?.nozzleValue != null ? '°C' : '',
        sub: job?.nozzleTarget ? `目标 ${job.nozzleTarget}` : '',
      },
      {
        zh: '热床', en: 'Bed', href: '#job', alert: Boolean(bedGauge?.alert),
        big: job?.bedValue != null ? String(Math.round(job.bedValue)) : '—',
        unit: job?.bedValue != null ? '°C' : '',
        sub: job?.bedTarget ? `目标 ${job.bedTarget}` : '',
      },
      {
        zh: '剩余', en: 'Left', href: '#job',
        big: remain ? remain.big : '—',
        unit: remain?.unit || '',
        sub: remain?.sub || '',
      },
    ],
  };
}

const ACTIONS = [
  ['login', '登录账号', true],
  ['refresh', '刷新状态', false],
  ['pause', '暂停', false],
  ['resume', '继续', false],
  ['stop', '停止', false],
];

function actionButtons() {
  return ACTIONS.map(([name, label]) => `<button class="ef-btn" data-act="${name}" type="button">${label}</button>`).join('');
}

export function createView(root, handlers) {
  root.innerHTML = `
    <header class="ef-head"><h1 id="title">当前打印 <small id="title-en">Job</small></h1></header>
    <ul class="ef-facts ef-field" id="facts"></ul>
    <p class="ef-notice" id="notice" data-flag="注意" role="status" hidden></p>
    <p id="login-link" hidden><a id="login-anchor" class="ef-btn small" target="_blank" rel="noopener noreferrer">打开登录页</a></p>
    <div class="ef-seg" id="source" role="radiogroup" aria-label="通道">
      <label><input type="radio" name="source" value="cloud" /><span>云端</span></label>
      <label><input type="radio" name="source" value="lan" /><span>局域网</span></label>
    </div>
    <div id="view-job">
      <div class="board">
        <div class="orbit">
          <figure class="hub"><svg class="ef-dial" id="dial" role="group" aria-label="打印读数"></svg></figure>
          <section class="ef-panel cut-br l1">
            <h2><span class="ef-pair"><span class="ef-eyebrow">Heat</span><span>温度</span></span></h2>
            <div class="ef-figs">
              <div><b id="nozzle">—</b><span id="nozzle-label">喷嘴</span></div>
              <div><b id="bed">—</b><span id="bed-label">热床</span></div>
              <div><b id="chamber">—</b><span>腔温</span></div>
            </div>
            <p class="legend" id="heat-legend"><i class="key"></i><span>喷嘴</span><i class="key dash"></i><span>热床</span></p>
            <p class="ef-empty" id="heat-empty">再收到两次温度之后，这里会画出喷嘴和热床。</p>
            <svg class="trace-chart" id="heat-chart" role="img" hidden></svg>
          </section>
          <section class="ef-panel cut-bl tab-right r1">
            <h2>
              <span class="ef-pair"><span class="ef-eyebrow">Progress</span><span>进度</span></span>
              <span class="badges" id="badges"></span>
            </h2>
            <ul class="ef-ledger" id="progress" hidden></ul>
            <p class="ef-field" id="bars-caption" hidden></p>
            <p class="ef-empty" id="bars-empty">进度上报之后，最近几次会按时间排成柱。</p>
            <ol class="ef-bars chart-bars" id="bars" hidden></ol>
          </section>
          <section class="ef-panel lead-right l2">
            <h2><span class="ef-pair"><span class="ef-eyebrow">Task</span><span>任务</span></span></h2>
            <p class="task-file" id="file">刷新之后，正在打印的文件名会显示在这里。</p>
            <p class="ef-actions">${actionButtons()}</p>
          </section>
          <section class="ef-panel lead-left tab-right r2">
            <h2><span class="ef-pair"><span class="ef-eyebrow">Printer</span><span>这台机器</span></span> <a href="#printers">全部</a></h2>
            <ul class="ef-ledger" id="machine" hidden></ul>
            <p class="ef-empty" id="machine-empty">还没有选定打印机。</p>
          </section>
          <section class="ef-panel cut-tr l3">
            <h2><span class="ef-pair"><span class="ef-eyebrow">Readings</span><span>读数</span></span></h2>
            <ul class="ef-ledger" id="ledger" hidden></ul>
            <p class="ef-empty" id="ledger-empty">还没有这台机器的状态。刷新之后，喷嘴、热床和进度会显示在这里。</p>
          </section>
          <section class="ef-panel cut-tl tab-right r3">
            <h2><span class="ef-pair"><span class="ef-eyebrow">Reports</span><span>回报</span></span></h2>
            <ul class="ef-ledger" id="log" hidden></ul>
            <p class="ef-empty" id="log-empty">连接之后，登录、设备和通道的回报会按时间列在这里。</p>
          </section>
        </div>
      </div>
    </div>
    <div class="cols" id="view-printers" hidden>
      <section class="ef-panel" id="fleet">
        <h2><span class="ef-pair"><span class="ef-eyebrow">Printers</span><span>打印机</span></span></h2>
        <ol class="ef-records" id="list"></ol>
        <p class="ef-empty" id="list-empty" hidden></p>
        <p class="ef-actions">${actionButtons()}</p>
      </section>
      <section class="ef-panel" id="view-lan">
        <h2><span class="ef-pair"><span class="ef-eyebrow">Lan</span><span>局域网</span></span></h2>
        <form class="ef-form form-grid" id="lan-form" novalidate>
          <label class="field"><span>序列号</span><input id="lan-id" name="dev_id" maxlength="64" autocomplete="off" spellcheck="false" /></label>
          <label class="field"><span>地址</span><input id="lan-ip" name="dev_ip" maxlength="15" inputmode="decimal" autocomplete="off" spellcheck="false" /></label>
          <label class="field"><span>访问码</span><input id="lan-pass" name="password" type="password" maxlength="128" autocomplete="off" /><small>访问码只留在这个页面里，刷新或关闭后需要重填。</small></label>
          <p class="ef-field" id="lan-state"></p>
          <p class="ef-actions">
            <button class="ef-btn go" id="lan-connect" type="submit">连接</button>
            <button class="ef-btn" id="lan-off" type="button">断开</button>
          </p>
        </form>
      </section>
    </div>
    <div class="cols" id="view-camera" hidden>
      <section class="ef-panel">
        <h2><span class="ef-pair"><span class="ef-eyebrow">Camera</span><span>摄像头</span></span></h2>
        <div class="frame">
          <p class="ef-empty" id="shot-empty">选定一台打印机之后，这里会显示它的摄像头画面。</p>
          <img id="shot" alt="打印机摄像头的一帧" hidden />
          <video id="live" muted autoplay playsinline hidden></video>
        </div>
      </section>
      <section class="ef-panel">
        <h2><span class="ef-pair"><span class="ef-eyebrow">Feed</span><span>画面控制</span></span></h2>
        <p class="ef-actions">
          <button class="ef-btn go" id="snap" type="button">拍一张</button>
          <button class="ef-btn" id="live-start" type="button">开始直播</button>
          <button class="ef-btn" id="live-stop" type="button" hidden>停止直播</button>
        </p>
        <div class="ef-seg" id="proto" role="radiogroup" aria-label="局域网画面协议">
          <label><input type="radio" name="proto" value="local" checked /><span>本地</span></label>
          <label><input type="radio" name="proto" value="rtsps" /><span>RTSPS</span></label>
          <label><input type="radio" name="proto" value="rtsp" /><span>RTSP</span></label>
        </div>
        <p class="ef-field" id="camera-note"></p>
      </section>
    </div>
  `;

  const q = (id) => root.querySelector(`#${id}`);
  const list = q('list');
  let listSig = '';
  let dialSig = '';

  q('source').addEventListener('change', (event) => {
    if (event.target.name === 'source') handlers.onSource(event.target.value);
  });
  list.addEventListener('click', (event) => {
    const row = event.target.closest('a[data-id]');
    if (!row) return;
    event.preventDefault();
    handlers.onSelect(row.dataset.id);
  });
  root.addEventListener('click', async (event) => {
    const act = event.target.closest('[data-act]');
    if (!act || act.disabled) return;
    const name = act.dataset.act;
    if (name === 'login') handlers.onLogin();
    else if (name === 'refresh') handlers.onRefresh();
    else if (name === 'pause') handlers.onPause();
    else if (name === 'resume') handlers.onResume();
    else if (name === 'stop') {
      const dialog = document.getElementById('stop-dialog');
      document.getElementById('stop-copy').textContent = handlers.stopCopy();
      dialog.returnValue = '';
      dialog.showModal();
      const value = await new Promise((resolve) => {
        dialog.addEventListener('close', () => resolve(dialog.returnValue), { once: true });
      });
      if (value === 'stop') handlers.onStop();
    }
  });
  q('lan-form').addEventListener('submit', (event) => {
    event.preventDefault();
    handlers.onLan(readLan());
  });
  q('lan-off').addEventListener('click', () => handlers.onLanOff());
  q('snap').addEventListener('click', () => handlers.onSnap());
  q('live-start').addEventListener('click', () => handlers.onLive());
  q('live-stop').addEventListener('click', () => handlers.onLiveStop());

  function readLan() {
    return {
      dev_id: q('lan-id').value.trim(),
      dev_ip: q('lan-ip').value.trim(),
      password: q('lan-pass').value,
    };
  }

  function readProto() {
    const checked = root.querySelector('#proto input:checked');
    return checked && (checked.value === 'rtsp' || checked.value === 'rtsps') ? checked.value : 'local';
  }

  function button(id) {
    const nodes = [...root.querySelectorAll(`[data-act="${id}"]`)];
    if (!nodes.length) return root.querySelector(`#${CSS.escape(id)}`);
    return nodes.find((node) => node.getClientRects().length > 0) || nodes[0];
  }

  function paintList(model) {
    const rows = rowsOf(model);
    const sig = rows.map((row) => `${row.id}\t${row.no}\t${row.name}\t${row.status}\t${row.offline}`).join('\n');
    if (sig !== listSig) {
      listSig = sig;
      list.replaceChildren();
      for (const row of rows) {
        const item = el('li');
        const link = el('a', 'ef-record');
        link.href = '#job';
        link.dataset.id = row.id;
        link.append(el('span', 'ef-tag', row.no), el('span', 'ef-field', row.serial), el('span', 'name ef-read', row.name), el('span', 'ef-field', row.status));
        item.append(link);
        list.append(item);
      }
    }
    for (const link of list.querySelectorAll('a')) {
      const on = link.dataset.id === model.selectedId;
      if (on) link.setAttribute('aria-current', 'true');
      else link.removeAttribute('aria-current');
      link.querySelector('.ef-tag').classList.toggle('is-current', on);
    }
    const empty = listEmpty(model, rows);
    q('list-empty').hidden = !empty;
    q('list-empty').textContent = empty;
    list.hidden = rows.length === 0;
  }

  function paintReadings(model, job, report) {
    const rows = [];
    if (job?.wifi) rows.push(['无线', job.wifi, false]);
    if (job?.humidity) rows.push(['湿度', job.humidity, false]);
    if (job?.error) rows.push(['错误码', job.error, true]);
    if (model.selectedId) {
      const via = model.source === 'lan' ? '局域网' : '云端';
      if (!report) rows.push(['通道', `${via}，还没有上报`, false]);
      else rows.push(['通道', report.confirmed ? via : `${via}，未确认`, !report.confirmed]);
      const ready = Array.isArray(model.status?.mqtt_ready) && model.status.mqtt_ready.includes(model.selectedId);
      if (model.source === 'cloud' && model.status) rows.push(['云端回报', ready ? '已就绪' : '未就绪', !ready && model.status.logged_in === true]);
    }
    if (job?.trays.length) {
      const item = el('li');
      const value = el('span', 'ef-field');
      job.trays.forEach((tray, index) => {
        const piece = el('span', 'tray');
        if (index) piece.prepend(' ');
        if (tray.color) {
          const swatch = el('i', 'swatch');
          swatch.style.background = tray.color;
          swatch.setAttribute('aria-hidden', 'true');
          piece.append(swatch);
        }
        const bits = [];
        if (tray.type) bits.push(tray.type);
        if (tray.remain != null) bits.push(`${tray.remain}%`);
        piece.append(bits.join(' ') || '未命名耗材');
        value.append(piece);
      });
      item.append(el('span', null, '耗材'), value);
      rows.push(item);
    }
    const ledger = q('ledger');
    fillLedger(ledger, rows.filter((row) => Array.isArray(row)));
    for (const item of rows) {
      if (item instanceof HTMLElement) {
        ledger.append(item);
        ledger.hidden = false;
      }
    }
    q('ledger-empty').hidden = rows.length > 0;
  }

  function paintMachine(model) {
    if (!model.selectedId) {
      fillLedger(q('machine'), []);
      q('machine-empty').hidden = false;
      return;
    }
    const printer = (model.printers || []).find((item) => item.dev_id === model.selectedId);
    fillLedger(q('machine'), [
      ['名称', selectedName(model), false],
      ['序列号', model.selectedId, false],
      ['状态', printer ? (stateLabel(printer.task_status) || onlineText(printer)) : '未在账号列表', printer?.dev_online === false],
    ]);
    q('machine-empty').hidden = true;
  }

  function paintChart(points, stale, alert = {}) {
    const temps = points.filter((point) => point.nozzle != null || point.bed != null);
    const count = temps.reduce((sum, point) => sum + (point.nozzle != null) + (point.bed != null), 0);
    const chart = q('heat-chart');
    const empty = count < 2;
    q('heat-empty').hidden = !empty;
    q('heat-legend').hidden = empty;
    if (empty) chart.setAttribute('hidden', '');
    else chart.removeAttribute('hidden');
    chart.classList.toggle('is-stale', stale && !empty);
    chart.replaceChildren();
    if (empty) {
      chart.removeAttribute('aria-label');
      return;
    }
    const nozzle = temps.filter((point) => point.nozzle != null).map((point) => ({ t: point.t, value: point.nozzle }));
    const bed = temps.filter((point) => point.bed != null).map((point) => ({ t: point.t, value: point.bed }));
    const values = [...nozzle, ...bed].map((point) => point.value);
    let min = Math.min(...values);
    let max = Math.max(...values);
    if (min === max) {
      min -= 5;
      max += 5;
    } else {
      const pad = (max - min) * 0.12;
      min -= pad;
      max += pad;
    }
    const times = temps.map((point) => point.t);
    const x0 = Math.min(...times);
    const x1 = Math.max(...times);
    const W = 320;
    const H = 120;
    const L = 36;
    const R = 8;
    const T = 12;
    const B = 8;
    const X = (t) => L + ((t - x0) / (x1 - x0 || 1)) * (W - L - R);
    const Y = (value) => T + (1 - (value - min) / (max - min)) * (H - T - B);
    chart.setAttribute('viewBox', `0 0 ${W} ${H}`);
    chart.append(svg('text', { class: 'axis', x: 0, y: 10 }, `${Math.round(max)}°`));
    chart.append(svg('text', { class: 'axis', x: 0, y: H - 2 }, `${Math.round(min)}°`));
    const line = (series, cls, mark) => {
      if (series.length >= 2) {
        const d = series.map((point, index) => `${index ? 'L' : 'M'}${X(point.t).toFixed(1)} ${Y(point.value).toFixed(1)}`).join('');
        chart.append(svg('path', { class: `line ${cls}`, d }));
      }
      const last = series[series.length - 1];
      chart.append(svg('rect', {
        class: `dot ${cls}${mark ? ' is-alert' : ''}`,
        x: (X(last.t) - 2.5).toFixed(1),
        y: (Y(last.value) - 2.5).toFixed(1),
        width: 5,
        height: 5,
      }));
    };
    if (nozzle.length) line(nozzle, 'nozzle', alert.nozzle);
    if (bed.length) line(bed, 'bed', alert.bed);
    const latestNozzle = nozzle.length ? `${Math.round(nozzle[nozzle.length - 1].value)}°C` : '没有';
    const latestBed = bed.length ? `${Math.round(bed[bed.length - 1].value)}°C` : '没有';
    chart.setAttribute('aria-label', `喷嘴 ${latestNozzle}，热床 ${latestBed}，共 ${temps.length} 次${stale ? '，未确认' : ''}`);
  }

  function paintBars(points, stale) {
    const rows = points.filter((point) => point.percent != null).slice(-12);
    const list = q('bars');
    const caption = q('bars-caption');
    q('bars-empty').hidden = rows.length > 0;
    list.hidden = rows.length === 0;
    caption.hidden = rows.length === 0;
    list.classList.toggle('is-stale', stale && rows.length > 0);
    list.replaceChildren();
    if (!rows.length) {
      list.removeAttribute('aria-label');
      return;
    }
    const first = rows[0];
    const last = rows[rows.length - 1];
    caption.textContent = `${clock(first.t)} – ${clock(last.t)}`;
    list.setAttribute('aria-label', `最近 ${rows.length} 次进度，从 ${clock(first.t)} 的 ${Math.round(first.percent)}% 到 ${clock(last.t)} 的 ${Math.round(last.percent)}%${stale ? '，未确认' : ''}`);
    rows.forEach((row, index) => {
      const item = document.createElement('li');
      item.style.setProperty('--v', String(Math.min(1, Math.max(0, row.percent / 100))));
      if (index === rows.length - 1) {
        item.className = 'is-here';
        item.append(el('span', null, `${Math.round(row.percent)}%`));
      }
      list.append(item);
    });
  }

  function paint(model) {
    const titles = {
      job: ['当前打印', 'Job'],
      printers: ['设备', 'Printers'],
      camera: ['画面', 'Camera'],
    };
    const [zh, en] = titles[model.view] || titles.job;
    q('title').firstChild.textContent = `${zh} `;
    q('title-en').textContent = en;
    q('facts').replaceChildren(...factItems(model).map((text) => el('li', null, text)));

    const note = noticeText(model);
    q('notice').hidden = !note;
    if (note) {
      q('notice').dataset.flag = note.flag;
      q('notice').textContent = note.text;
    }
    q('login-link').hidden = !model.loginHref || model.status?.logged_in === true;
    if (model.loginHref) q('login-anchor').href = model.loginHref;

    for (const input of q('source').querySelectorAll('input')) input.checked = input.value === model.source;
    for (const link of document.querySelectorAll('[data-view]')) {
      if (link.dataset.view === model.view) link.setAttribute('aria-current', 'page');
      else link.removeAttribute('aria-current');
    }
    const offline = (model.printers || []).filter((printer) => printer.dev_online === false).length;
    const badge = document.getElementById('need-count');
    badge.hidden = offline === 0;
    badge.textContent = String(offline);
    if (offline) badge.setAttribute('aria-label', `${offline} 台离线`);
    const linked = linkState(model);
    const chip = document.getElementById('link-state');
    chip.textContent = linked.text;
    chip.className = linked.tone ? `ef-state ${linked.tone}` : 'ef-state';

    const report = currentReport(model);
    const job = displayJob(report?.data);
    const shown = jobHasReadings(job) ? job : null;
    const points = model.traces?.[model.selectedId] || [];
    const stale = Boolean(report && !report.confirmed);
    const spec = dialSpec(shown);
    const nextSig = JSON.stringify(spec);
    if (nextSig !== dialSig) {
      dialSig = nextSig;
      drawDial(q('dial'), spec);
    }

    const setFig = (id, text) => {
      const node = q(id);
      node.textContent = text || '—';
      node.setAttribute('aria-label', text || '还没有上报');
    };
    setFig('nozzle', shown?.nozzle);
    setFig('bed', shown?.bed);
    setFig('chamber', shown?.chamber);
    q('nozzle-label').textContent = targetLabel('喷嘴', shown?.nozzleTarget);
    q('bed-label').textContent = targetLabel('热床', shown?.bedTarget);
    q('file').textContent = shown?.file || '刷新之后，正在打印的文件名会显示在这里。';
    const badges = q('badges');
    badges.replaceChildren();
    if (shown?.stateLabel) badges.append(el('span', shown.tone ? `ef-state ${shown.tone}` : 'ef-state', shown.stateLabel));
    if (stale) badges.append(el('span', 'ef-state is-draft', '未确认'));
    fillLedger(q('progress'), [
      shown?.remain ? ['剩余', shown.remain, false] : null,
      shown?.layer ? ['层', shown.layer, false] : null,
      shown?.speed ? ['速度', shown.speed, false] : null,
    ].filter(Boolean));
    paintChart(points, stale, {
      nozzle: Boolean(heatGauge(shown?.nozzleValue, shown?.nozzleTargetValue)?.alert),
      bed: Boolean(heatGauge(shown?.bedValue, shown?.bedTargetValue)?.alert),
    });
    paintBars(points, stale);
    paintReadings(model, shown, report);
    paintMachine(model);
    paintList(model);

    const loggedIn = model.status?.logged_in === true;
    const loginPrimary = !loggedIn && model.source === 'cloud';
    const can = canCommand(model);
    const gates = actionGates(report);
    const busy = model.pending != null;
    const setAct = (name, apply) => {
      for (const node of root.querySelectorAll(`[data-act="${name}"]`)) apply(node);
    };
    setAct('login', (node) => {
      node.hidden = loggedIn;
      node.classList.toggle('go', loginPrimary);
      node.disabled = busy;
    });
    setAct('refresh', (node) => {
      node.classList.toggle('go', !loginPrimary);
      node.disabled = !can || busy;
    });
    setAct('pause', (node) => { node.disabled = !can || !gates.pause || busy; });
    setAct('resume', (node) => { node.disabled = !can || !gates.resume || busy; });
    setAct('stop', (node) => { node.disabled = !can || !gates.stop || busy; });
    q('lan-connect').disabled = busy;
    q('lan-off').disabled = busy || model.lanLink?.status !== 0;
    q('lan-state').textContent = model.lanPending
      ? `${model.lanPending} 等待连接回报`
      : model.lanLink
        ? (model.lanLink.status === 0 ? `${model.lanLink.devId} 已连接` : `${model.lanLink.devId} 连接失败，状态 ${model.lanLink.status}`)
        : '';

    q('view-job').hidden = model.view !== 'job';
    q('view-printers').hidden = model.view !== 'printers';
    q('view-camera').hidden = model.view !== 'camera';
    q('proto').hidden = model.source !== 'lan';
    q('camera-note').textContent = model.source === 'lan'
      ? '局域网画面使用设备页里这台机器的地址和访问码。访问码不会被保存。本地协议走打印机 6000 端口。'
      : '云端画面使用当前选中的序列号。';
    const showShot = Boolean(model.shotUrl) && !model.live;
    q('shot').hidden = !showShot;
    q('live').hidden = !model.live;
    q('shot-empty').hidden = showShot || model.live;
    if (model.shotUrl && q('shot').dataset.url !== model.shotUrl) {
      q('shot').dataset.url = model.shotUrl;
      q('shot').src = model.shotUrl;
    }
    q('snap').disabled = !model.selectedId || busy || model.live;
    q('live-start').hidden = model.live;
    q('live-start').disabled = !model.selectedId || busy;
    q('live-stop').hidden = !model.live;

    const log = q('log');
    const events = model.events.map((event) => [event.text, event.time, false]);
    fillLedger(log, events);
    q('log-empty').hidden = events.length > 0;
  }

  return { paint, readLan, readProto, button };
}

export function stopCopy(model) {
  const name = selectedName(model);
  return name && model.selectedId
    ? `将停止「${name}」上的当前任务。已经打出来的部分会留在热床上。`
    : '将停止当前任务。已经打出来的部分会留在热床上。';
}
