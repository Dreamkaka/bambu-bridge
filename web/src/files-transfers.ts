import { ApiError, downloadBlob, errorText, postJson } from './api';
import { esc } from './html';
import { byteText } from './logic';
import { base, category, measured, payload, volume, type Context, type Entry } from './files-model';

type Job = { file: Entry; state: 'waiting' | 'active' | 'done' | 'failed' | 'skipped'; error?: string };
const states = { waiting: '等待', active: '处理中', done: '完成', failed: '失败 / 未确认', skipped: '未执行' };
export interface FileTransfers {
  readonly busy: boolean;
  readonly removing: boolean;
  readonly deleteUnavailable: boolean;
  readonly deletedHere: boolean;
  download(files: Entry[]): void;
  confirm(files: Entry[], printer: string): void;
  dispose(): void;
}

export function transfers(bar: HTMLElement, modal: HTMLDialogElement, ctx: Context, current: () => boolean,
  updateControls: () => void, afterDelete: () => void): FileTransfers {
  const abort = new AbortController();
  let jobs: Job[] = [], targets: Entry[] = [];
  let action: 'download' | 'delete' = 'download';
  let running = false, deleteUnavailable = false, deletedHere = false;
  let received = 0, total: number | undefined, start = 0, lastPaint = 0;
  let expanded = false;
  const valid = () => !abort.signal.aborted && current();
  function paint() {
    bar.hidden = !jobs.length;
    if (!jobs.length) { bar.replaceChildren(); return; }
    const i = jobs.findIndex(j => j.state === 'active');
    const j = jobs[i], done = jobs.filter(j => j.state === 'done').length;
    const failed = jobs.filter(j => j.state === 'failed').length;
    const verb = action === 'download' ? '下载' : '删除';
    const bytes = j && action === 'download';
    const pct = bytes && total && received <= total ? Math.floor(100 * received / total) : undefined;
    const rate = received / Math.max(0.001, (performance.now() - start) / 1000);
    const title = j ? `${verb} ${i + 1}/${jobs.length}：${j.file.name}` : `${verb}完成 ${done}/${jobs.length}${failed ? ` / ${failed} 项未成功` : ''}${action === 'download' && done ? ' / 已交给浏览器保存' : ''}`;
    bar.innerHTML = `<div class="files-qline"><span class="files-kicker">// TRANSFER QUEUE</span><strong title="${esc(title)}">${esc(title)}</strong>
      ${bytes ? `<progress aria-label="当前文件下载进度" ${pct !== undefined ? `value="${pct}" max="100"` : ''}></progress><small>${pct !== undefined ? `${pct}% / ` : ''}${byteText(received)}${total !== undefined ? ` / ${byteText(total)}` : ''}${received ? ` / ${byteText(rate)}/s` : ''}</small>` : ''}
      <button class="ef-chip" data-queue="details" aria-expanded="${expanded}">${expanded ? '收起' : '详情'}</button>
      ${!running && failed && action === 'download' ? '<button class="ef-btn small" data-queue="retry">重试失败项</button>' : ''}
      ${!running ? '<button class="ef-chip" data-queue="clear">关闭</button>' : ''}</div>
      ${expanded ? `<ol class="files-joblist">${jobs.map(item => `<li>${esc(item.file.path || item.file.name)} <b>${states[item.state]}</b>${item.error ? `：${esc(item.error)}` : ''}</li>`).join('')}</ol>` : ''}`;
  }
  function paintModal() {
    modal.innerHTML = `<div class="files-warning-band"></div><div class="files-confirm-inner"><p class="files-kicker">// CONFIRM DELETE</p><h2 id="files-purge-title">${running ? '正在删除存储文件' : '永久删除这些文件？'}</h2>
      <p>打印机 <strong>${esc(deviceName)}</strong> / ${esc(ctx.devId)}<br>${ctx.source === 'lan' ? '局域网' : '云端'} / ${volume[ctx.storage]} / ${targets.length} 个文件 / 已统计 ${measured(targets)}</p>
      <ol class="files-joblist">${targets.map((f, i) => `<li><strong>${esc(f.name)}</strong><small>${esc(f.path || '路径未上报，按名称匹配')} / ${category[f.kind]}${running ? ` / ${states[jobs[i].state]} ${esc(jobs[i].error || '')}` : ''}</small></li>`).join('')}</ol>
      ${running ? `<p role="status">正在处理 ${jobs.findIndex(j => j.state === 'active') + 1} / ${jobs.length}</p>` : '<p>此操作无法撤销，打印机没有回收站。</p><div class="files-confirm-actions"><button class="ef-btn small" data-purge="cancel" autofocus>取消</button><button class="ef-btn small files-destructive" data-purge="confirm">确认永久删除</button></div>'}</div>`;
  }
  let deviceName = '';
  async function run() {
    if (running || !valid() || !jobs.length) return;
    running = true;
    updateControls();
    for (const j of jobs) {
      if (!valid()) return;
      if (j.state !== 'waiting') continue;
      j.state = 'active'; received = 0; total = j.file.size; start = performance.now(); lastPaint = start;
      paint(); if (action === 'delete') paintModal();
      try {
        const body = payload(ctx, j.file.kind, j.file);
        if (action === 'download') {
          await downloadBlob(`${base(ctx)}/download`, body, j.file.name, { signal: abort.signal, canSave: valid,
            onProgress(n, responseLength) {
              if (!valid()) return;
              received = n; total = responseLength ?? j.file.size;
              if (performance.now() - lastPaint >= 160) { lastPaint = performance.now(); paint(); }
            },
          });
        } else {
          const res = await postJson<{ deleted: boolean }>(`${base(ctx)}/delete`, body);
          if (res.status !== 200 || res.result?.deleted !== true) throw new ApiError(res.status, 'file_delete_unconfirmed');
        }
        if (!valid()) return;
        j.state = 'done';
        if (action === 'delete') deletedHere = true;
      } catch (e) {
        if (!valid()) return;
        j.state = 'failed';
        j.error = e instanceof ApiError && e.code === 'file_delete_unconfirmed' ? '打印机未确认删除。' : errorText(e);
        if (action === 'delete') {
          if (e instanceof ApiError && ['not_found', 'method_not_allowed'].includes(e.code)) deleteUnavailable = true;
          for (const later of jobs) if (later.state === 'waiting') later.state = 'skipped';
          j.error += ' 已停止后续删除，请核对重新读取的列表。';
          break;
        }
      }
    }
    if (!valid()) return;
    running = false;
    expanded = jobs.some(j => j.state === 'failed');
    paint();
    if (action === 'delete') {
      modal.close(); targets = []; afterDelete();
    } else updateControls();
  }
  function cancel() { if (!running) { targets = []; if (modal.open) modal.close(); } }
  modal.addEventListener('cancel', e => { e.preventDefault(); cancel(); }, { signal: abort.signal });
  modal.addEventListener('click', e => {
    const btn = e.target instanceof Element ? e.target.closest<HTMLElement>('[data-purge]') : null;
    if (btn?.dataset.purge === 'cancel') cancel();
    if (btn?.dataset.purge !== 'confirm' || !modal.open || !valid() || running || !targets.length || deleteUnavailable) return;
    action = 'delete'; jobs = targets.map(file => ({ file, state: 'waiting' })); expanded = false;
    void run();
  }, { signal: abort.signal });
  bar.addEventListener('click', e => {
    const op = e.target instanceof Element ? e.target.closest<HTMLElement>('[data-queue]')?.dataset.queue : undefined;
    if (!op || !valid()) return;
    if (op === 'details') { expanded = !expanded; paint(); }
    if (op === 'clear' && !running) { jobs = []; paint(); }
    if (op === 'retry' && !running && action === 'download') {
      for (const j of jobs) if (j.state === 'failed') { j.state = 'waiting'; delete j.error; }
      void run();
    }
  }, { signal: abort.signal });
  return {
    get busy() { return running; }, get removing() { return running && action === 'delete'; },
    get deleteUnavailable() { return deleteUnavailable; },
    get deletedHere() { return deletedHere; },
    download(files: Entry[]) {
      if (!valid() || modal.open || (running && action !== 'download')) return;
      if (!running) { jobs = []; expanded = false; }
      action = 'download';
      const queued = new Set(jobs.map(j => j.file.path || `${j.file.kind}:${j.file.name}`));
      for (const f of files) {
        const key = f.path || `${f.kind}:${f.name}`;
        if (!queued.has(key)) { queued.add(key); jobs.push({ file: f, state: 'waiting' }); }
      }
      if (running) paint(); else void run();
    },
    confirm(files: Entry[], printer: string) {
      if (!valid() || modal.open || running || deleteUnavailable || !files.length) return;
      targets = files.slice(); deviceName = printer; paintModal(); modal.showModal();
    },
    dispose() { abort.abort(); if (modal.open) modal.close(); bar.replaceChildren(); bar.hidden = true; },
  };
}
