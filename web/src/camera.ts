import ts from 'mpegts.js';
import { store } from './store';

function streamUrl(): string {
  const q = new URLSearchParams({ dev_id: store.prefs.devId });
  if (store.prefs.source === 'lan') {
    q.set('ip', store.prefs.lanIp);
    q.set('password', store.prefs.lanPassword);
    q.set('proto', store.prefs.cameraProto);
  }
  return `/camera.ts?${q}`;
}

// Telemetry updates never open a stream. Ignore late playback callbacks after stop.
export function mountCamera(root: HTMLElement): () => void {
  const video = root.querySelector<HTMLVideoElement>('video[data-camera]')!;
  const note = root.querySelector<HTMLElement>('[data-camera-note]')!;
  const toggle = root.querySelector<HTMLButtonElement>('[data-camera-toggle]')!;
  const full = root.querySelector<HTMLButtonElement>('[data-act="fullscreen"]');
  let prefs = store.prefs;
  let player: ts.Player | null = null;
  let generation = 0;
  let active = false;

  function stop(message = '点击播放，查看实时画面。') {
    ++generation;
    active = false;
    if (player) { player.destroy(); player = null; }
    video.hidden = true;
    if (full) full.hidden = true;
    toggle.textContent = '播放监控';
    prefs = store.prefs;
    const missing = !prefs.devId ? '先选择一台打印机。' : prefs.source === 'lan' && (!prefs.lanIp || !prefs.lanPassword) ? '请先填写局域网地址和访问码。' : '';
    toggle.disabled = !!missing;
    note.textContent = missing || message;
    note.hidden = false;
  }

  async function toggleStream() {
    if (active) { stop(); return; }
    if (toggle.disabled || document.hidden) return;
    active = true;
    const session = ++generation;
    toggle.textContent = '停止监控';
    note.textContent = '正在连接摄像头…';
    const failed = () => {
      if (session === generation) stop('未能播放视频流。请检查摄像头连接；云端不提供 MPEG-TS 时可改用局域网。');
    };
    try {
      if (!ts.isSupported()) { stop('此浏览器不支持 MPEG-TS 播放。'); return; }
      ts.LoggingControl.enableAll = false;
      player = ts.createPlayer({ type: 'mpegts', isLive: true, url: streamUrl() }, { enableStashBuffer: false, lazyLoad: false });
      player.on(ts.Events.ERROR, failed);
      player.on(ts.Events.LOADING_COMPLETE, () => { if (session === generation) stop('视频流已结束。'); });
      player.attachMediaElement(video);
      player.load();
      await player.play();
      if (session !== generation) return;
      note.hidden = true;
      video.hidden = false;
      if (full) full.hidden = false;
    } catch { failed(); }
  }
  const background = () => { if (active && document.hidden) stop('监控已停止。点击播放可重新连接。'); };
  stop();
  toggle.addEventListener('click', toggleStream);
  document.addEventListener('visibilitychange', background);
  const unsub = store.subscribe(() => { if (prefs !== store.prefs) stop(); });
  return () => {
    unsub();
    toggle.removeEventListener('click', toggleStream);
    document.removeEventListener('visibilitychange', background);
    stop();
  };
}
