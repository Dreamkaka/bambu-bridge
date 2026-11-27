export function askStop(): Promise<boolean> {
  const dialog = document.querySelector('#confirm');
  if (!(dialog instanceof HTMLDialogElement)) return Promise.resolve(false);
  if (!dialog.open) dialog.showModal();
  const { promise, resolve } = Promise.withResolvers<boolean>();
  const finish = () => {
    dialog.removeEventListener('close', finish);
    resolve(dialog.returnValue === 'stop');
  };
  dialog.addEventListener('close', finish);
  return promise;
}
export function wireDialog() {
  const dialog = document.querySelector('#confirm');
  if (!(dialog instanceof HTMLDialogElement)) return;
  dialog.addEventListener('click', (ev) => {
    if (ev.target === dialog) {
      dialog.close('cancel');
      return;
    }
    if (!(ev.target instanceof Element)) return;
    const button = ev.target.closest('[data-close]');
    if (!(button instanceof HTMLElement)) return;
    dialog.close(button.dataset.close || 'cancel');
  });
}
