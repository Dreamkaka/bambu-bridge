export class ApiError extends Error {
  status: number;
  code: string;
  pluginCode?: number;
  printerCode?: number;

  constructor(status: number, code: string, pluginCode?: number, printerCode?: number) {
    super(code);
    this.status = status;
    this.code = code;
    this.pluginCode = pluginCode;
    this.printerCode = printerCode;
  }
}

const ERRORS: Record<string, string> = {
  login_required: '还没有登录。',
  agent_unavailable: '桥接还没就绪。',
  forbidden_origin: '请求来源不被接受。',
  invalid_body: '请求内容不正确。',
  invalid_query: '这个接口不接受查询参数。',
  invalid_device_id: '设备编号不正确。',
  storage_plugin_unavailable: '缺少读取存储的插件。',
  storage_connection_failed: '连不上打印机存储。',
  storage_timeout: '读取存储超时。',
  file_not_found: '文件已经不在列表里。',
  storage_unavailable: '这块存储现在不可用。',
  storage_request_failed: '打印机拒绝了这次读取。',
  storage_cloud_unavailable: '云端存储不可用。',
  file_too_large: '文件超过 512 MiB，没有下载。',
  body_too_large: '请求太大。',
  unsupported_media_type: '请求类型不正确。',
  not_found: '没有这个接口。',
  method_not_allowed: '这个接口不接受该操作。',
  internal_error: '桥接内部出错。',
  bad_response: '接口没有返回可解析的结果。',
  request_failed: '请求失败。',
  network: '连不上本机桥接。',
};

export function errorText(error: unknown): string {
  if (error instanceof ApiError) {
    const base = ERRORS[error.code] ?? `请求失败（${error.code}）。`;
    const extra = [
      error.pluginCode ? `插件码 ${error.pluginCode}` : '',
      error.printerCode ? `打印机码 ${error.printerCode}` : '',
    ].filter(Boolean);
    return extra.length ? `${base} ${extra.join(' / ')}` : base;
  }
  if (error instanceof TypeError) return ERRORS.network;
  return ERRORS.request_failed;
}

async function read<T>(res: Response): Promise<T> {
  const text = await res.text();
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch {
    throw new ApiError(res.status, 'bad_response');
  }
  if (!json || typeof json !== 'object') throw new ApiError(res.status, 'bad_response');
  if (!('ok' in json) || json.ok !== true) {
    let code = 'request_failed';
    let plugin: number | undefined;
    let printer: number | undefined;
    if ('error' in json && json.error && typeof json.error === 'object') {
      const err = json.error;
      if ('code' in err && typeof err.code === 'string') code = err.code;
      if ('plugin_code' in err && typeof err.plugin_code === 'number') plugin = err.plugin_code;
      if ('printer_code' in err && typeof err.printer_code === 'number') printer = err.printer_code;
    }
    throw new ApiError(res.status, code, plugin, printer);
  }
  if (!('result' in json)) throw new ApiError(res.status, 'bad_response');
  return json.result as T;
}

export async function getJson<T>(path: string): Promise<T> {
  const res = await fetch(path, { headers: { accept: 'application/json' } });
  return read<T>(res);
}

export async function postJson<T>(path: string, body: unknown): Promise<{ status: number; result: T }> {
  const res = await fetch(path, {
    method: 'POST',
    headers: { 'content-type': 'application/json', accept: 'application/json' },
    body: JSON.stringify(body),
  });
  return { status: res.status, result: await read<T>(res) };
}

export async function delJson<T>(path: string): Promise<T> {
  const res = await fetch(path, { method: 'DELETE', headers: { accept: 'application/json' } });
  return read<T>(res);
}

function filenameOf(header: string | null, fallback: string): string {
  if (!header) return fallback;
  const encoded = /filename\*=UTF-8''([^;]+)/i.exec(header);
  if (encoded) {
    try {
      return decodeURIComponent(encoded[1]);
    } catch {
      return fallback;
    }
  }
  return fallback;
}

export async function downloadBlob(
  path: string, body: unknown, fallback: string,
  options?: { signal: AbortSignal; onProgress: (received: number, total?: number) => void; canSave: () => boolean },
): Promise<string> {
  let res: Response;
  try {
    res = await fetch(path, {
      method: 'POST',
      headers: { 'content-type': 'application/json', accept: 'application/octet-stream' },
      body: JSON.stringify(body),
      signal: options?.signal,
    });
  } catch {
    throw new ApiError(0, 'network');
  }
  const type = res.headers.get('content-type') || '';
  if (!res.ok || type.includes('json') || type.startsWith('text/')) {
    await read(res);
    throw new ApiError(res.status, 'request_failed');
  }
  let blob: Blob;
  if (options && res.body) {
    const length = res.headers.get('content-length');
    const total = length && /^\d+$/.test(length) ? Number(length) : undefined;
    let received = 0;
    const stream = res.body.pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
      transform(chunk, controller) {
        received += chunk.byteLength;
        options.onProgress(received, total);
        controller.enqueue(chunk);
      },
    }));
    blob = await new Response(stream, { headers: { 'content-type': type } }).blob();
  } else {
    blob = await res.blob();
    options?.onProgress(blob.size, blob.size);
  }
  if (options && (options.signal.aborted || !options.canSave())) throw new DOMException('Aborted', 'AbortError');
  const name = filenameOf(res.headers.get('content-disposition'), fallback);
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.click();
  URL.revokeObjectURL(url);
  return name;
}
