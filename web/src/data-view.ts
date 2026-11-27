import { esc } from './html';

export function marked(text: string, pattern: RegExp | null) {
  if (!pattern) return esc(text);
  let end = 0, html = '';
  for (const match of text.matchAll(pattern)) {
    if (!match[0]) continue;
    html += esc(text.slice(end, match.index)) + `<mark>${esc(match[0])}</mark>`;
    end = match.index + match[0].length;
  }
  return html + esc(text.slice(end));
}

// Live updates may change button text/counts while someone is using the keyboard.
const previous = new WeakMap<HTMLElement, string>();
export function markup(host: HTMLElement, html: string) {
  if (previous.get(host) === html) return;
  const active = document.activeElement;
  const key = active instanceof HTMLElement && host.contains(active) ? active.dataset.focus : undefined;
  previous.set(host, html);
  host.innerHTML = html;
  if (key !== undefined) host.querySelector<HTMLElement>(`[data-focus="${CSS.escape(key)}"]`)?.focus({ preventScroll: true });
}
export function button(action: string, text: string, value = '', attrs = '') {
  return `<button type="button" class="dt-button" data-action="${action}" data-value="${esc(value)}" data-focus="${esc(`${action}:${value}`)}" ${attrs}>${text}</button>`;
}
export function download(content: string, type: 'json' | 'csv', name: string, at: number) {
  const blob = new Blob([content], { type: type === 'json' ? 'application/json;charset=utf-8' : 'text/csv;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `${name}-${new Date(at).toISOString().replace(/:/g, '-')}.${type}`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
export async function copyText(text: string, host: HTMLElement) {
  if (navigator.clipboard?.writeText) {
    try { await navigator.clipboard.writeText(text); return true; } catch { /* LAN HTTP or clipboard policy. */ }
  }
  const focus = document.activeElement;
  const area = document.createElement('textarea');
  area.className = 'dt-copy-buffer';
  area.value = text;
  host.append(area);
  area.select();
  try { return document.execCommand('copy'); } finally {
    area.remove();
    if (focus instanceof HTMLElement) focus.focus({ preventScroll: true });
  }
}
