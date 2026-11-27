"""Storage REST integration using the same-toolchain networking and source fixtures."""
from http.client import IncompleteRead
import json
from pathlib import Path
import sys
from urllib.parse import unquote

from web_api import V1, api, bridge, check_access, http

JSON = {'Content-Type': 'application/json'}
LAN = {'source': 'lan', 'dev_ip': '192.0.2.10', 'password': 'a&b+c', 'storage': 'external', 'type': 'model'}
CLOUD = {'source': 'cloud', 'storage': 'external', 'type': 'model'}
LAN_FILES = '/printers/lan-fixture/files'
CLOUD_FILES = '/printers/storage-fixture/files'
FILE = '/cache/模型.3mf'


def main(exe, plugin):
    with bridge(exe, plugin) as (_, port):
        def post(path, body, status=200, error=None, *, headers=JSON):
            return api(port, 'POST', path, status, body=json.dumps(body), headers=headers, error=error)

        post(CLOUD_FILES, CLOUD, 409, 'login_required')
        for action in ('delete', 'thumbnail'):
            post(f'{CLOUD_FILES}/{action}', {**CLOUD, 'path': FILE}, 409, 'login_required')
        post(LAN_FILES + '/delete', {**LAN, 'path': FILE}, 415, 'unsupported_media_type', headers={})
        post(LAN_FILES + '/delete', {**LAN, 'path': FILE}, 403, 'forbidden_origin', headers={
            **JSON, 'Origin': 'https://other.invalid'})
        post(CLOUD_FILES, CLOUD, 415, 'unsupported_media_type', headers={'Content-Type': 'application/xml'})
        post(LAN_FILES, LAN, 403, 'forbidden_origin', headers={
            'Content-Type': 'text/plain', 'Origin': 'https://other.invalid'})
        api(port, 'POST', LAN_FILES, 400, body='source=lan', error='invalid_body',
            headers={'Content-Type': 'application/x-www-form-urlencoded'})
        post('/printers/lan-fixture/actions/pause', {'source': 'lan'}, 415, 'unsupported_media_type',
             headers={'Content-Type': 'text/plain'})
        post(LAN_FILES + '/download', {**LAN, 'path': '/cache/../outside'}, 400, 'invalid_body')
        post(LAN_FILES + '/download', {**LAN, 'path': '/cache/not-listed'}, 404, 'file_not_found')
        post(LAN_FILES + '/download', {**LAN, 'path': '/cache/huge.3mf'}, 413, 'file_too_large')
        post(LAN_FILES, {**LAN, 'type': 'video'}, 409, 'storage_unavailable')
        callback, navigation = check_access(port)
        assert http(port, 'GET', callback, headers=navigation)[0] == 200

        for path, options in [(LAN_FILES, LAN), (CLOUD_FILES, CLOUD)]:
            assert post(path, options) == {'files': [
                {'name': '模型.3mf', 'path': FILE, 'size': 6},
                {'name': 'huge.3mf', 'path': '/cache/huge.3mf', 'size': 536870913},
            ]}
            for raw in ({}, {'Content-Type': 'Text/Plain; charset=utf-8'}):
                assert post(path, options, headers=raw) == post(path, options)
            code, headers, data = http(port, 'POST', V1 + path + '/download',
                                       json.dumps({**options, 'path': FILE}),
                                       {'Content-Type': 'application/x-www-form-urlencoded'})
            assert code == 200 and headers['content-type'] == 'application/octet-stream'
            assert unquote(headers['content-disposition']).endswith("UTF-8''模型.3mf")
            assert data == b'\x00\x01\x02\n\nX'

            for extra in ({}, {'plate': 2}):
                code, headers, data = http(port, 'POST', V1 + path + '/thumbnail',
                                           json.dumps({**options, 'path': FILE, **extra}), JSON)
                assert (code, headers['content-type'], data) == (200, 'image/png', b'\x89PNG\r\n\x1a\n')
                assert headers['cache-control'] == 'no-store' and headers['x-content-type-options'] == 'nosniff'
            post(path + '/thumbnail', {**options, 'path': FILE, 'plate': 0}, 400, 'invalid_body')
            post(path + '/thumbnail', {**options, 'path': FILE, 'plate': 3}, 502, 'storage_request_failed')
            post(path + '/thumbnail', {**options, 'path': FILE, 'plate': 4}, 404, 'thumbnail_not_found')
            assert post(path + '/delete', {**options, 'path': FILE}) == {'deleted': True}
            post(path + '/delete', {**options, 'path': FILE}, 404, 'file_not_found')
            post(path + '/thumbnail', {**options, 'path': FILE}, 404, 'file_not_found')

        legacy = {**LAN, 'type': 'timelapse'}
        assert post(LAN_FILES, legacy) == {'files': [{'name': 'legacy.mp4', 'size': 0}]}
        code, _, data = http(port, 'POST', V1 + LAN_FILES + '/download',
                            json.dumps({**legacy, 'name': 'legacy.mp4'}), JSON)
        assert code == 200 and data == b''
        post(LAN_FILES + '/delete', {**legacy, 'name': 'legacy.mp4'}, 502, 'file_delete_unconfirmed')
        for options in ({**legacy, 'name': 'legacy.mp4'},
                        {**LAN, 'type': 'video', 'storage': 'internal', 'path': '/userdata/media/clip.mp4'}):
            code, headers, data = http(port, 'POST', V1 + LAN_FILES + '/thumbnail', json.dumps(options), JSON)
            assert (code, headers['content-type'], data) == (200, 'image/jpeg', b'\xff\xd8\xff\xd9')

        try:
            http(port, 'POST', V1 + LAN_FILES + '/download',
                 json.dumps({**LAN, 'storage': 'internal', 'path': '/userdata/model/broken.3mf'}), JSON)
        except IncompleteRead:
            pass  # An invalid terminal frame must not turn a partial body into a successful download.
        else:
            raise AssertionError('accepted a download with a mismatched chunk offset')
    print('Printer files: LAN and cloud listing, downloads, deletion, thumbnails and error handling passed')


if __name__ == '__main__':
    main(*(str(Path(arg).resolve()) for arg in sys.argv[1:]))
