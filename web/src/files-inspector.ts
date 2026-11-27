import { ApiError, errorText } from './api';
import { esc } from './html';
import { byteText } from './logic';
import { base, category, curl, payload, when, type Context, type Entry } from './files-model';

export function inspector(dock: HTMLElement, drawer: HTMLDialogElement, alive: () => boolean, updateControls: () => void) {
  const wide = matchMedia('(min-width: 1440px)');
  const box = document.createElement('div');
  box.className = 'files-detail';
  let file: Entry | null = null, ctx: Context | null = null;
  let preview: AbortController | null = null, image = '', message = '', copyMessage = '';
  function clearPreview() {
    preview?.abort(); preview = null; message = ''; copyMessage = '';
    if (image) URL.revokeObjectURL(image);
    image = '';
  }
  function place() {
    if (file && !wide.matches) {
      drawer.append(box);
      if (!drawer.open) drawer.showModal();
    } else {
      if (drawer.open) drawer.close();
      dock.append(box);
    }
  }
  function render() {
    const head = `<header class="files-region-head"><span class="files-kicker">// FILE INSPECTOR</span>${file ? '<button type="button" class="files-close" data-inspect-act="close" aria-label="关闭文件详情">×</button>' : ''}</header>`;
    if (!file || !ctx) {
      box.innerHTML = `${head}<div class="files-idle-detail"><span class="files-sheet" aria-hidden="true"></span><h3>文件详情</h3><p>选择一个文件，查看完整路径、预览及操作。</p></div>`;
    } else {
      const endpoint = esc(base(ctx));
      box.innerHTML = `${head}<div class="files-detail-body"><p class="files-kicker">${esc(category[file.kind])} / ${esc(file.format)}</p><h3>${esc(file.name)}</h3>
      <div class="files-preview">${image ? `<img alt="${esc(file.name)} 的缩略图" src="${esc(image)}">` : `<span class="files-sheet" aria-hidden="true"><b>${esc(file.format)}</b></span>`}</div>
      ${image ? '' : `<button type="button" class="ef-chip" data-inspect-act="preview" ${preview ? 'disabled' : ''}>${preview ? '正在读取…' : message ? '重新读取预览' : '读取缩略图'}</button>`}
      ${message ? `<p class="files-help" role="status">${esc(message)}</p>` : ''}
      <dl class="files-facts"><div><dt>大小</dt><dd>${file.size === undefined ? '未上报' : byteText(file.size)}</dd></div><div><dt>修改时间</dt><dd>${esc(when(file))}</dd></div></dl>
      <h4>完整路径</h4><code>${esc(file.path || '设备只返回了文件名。')}</code>
      ${file.path ? '<button type="button" class="ef-chip" data-inspect-act="path">复制路径</button>' : ''}
      ${file.kind === 'model' ? '<details><summary>切片摘要</summary><p class="files-help">文件接口未提供预计耗时、耗材重量或工具头数据。</p></details>' : ''}
      <details><summary>Bridge API</summary>${(['download','delete'] as const).map(op => `<p class="files-kicker">POST</p><code>${endpoint}/${op}</code><button type="button" class="ef-chip" data-inspect-act="${op}-curl">复制 ${op === 'download' ? '下载' : '删除'} cURL</button>`).join('')}<p class="files-help">${ctx.source === 'lan' ? '示例以占位符替代局域网地址和访问码。' : '示例使用当前桥接和文件引用。'}</p></details>
      <span class="files-help" role="status" data-copy-note></span>
      <div class="files-detail-actions"><button type="button" class="ef-btn small go" data-entry-act="download">↓ 下载到本地</button><button type="button" class="ef-btn small danger" data-entry-act="delete">从打印机删除</button></div></div>`;
    }
    place();
    if (file) updateControls();
  }
  function close() { file = null; ctx = null; clearPreview(); render(); }
  async function onClick(e: MouseEvent) {
    const op = e.target instanceof Element ? e.target.closest<HTMLElement>('[data-inspect-act]')?.dataset.inspectAct : undefined;
    if (!op || !file || !ctx || !alive()) return;
    if (op === 'close') { close(); return; }
    const f = file, c = ctx;
    if (op === 'preview' && !preview && !image) {
      const ctl = new AbortController(); preview = ctl; message = ''; render();
      try {
        const res = await fetch(`${base(c)}/thumbnail`, { method: 'POST', headers: { 'content-type': 'application/json', accept: 'image/png, image/jpeg' }, body: JSON.stringify(payload(c, f.kind, f)), signal: ctl.signal });
        if (!res.ok) {
          const info = await res.json().catch(() => null);
          throw new ApiError(res.status, info?.error?.code || 'request_failed');
        }
        if (!/^image\/(png|jpeg)(;|$)/i.test(res.headers.get('content-type') || '')) throw new ApiError(res.status, 'bad_response');
        const blob = await res.blob();
        if (preview !== ctl || !alive()) return;
        image = URL.createObjectURL(blob);
      } catch (err) {
        if (preview !== ctl || !alive()) return;
        message = err instanceof ApiError && err.code === 'thumbnail_not_found' ? '打印机没有提供这个文件的缩略图。' : errorText(err);
      } finally {
        if (preview === ctl && alive()) { preview = null; render(); }
      }
      return;
    }
    const text = op === 'path' ? f.path : op === 'download-curl' || op === 'delete-curl' ? curl(c, f, op === 'delete-curl' ? 'delete' : 'download') : '';
    if (!text) return;
    try { await navigator.clipboard.writeText(text); copyMessage = '已复制。'; } catch { copyMessage = '浏览器未允许复制。'; }
    if (file === f && ctx === c && alive()) { const note = box.querySelector('[data-copy-note]'); if (note) note.textContent = copyMessage; }
  }
  const life = new AbortController();
  box.addEventListener('click', onClick, { signal: life.signal });
  drawer.addEventListener('cancel', e => { e.preventDefault(); close(); }, { signal: life.signal });
  wide.addEventListener('change', place, { signal: life.signal });
  render();
  return {
    get file() { return file; },
    open(f: Entry, c: Context) { clearPreview(); file = f; ctx = c; render(); }, close,
    dispose() { life.abort(); clearPreview(); if (drawer.open) drawer.close(); },
  };
}
