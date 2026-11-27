"""Exercise the real local REST, SSE and WebSocket transports with the synthetic plugin."""
import contextlib
from http import client as http_client
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import parse_qs, urlsplit

LOOPBACK = '127.0.0.1'
TIMEOUT = 4
V1 = '/api/v1'
KEY = 'dGhlIHNhbXBsZSBub25jZQ=='


@contextlib.contextmanager
def bridge(exe, plugin, web=True):
    with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryFile() as log:
        with socket.socket() as probe:
            probe.bind((LOOPBACK, 0))
            port = probe.getsockname()[1]
        args = [exe, '--plugin-dir', plugin, '--config-dir', tmp, '--http', str(port), '--stdio']
        if web:
            args += ['--country', 'US', '--web-api']
        with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log) as proc:
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with socket.create_connection((LOOPBACK, port), timeout=0.2):
                            break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() >= deadline:
                            raise
                        time.sleep(0.025)
                yield proc, port
            except BaseException:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=5)
                log.seek(0)
                print(log.read().decode(errors='replace')[-12000:], file=sys.stderr)
                raise
            finally:
                proc.stdin.close()
                if proc.poll() is None:
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
                        raise


def http(port, method, path, body=None, headers=None):
    with contextlib.closing(http_client.HTTPConnection(LOOPBACK, port, timeout=TIMEOUT)) as client:
        client.request(method, path, body, headers or {})
        with client.getresponse() as resp:
            return resp.status, dict((k.lower(), v) for k, v in resp.getheaders()), resp.read()


def api(port, method, path, status, *, body=None, headers=None, error=None, plugin_code=None):
    status_actual, hdr, raw = http(port, method, V1 + path, body, headers)
    assert status_actual == status, (method, path, status_actual, raw)
    assert hdr['content-type'] == 'application/json; charset=utf-8' and hdr['cache-control'] == 'no-store', hdr
    assert 'access-control-allow-origin' not in hdr, hdr
    data = json.loads(raw)
    if error:
        expected = {'code': error}
        if plugin_code is not None:
            expected['plugin_code'] = plugin_code
        assert data == {'ok': False, 'error': expected}, data
        return hdr
    assert data['ok'] is True and data.keys() == {'ok', 'result'}, data
    return data['result']


def ws_headers(port):
    host = f'localhost:{port}'
    return {'Host': host, 'Origin': f'http://{host}', 'Upgrade': 'WebSocket',
            'Connection': 'keep-alive, UpGrAdE', 'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': KEY}


def masked(opcode, payload):
    assert len(payload) <= 125
    key = b'\x13\x29\x7f\x85'
    return bytes((0x80 | opcode, 0x80 | len(payload))) + key + bytes(b ^ key[i % 4] for i, b in enumerate(payload))


def close_code(code):
    return struct.pack('!H', code)


def read_exact(stream, length):
    data = stream.read(length)
    assert len(data) == length, ('unexpected EOF', data)
    return data


def read_frame(stream):
    op, length = read_exact(stream, 2)
    assert op & 0xf0 == 0x80 and length & 0x80 == 0, (op, length)
    if length >= 126:
        length = int.from_bytes(read_exact(stream, 2 if length == 126 else 8), 'big')
    assert length <= 1024 * 1024, length
    return op & 15, read_exact(stream, length)


def ws_event(stream, name):
    for _ in range(16):
        op, payload = read_frame(stream)
        assert op == 1, (op, payload)
        event = json.loads(payload)
        assert event.keys() == {'event', 'payload'}, event
        if event['event'] == name:
            return event['payload']
    raise AssertionError(f'no {name} event')


@contextlib.contextmanager
def websocket(port, first_frame=b''):
    with socket.create_connection((LOOPBACK, port), timeout=TIMEOUT) as sock, sock.makefile('rb') as stream:
        req = f'GET {V1}/ws HTTP/1.1\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in ws_headers(port).items())
        sock.sendall((req + '\r\n').encode('ascii') + first_frame)
        line = stream.readline()
        assert line == b'HTTP/1.1 101 Switching Protocols\r\n', line
        headers = http_client.parse_headers(stream)
        assert headers['Sec-WebSocket-Accept'] == 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=', headers
        assert read_frame(stream) == (1, b'{"event":"ready","payload":{}}')
        yield sock, stream


@contextlib.contextmanager
def sse(port):
    with contextlib.closing(http_client.HTTPConnection(LOOPBACK, port, timeout=TIMEOUT)) as client:
        client.request('GET', '/events')
        with client.getresponse() as stream:
            assert stream.status == 200 and stream.getheader('Content-Type') == 'text/event-stream'
            yield stream


def sse_event(stream, name):
    for _ in range(16):
        line = stream.readline()
        assert line, 'unexpected SSE EOF'
        if line.startswith(b':'):
            assert stream.readline() == b'\n'
            continue
        data = stream.readline()
        assert line.startswith(b'event: ') and data.startswith(b'data: ') and stream.readline() == b'\n'
        event = json.loads(data[6:])
        assert line[7:].strip().decode() == event['event']
        if event['event'] == name:
            return event['payload']
    raise AssertionError(f'no {name} SSE event')


