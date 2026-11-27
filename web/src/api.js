const ERRORS = {
  login_required: '账号还没有登录。',
  agent_unavailable: '桥接还没有就绪。',
  upstream_failed: '插件没有接受这次请求。',
  forbidden_origin: '这个页面和桥接不在同一个地址。',
  invalid_body: '填写的内容不符合要求。',
  invalid_device_id: '序列号只能包含字母、数字、下划线和连字符。',
  invalid_query: '请求不被接受。',
  not_found: '没有这个接口。',
  method_not_allowed: '这个操作不被接受。',
  body_too_large: '提交的内容太长。',
  unsupported_media_type: '提交的格式不对。',
  internal_error: '桥接内部出错。',
  stream_capacity: '事件通道已经满了。',
};

const PLUGIN = {
  '-1': '无效句柄',
  '-2': '连接失败',
  '-4': '发送失败',
  '-17': '超时',
  '-18': '已取消',
  '-19': '无效结果',
};

const CAMERA_ERRORS = {
  'not logged in': '还没有登录，云端画面不可用。',
  'missing dev_id': '没有序列号。',
  'BambuSource not loaded': '没有加载摄像头组件。',
  'resolve camera URL failed': '没有拿到摄像头地址。',
  'BRTC/WebRTC URL is not supported for snapshots; try LAN': '云端这路画面不能拍成图片，可以改用局域网。',
};

export class ApiError extends Error {
  constructor(code, pluginCode, status) {
    super(code || 'bad_response');
    this.code = code || 'bad_response';
    this.pluginCode = pluginCode;
    this.status = status;
  }
}

export function explain(error) {
  if (error instanceof ApiError) {
    if (error.code === 'network') return '没有连上桥接。';
    if (error.code === 'bad_response') return '桥接没有返回可识别的结果。';
    const sentence = ERRORS[error.code] || `操作没有完成（${error.code}）。`;
    if (error.pluginCode == null) return sentence;
    const name = PLUGIN[String(error.pluginCode)];
    const core = sentence.replace(/。$/, '');
    return name ? `${core}（${error.pluginCode} ${name}）。` : `${core}（${error.pluginCode}）。`;
  }
  if (error instanceof Error && error.message) return error.message;
  return '没有完成这次操作。';
}

async function request(method, path, body) {
  const options = { method, headers: {}, cache: 'no-store' };
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  let response;
  try {
    response = await fetch(`/api/v1${path}`, options);
  } catch {
    throw new ApiError('network', null, 0);
  }
  let data = null;
  try {
    data = await response.json();
  } catch {
    throw new ApiError('bad_response', null, response.status);
  }
  if (!data || data.ok !== true || !Object.prototype.hasOwnProperty.call(data, 'result')) {
    const err = data && data.error && typeof data.error === 'object' ? data.error : {};
    throw new ApiError(typeof err.code === 'string' ? err.code : 'bad_response', err.plugin_code ?? null, response.status);
  }
  return { status: response.status, result: data.result };
}

export const status = () => request('GET', '/status');
export const loginUrl = () => request('GET', '/session/login-url');
export const printers = () => request('GET', '/printers');

export function refresh(id, source) {
  return request('POST', `/printers/${encodeURIComponent(id)}/status/refresh`, { source });
}

export function action(id, name, source) {
  return request('POST', `/printers/${encodeURIComponent(id)}/actions/${name}`, { source });
}

export function connectLan(body) {
  return request('POST', '/lan/connection', body);
}

export function disconnectLan() {
  return request('DELETE', '/lan/connection');
}

export async function snapshot(params) {
  let response;
  try {
    response = await fetch(`/camera.jpg?${params.toString()}`, { cache: 'no-store' });
  } catch {
    throw new Error('没有连上桥接。');
  }
  if (!response.ok) {
    let message = '没有拍到画面。';
    try {
      const data = await response.json();
      if (data && typeof data.error === 'string') message = CAMERA_ERRORS[data.error] || data.error;
    } catch { /* response was not JSON */ }
    throw new Error(message);
  }
  const blob = await response.blob();
  if (!blob.size || (blob.type && !blob.type.includes('image'))) throw new Error('没有拍到画面。');
  return blob;
}

export function openEvents(onEvent, onState) {
  let socket = null;
  let stopped = false;
  let delay = 1000;
  let timer = 0;
  const connect = () => {
    if (stopped) return;
    onState('connecting');
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/v1/ws`;
    socket = new WebSocket(url);
    socket.onmessage = (ev) => {
      let message = null;
      try {
        message = JSON.parse(ev.data);
      } catch {
        return;
      }
      if (!message || typeof message.event !== 'string') return;
      if (message.event === 'ready') onState('open');
      onEvent(message);
    };
    socket.onclose = () => {
      onState('closed');
      if (stopped) return;
      timer = setTimeout(connect, delay);
      delay = Math.min(8000, delay * 2);
    };
  };
  connect();
  return () => {
    stopped = true;
    clearTimeout(timer);
    socket?.close();
  };
}
