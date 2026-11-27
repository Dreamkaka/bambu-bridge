"""Read-only storage REST integration using the same-toolchain networking and source fixtures."""
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
        def post(path, body, status=200, error=None):
            return api(port, 'POST', path, status, body=json.dumps(body), headers=JSON, error=error)

        post(CLOUD_FILES, CLOUD, 409, 'login_required')
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
            code, headers, data = http(port, 'POST', V1 + path + '/download',
                                       json.dumps({**options, 'path': FILE}), JSON)
            assert code == 200 and headers['content-type'] == 'application/octet-stream'
            assert unquote(headers['content-disposition']).endswith("UTF-8''模型.3mf")
            assert data == b'\x00\x01\x02\n\nX'

        legacy = {**LAN, 'type': 'timelapse'}
        assert post(LAN_FILES, legacy) == {'files': [{'name': 'legacy.mp4', 'size': 0}]}
        code, _, data = http(port, 'POST', V1 + LAN_FILES + '/download',
                            json.dumps({**legacy, 'name': 'legacy.mp4'}), JSON)
        assert code == 200 and data == b''

        try:
            http(port, 'POST', V1 + LAN_FILES + '/download',
                 json.dumps({**LAN, 'storage': 'internal', 'path': '/userdata/model/broken.3mf'}), JSON)
        except IncompleteRead:
            pass  # An invalid terminal frame must not turn a partial body into a successful download.
        else:
            raise AssertionError('accepted a download with a mismatched chunk offset')
    print('Printer files: LAN and cloud listing, binary downloads, legacy empty file and failed transfers passed')


if __name__ == '__main__':
    main(*(str(Path(arg).resolve()) for arg in sys.argv[1:]))