def check_access(port):
    host = f'localhost:{port}'
    status = api(port, 'GET', '/status', 200)
    assert status.keys() == {'loaded', 'agent_ready', 'logged_in', 'server_connected', 'version',
                             'client_version', 'country', 'mqtt_ready'}, status
    assert status['agent_ready'] is True and status['logged_in'] is False and status['country'] == 'US', status
    api(port, 'GET', '/printers', 409, error='login_required')
    for hdr in ({'Origin': 'https://other.invalid'}, {'Host': host, 'Origin': f'http://{LOOPBACK}:{port}'},
                {'Host': 'other.invalid', 'X-Forwarded-Host': host}, {'Origin': 'null'}, {'Origin': ''},
                {'Sec-Fetch-Site': 'cross-site'}, {'Host': f'localhost:0{port}'}):
        api(port, 'GET', '/status', 403, headers=hdr, error='forbidden_origin')
    api(port, 'GET', '/status', 200, headers={'Host': f'LOCALHOST:{port}', 'Origin': f'http://{host}'})
    assert http(port, 'POST', '/rpc', '{}', {'Origin': 'https://other.invalid'})[0] == 403
    login = api(port, 'GET', '/session/login-url', 200)
    assert login.keys() == {'url'}, login
    to, = parse_qs(urlsplit(login['url']).query)['to']
    assert parse_qs(urlsplit(to).query)['redirect_url'] == [f'http://{host}'], login
    assert api(port, 'POST', '/status', 405, error='method_not_allowed')['allow'] == 'GET'
    api(port, 'GET', '/status?ticket=web-ticket', 400, error='invalid_query')
    assert http(port, 'GET', '/health?ticket=web-ticket', headers={'Host': host})[0] == 200
    nav = {'Host': host, 'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'navigate', 'Sec-Fetch-Dest': 'document'}
    callback = '/sign-in/callback?ticket=web-ticket'
    assert http(port, 'GET', callback, headers={**nav, 'Sec-Fetch-Dest': 'iframe'})[0] == 403
    assert http(port, 'GET', callback, headers={**nav, 'Host': f'{LOOPBACK}:{port}'})[0] == 403
    assert api(port, 'GET', '/status', 200)['logged_in'] is False
    return callback, nav


def check_handshake(port):
    headers = ws_headers(port)
    missing_origin = {k: v for k, v in headers.items() if k != 'Origin'}
    api(port, 'GET', '/ws', 403, headers=missing_origin, error='forbidden_origin')
    old = {**headers, 'Sec-WebSocket-Version': '12'}
    assert api(port, 'GET', '/ws', 426, headers=old, error='unsupported_websocket_version')['sec-websocket-version'] == '13'
    assert api(port, 'POST', '/ws', 405, headers=headers, error='method_not_allowed')['allow'] == 'GET'
    for override in ({'Sec-WebSocket-Key': KEY[:-3] + 'R=='}, {'Upgrade': 'websocket, other'}, {'Connection': 'x-upgrade'}):
        api(port, 'GET', '/ws', 400, headers={**headers, **override}, error='invalid_websocket_handshake')


def check_commands(port, first, second, events):
    json_headers = {'Content-Type': 'Application/JSON; charset=utf-8'}
    cloud = '{"source":"cloud"}'
    refresh = '/printers/online/status/refresh'
    assert api(port, 'POST', refresh, 202, body=cloud, headers=json_headers) == {
        'dev_id': 'online', 'source': 'cloud', 'accepted': True}
    expected = {'dev_id': 'online', 'source': 'cloud', 'report': {'command': 'push_status', 'nozzle_temper': 215}}
    assert ws_event(first, 'printer_status') == expected
    assert ws_event(second, 'printer_status') == expected
    raw = sse_event(events, 'on_message')
    assert raw['dev_id'] == 'online' and json.loads(raw['msg'])['print'] == expected['report']
    assert 'system' in json.loads(raw['msg'])
    api(port, 'POST', '/printers/denied/status/refresh', 502, body=cloud, headers=json_headers,
        error='upstream_failed', plugin_code=-1)
    for action, state in [('pause', 'PAUSE'), ('resume', 'RUNNING'), ('stop', 'IDLE')]:
        assert api(port, 'POST', '/printers/online/actions/' + action, 202, body=cloud, headers=json_headers) == {
            'dev_id': 'online', 'source': 'cloud', 'accepted': True}
        assert ws_event(first, 'printer_status') == {
            'dev_id': 'online', 'source': 'cloud', 'report': {'command': 'push_status', 'gcode_state': state}}
    lan = {'dev_id': 'lan-fixture', 'dev_ip': '192.0.2.10', 'password': 'fixture-pass'}
    assert api(port, 'POST', '/lan/connection', 202, body=json.dumps(lan), headers=json_headers) == {
        'dev_id': 'lan-fixture', 'accepted': True}
    assert ws_event(first, 'lan_connection') == {'dev_id': 'lan-fixture', 'status': 0}
    assert ws_event(first, 'printer_status') == {**expected, 'dev_id': 'lan-fixture', 'source': 'lan'}
    assert api(port, 'DELETE', '/lan/connection', 200) == {'accepted': True}

    def count():
        status, _, data = http(port, 'POST', '/api/status')
        assert status == 200, data
        return int(json.loads(data)['result']['user_id'])

    before = count()
    assert before == 5, before  # cloud refresh, three actions, LAN's initial pushall
    for path, body, error in [(refresh, '{', 'invalid_body'), (refresh, '{"source":"other"}', 'invalid_body'),
                              (refresh, '\v' + cloud, 'invalid_body'),
                              (refresh, '{"source":"cloud","source":"lan"}', 'invalid_body'),
                              ('/printers/bad%2Fid/status/refresh', cloud, 'invalid_device_id'),
                              ('/lan/connection', json.dumps({**lan, 'dev_ip': lan['dev_ip'] + '\0x'}), 'invalid_body')]:
        api(port, 'POST', path, 400, body=body, headers=json_headers, error=error)
    api(port, 'POST', refresh, 413, body=' ' * 4095 + cloud, headers=json_headers, error='body_too_large')
    api(port, 'POST', refresh, 415, body=cloud, error='unsupported_media_type')
    api(port, 'POST', '/printers/online/actions/reboot', 404, body=cloud, headers=json_headers, error='not_found')
    assert count() == before


def check_frames(port):
    with websocket(port, masked(9, b'early')) as (sock, stream):
        assert read_frame(stream) == (10, b'early')
        ping = masked(9, b'split ping')
        sock.sendall(ping[:1])
        time.sleep(0.06)
        sock.sendall(ping[1:5])
        time.sleep(0.06)
        sock.sendall(ping[5:] + masked(9, b'next'))
        assert read_frame(stream) == (10, b'split ping')
        assert read_frame(stream) == (10, b'next')
        sock.sendall(masked(8, close_code(1000)))
        assert read_frame(stream) == (8, close_code(1000))
        assert stream.read(1) == b''
    for frame, code in [(b'\x89\x00', 1002), (masked(8, close_code(1005)), 1002),
                        (masked(8, close_code(1000) + b'\xed\xa0\x80'), 1007),
                        (masked(1, b'text')[:6], 1003)]:
        with websocket(port) as (sock, stream):
            sock.sendall(frame)
            assert read_frame(stream) == (8, close_code(code))


def check_capacity(port):
    with contextlib.ExitStack() as clients:
        sockets = [clients.enter_context(websocket(port)) for _ in range(16)]
        api(port, 'GET', '/ws', 503, headers=ws_headers(port), error='stream_capacity')
        sock, stream = sockets[0]
        sock.sendall(masked(8, b''))
        assert read_frame(stream) == (8, b'') and stream.read(1) == b''
        with websocket(port):
            pass


def main(exe, plugin):
    for args in ([], ['--http', '0'], ['--http', '65536'], ['--http', '1junk'],
                 ['--http', '99999999999999999999'], ['--http', '18080', '--http', '0']):
        result = subprocess.run([exe, '--web-api', *args], capture_output=True, timeout=5)
        assert result.returncode == 1, (args, result)
        assert b'[bridge] --web-api requires --http with a port from 1 to 65535' in result.stderr, result
    with bridge(exe, plugin) as (proc, port):
        callback, nav = check_access(port)
        check_handshake(port)
        with websocket(port) as (_, first), websocket(port) as (_, second), sse(port) as events:
            status, _, page = http(port, 'GET', callback, headers=nav)
            assert status == 200 and b'Authentication complete' in page, page
            assert ws_event(first, 'account_changed') == {'logged_in': True}
            assert ws_event(first, 'login_result') == ws_event(second, 'login_result') == {'success': True}
            assert sse_event(events, 'login_result') == {'success': True}
            assert api(port, 'GET', '/printers', 200) == {'printers': [{
                'dev_id': 'online', 'dev_name': 'Fixture', 'dev_online': True, 'dev_model_name': None,
                'dev_product_name': None, 'task_status': None}]}
            check_commands(port, first, second, events)
        check_frames(port)
        check_capacity(port)
        with websocket(port) as (_, active), sse(port) as events, socket.create_connection((LOOPBACK, port), TIMEOUT) as idle:
            idle.sendall(b'GET /health HTTP/1.1\r\n')
            proc.stdin.close()
            assert read_frame(active) == (8, close_code(1001))
            assert events.read() == b''
            assert proc.wait(timeout=5) == 0

    with bridge(exe, plugin, web=False) as (_, port):
        api(port, 'GET', '/status', 404, error='not_found')
        api(port, 'POST', '/status?ticket=web-ticket', 404, error='not_found')
        assert http(port, 'GET', '/health')[0] == 200
        assert http(port, 'POST', '/api/status', '{}', {'Origin': 'https://old-client.invalid'})[0] == 200
    print('Web API: local access, OAuth, REST, cloud/LAN reports, SSE, WebSocket framing and shutdown passed')


if __name__ == '__main__':
    main(*(str(Path(arg).resolve()) for arg in sys.argv[1:]))
