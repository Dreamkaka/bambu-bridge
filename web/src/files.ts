import './files.css';
import { errorText, postJson } from './api';
import { esc } from './html';
import { byteText } from './logic';
import { store } from './store';
import { base, category, entries, kinds, measured, order, payload, volume, when, type Context, type Entry, type Filter, type Sort, type Storage } from './files-model';
import { inspector } from './files-inspector';
import { transfers, type FileTransfers } from './files-transfers';

export function mountFiles(root: HTMLElement): () => void {
  const life = new AbortController();
  root.innerHTML = `<div class="files-page"><header class="ef-head"><h1>文件 <small>Files</small></h1></header>
    <div class="files-context"><strong data-who></strong><b class="files-channel" data-channel></b><span data-measured></span><span class="files-perms" data-access></span></div>
    <div class="files-tools" aria-label="文件筛选与操作"><div class="files-segment" role="group" aria-label="存储介质">${(['internal','external'] as const).map(s => `<button type="button" class="ef-chip" data-storage="${s}">${volume[s]}</button>`).join('')}</div>
    <div class="files-segment" role="group" aria-label="文件分类">${(['all', ...kinds] as const).map(k => `<button type="button" class="ef-chip" data-kind="${k}">${category[k]}</button>`).join('')}</div>
    <select class="files-dir-select" aria-label="目录筛选"></select>
    <label class="files-search"><span aria-hidden="true">//</span><input type="search" placeholder="过滤文件名…" aria-label="过滤文件名"></label>
    <select data-sort aria-label="排序"><option value="new">最新优先</option><option value="old">最早优先</option><option value="large">大小降序</option><option value="name">名称 A–Z</option></select>
    <div data-batch hidden><button class="ef-chip" data-entry-act="download" data-bulk></button><button class="ef-chip files-danger" data-entry-act="delete" data-bulk></button></div>
    <button class="ef-btn small" data-refresh>↻ 重新读取</button></div>
    <p class="files-notice" role="status" hidden></p>
    <div class="files-work"><aside class="files-rail" aria-label="目录"><header class="files-region-head"><span class="files-kicker">// DIRECTORY</span><b>目录</b></header><nav data-dirs></nav></aside>
    <div class="files-matrix"><header class="files-region-head"><span class="files-kicker">// FILE INDEX</span><b data-index-count></b></header>
    <table class="files-table" aria-label="文件列表"><colgroup><col class="files-check-col"><col><col class="files-size-col"><col class="files-date-col"><col class="files-act-col"></colgroup>
    <thead><tr><th><label><input type="checkbox" data-all aria-label="全选当前筛选结果"></label></th><th>文件名称 <small>// NAME</small></th><th class="files-size-col">大小</th><th class="files-date-col">修改时间</th><th class="files-act-col">操作</th></tr></thead><tbody></tbody></table>
    <div class="files-empty" data-empty></div><footer class="files-pager" hidden><button class="ef-chip" data-page="-1">上一页</button><span></span><button class="ef-chip" data-page="1">下一页</button></footer></div>
    <aside class="files-dock" aria-label="文件详情"></aside></div>
    <div class="files-queue" aria-label="传输队列" hidden></div>
    <dialog class="files-drawer" aria-label="文件详情"></dialog><dialog class="files-confirm" aria-labelledby="files-purge-title"></dialog></div>`;
  const find = <T extends HTMLElement = HTMLElement>(selector: string) => root.querySelector<T>(selector)!;
  const search = find<HTMLInputElement>('.files-search input'), dirs = find<HTMLSelectElement>('.files-dir-select');
  const checkAll = find<HTMLInputElement>('[data-all]'), refresh = find<HTMLButtonElement>('[data-refresh]');
  const body = find<HTMLTableSectionElement>('tbody'), pager = find('.files-pager');
  let storage: Storage = 'external', filter: Filter = 'model', sort: Sort = 'new';
  let context: Context, queue: FileTransfers, block = '', displayName = '';
  let listed: Entry[] = [], visible: Entry[] = [], counts = new Map<string, number>();
  const selected = new Set<Entry>();
  let anchor: Entry | null = null, dir = '', page = 0, loading = false, readOk = false, problem = '', seq = 0;
  const pageSize = 80;
  function blocked() {
    const p = store.prefs;
    if (!p.devId) return '先在命令栏或设置中选择打印机。';
    if (p.source === 'lan' && (!p.lanIp || !p.lanPassword)) return '局域网文件需要在设置中填写地址和访问码。';
    if (store.status && !store.status.agent_ready) return '桥接尚未就绪。';
    if (p.source === 'cloud' && store.status && !store.status.logged_in) return '请先在设置中登录云端账号。';
    return '';
  }
  function current(c: Context) {
    const p = store.prefs;
    return !life.signal.aborted && c === context && c.devId === p.devId && c.source === p.source && c.storage === storage && block === blocked() &&
      (c.source !== 'lan' || (c.lanIp === p.lanIp && c.lanPassword === p.lanPassword));
  }
  const detail = inspector(find('.files-dock'), find<HTMLDialogElement>('.files-drawer'), () => current(context), controls);
  function controls() {
    const busy = !!queue?.busy, noDelete = !!queue?.deleteUnavailable;
    const access = block ? '未连接' : noDelete ? 'RO // 桥接不支持删除' : queue?.deletedHere ? 'RW // 此范围已有删除确认' : readOk ? '读取已确认 / 删除由设备确认' : '权限待确认';
    find('[data-access]').textContent = access;
    refresh.disabled = loading || busy || !!block;
    refresh.textContent = loading ? '读取中…' : '↻ 重新读取';
    refresh.setAttribute('aria-busy', String(loading));
    root.querySelectorAll<HTMLButtonElement>('[data-storage], [data-kind]').forEach(btn => {
      btn.disabled = busy;
      btn.setAttribute('aria-pressed', String(btn.dataset.storage === storage || btn.dataset.kind === filter));
    });
    find('[data-batch]').hidden = selected.size === 0;
    root.querySelectorAll<HTMLButtonElement>('[data-entry-act]').forEach(btn => {
      const del = btn.dataset.entryAct === 'delete';
      const unavailable = loading || !!block || (del ? busy || noDelete : !!queue?.removing);
      if (btn.hasAttribute('data-bulk')) btn.textContent = `${del ? '批量删除' : '批量下载'} (${selected.size})`;
      btn.disabled = unavailable;
      btn.title = del ? noDelete ? '当前桥接未提供删除接口' : busy ? '等待传输完成' : '实际删除权限由设备逐次确认' : '';
    });
  }
  function listing() {
    const start = page * pageSize;
    body.innerHTML = visible.slice(start, start + pageSize).map((f, offset) => `<tr data-row="${start + offset}" ${selected.has(f) ? 'class="is-selected"' : ''}>
      <td><label><input type="checkbox" data-check aria-label="选择 ${esc(f.name)}" ${selected.has(f) ? 'checked' : ''}></label></td>
      <td><button type="button" class="files-name" data-open title="${esc(f.name)}"><span class="files-name-line"><b class="files-format" data-format="${esc(f.format)}">${esc(f.format)}</b><strong>${esc(f.name)}</strong></span><small>// ${esc(f.dir)}</small></button></td>
      <td class="files-size-col">${f.size === undefined ? '—' : byteText(f.size)}</td><td class="files-date-col" title="${esc(when(f))}">${esc(when(f))}</td>
      <td class="files-act-col"><button class="ef-chip" data-entry-act="download">↓ 下载</button> <button class="ef-chip files-danger" data-entry-act="delete">删除</button></td></tr>`).join('');
    checkAll.checked = visible.length > 0 && selected.size === visible.length;
    checkAll.indeterminate = selected.size > 0 && !checkAll.checked;
    checkAll.disabled = !visible.length;
    find('[data-index-count]').textContent = loading ? '读取中' : `${visible.length} 项`;
    const empty = find('[data-empty]'); empty.hidden = !!visible.length;
    empty.classList.toggle('is-scanning', loading);
    empty.innerHTML = `<span class="files-empty-icon" aria-hidden="true"></span><b class="files-kicker">${loading ? 'SCANNING DIRECTORY' : problem || block ? 'DIRECTORY UNAVAILABLE' : 'NO FILES FOUND'}</b><p>${loading ? '正在读取存储目录…' : block || problem ? '暂时无法列出这一范围的文件。' : listed.length ? '当前筛选条件下没有文件。' : '这个存储分类里还没有文件。'}</p>`;
    pager.hidden = visible.length <= pageSize;
    pager.querySelector('span')!.textContent = `${page + 1} / ${Math.max(1, Math.ceil(visible.length / pageSize))}`;
    find<HTMLButtonElement>('[data-page="-1"]').disabled = page === 0;
    find<HTMLButtonElement>('[data-page="1"]').disabled = start + pageSize >= visible.length;
    controls();
  }
  function applyFilters(reset: boolean) {
    const q = search.value.trim().toLocaleLowerCase();
    visible = listed.filter(f => (!dir || f.dir === dir) && (!q || f.name.toLocaleLowerCase().includes(q))).sort((a,b) => order(a,b,sort));
    if (reset) { page = 0; selected.clear(); anchor = null; if (detail.file && !visible.includes(detail.file)) detail.close(); }
    listing();
  }
  function renderDirs() {
    const items: [string, number][] = [['', listed.length], ...Array.from(counts).sort(([a],[b]) => a.localeCompare(b))];
    const leafCounts = new Map<string, number>();
    for (const [key] of items) { const leaf = key.split('/').pop() || key; leafCounts.set(leaf, (leafCounts.get(leaf) || 0) + 1); }
    dirs.innerHTML = items.map(([key,n]) => `<option value="${esc(key)}" ${key === dir ? 'selected' : ''}>${esc(key || '全部目录')} (${n})</option>`).join('');
    find('[data-dirs]').innerHTML = items.map(([key,n]) => {
      const leaf = key.split('/').pop() || key;
      return `<button type="button" data-dir="${esc(key)}" title="${esc(key || '全部目录')}" aria-pressed="${key === dir}"><i aria-hidden="true"></i><span>${esc(!key ? '全部目录' : leafCounts.get(leaf) === 1 ? leaf : key)}</span><b>${n}</b></button>`;
    }).join('');
  }
  function describe() {
    find('[data-who]').textContent = displayName || '未选择打印机';
    find('[data-channel]').textContent = context.source === 'lan' ? 'LAN / CTRL' : 'CLOUD / CTRL';
    find('[data-measured]').textContent = `${volume[storage]} / ${category[filter]}${readOk ? ` / 已统计 ${measured(listed)} / ${listed.length} 个文件` : ' / 容量未上报'}`;
    const note = find('.files-notice'); note.textContent = block || problem; note.hidden = !note.textContent;
  }
  async function readList() {
    const generation = ++seq, c = context;
    const valid = () => seq === generation && current(c);
    listed = []; visible = []; counts.clear(); selected.clear(); anchor = null; page = 0; detail.close();
    readOk = false; problem = ''; loading = !block;
    describe(); renderDirs(); listing();
    if (block) return;
    const records = new Map<string, Entry>(), issues: string[] = [];
    let success = false;
    for (const k of filter === 'all' ? kinds : [filter] as const) {
      if (!valid()) return;
      try {
        const res = await postJson<unknown>(base(c), payload(c,k));
        if (!valid()) return;
        for (const f of entries(res.result, k)) {
          const key = f.path || `${f.kind}:${f.name}`;
          if (!records.has(key)) records.set(key,f);
        }
        success = true;
      } catch (err) { if (!valid()) return; issues.push(`${category[k]}：${errorText(err)}`); }
    }
    if (!valid()) return;
    listed = Array.from(records.values());
    for (const f of listed) counts.set(f.dir, (counts.get(f.dir) || 0) + 1);
    if (!counts.has(dir)) dir = '';
    loading = false; readOk = success; problem = issues.join(' / ');
    describe(); renderDirs(); applyFilters(false);
  }
  function sync(force = false) {
    const p = store.prefs, name = store.printers.find(item => item.dev_id === p.devId)?.dev_name || p.devId;
    if (!force && context && current(context)) {
      if (displayName !== name) { displayName = name; describe(); }
      return;
    }
    queue?.dispose();
    context = { devId: p.devId, source: p.source, storage, lanIp: p.source === 'lan' ? p.lanIp : '', lanPassword: p.source === 'lan' ? p.lanPassword : '' };
    const c = context; block = blocked(); displayName = name; dir = ''; search.value = '';
    queue = transfers(find('.files-queue'), find<HTMLDialogElement>('.files-confirm'), c, () => current(c), controls, () => { void readList(); });
    void readList();
  }
  root.addEventListener('click', e => {
    if (!(e.target instanceof Element) || !current(context)) return;
    const t = e.target, btn = t.closest<HTMLButtonElement>('button');
    if (btn?.disabled) return;
    const row = t.closest<HTMLElement>('[data-row]'), index = row ? Number(row.dataset.row) : -1;
    const f = visible[index];
    if (t === checkAll) {
      selected.clear(); if (checkAll.checked) visible.forEach(file => selected.add(file)); anchor = null; listing(); return;
    }
    if (t instanceof HTMLInputElement && t.hasAttribute('data-check') && f) {
      const from = e.shiftKey && anchor ? visible.indexOf(anchor) : -1;
      const range = from === -1 ? [f] : visible.slice(Math.min(from,index),Math.max(from,index) + 1);
      for (const item of range) { if (t.checked) selected.add(item); else selected.delete(item); }
      anchor = f; listing(); find<HTMLInputElement>(`[data-row="${index}"] [data-check]`).focus({preventScroll:true}); return;
    }
    if (t.closest('label')) return;
    if (btn?.dataset.storage && btn.dataset.storage !== storage && !queue.busy) { storage = btn.dataset.storage as Storage; sync(true); return; }
    if (btn?.dataset.kind && btn.dataset.kind !== filter && !queue.busy) { filter = btn.dataset.kind as Filter; sync(true); return; }
    if (btn?.hasAttribute('data-refresh') && !queue.busy && !loading) { void readList(); return; }
    if (btn?.dataset.dir !== undefined) { dir = btn.dataset.dir; renderDirs(); applyFilters(true); return; }
    if (btn?.dataset.page) { page += Number(btn.dataset.page); listing(); return; }
    const action = btn?.dataset.entryAct;
    if (action === 'delete' || action === 'download') {
      if (loading || block) return;
      const items = btn!.hasAttribute('data-bulk') ? Array.from(selected) : f ? [f] : detail.file ? [detail.file] : [];
      if (items.length) { if (action === 'delete') queue.confirm(items,displayName); else queue.download(items); }
      return;
    }
    if (f && (!btn || btn.hasAttribute('data-open'))) detail.open(f,context);
  }, {signal:life.signal});
  search.addEventListener('input', () => applyFilters(true), {signal:life.signal});
  dirs.addEventListener('change', () => { dir = dirs.value; renderDirs(); applyFilters(true); }, {signal:life.signal});
  find<HTMLSelectElement>('[data-sort]').addEventListener('change', e => {
    sort = (e.target as HTMLSelectElement).value as Sort; page = 0; anchor = null; applyFilters(false);
  }, {signal:life.signal});
  const unsub = store.subscribe(() => sync()); sync();
  return () => { life.abort(); unsub(); queue.dispose(); detail.dispose(); };
}
