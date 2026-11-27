export function bindText(root: ParentNode, name: string, value: string) {
  const el = root.querySelector(`[data-bind="${name}"]`);
  if (el && el.textContent !== value) el.textContent = value;
}

export function bindHtml(root: ParentNode, name: string, html: string) {
  const el = root.querySelector(`[data-bind="${name}"]`);
  if (el && el.innerHTML !== html) el.innerHTML = html;
}
