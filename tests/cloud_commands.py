"""Exercise the real stdio RPC dispatcher against the same-toolchain test library."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe, plugin = (str(Path(arg).resolve()) for arg in sys.argv[1:])
command = json.dumps({'info': {'command': 'get_version', 'sequence_id': '7'}})
requests = [dict(method='send_message', params=dict(dev_id=device, json_str=command,
                 timeout_ms=100, wait=wait)) for device, wait in
            [('online', True), ('quiet', True), ('quiet', False), ('denied', False),
             ('uncertain', True), ('', False)]]
with tempfile.TemporaryDirectory() as cfg:
    result = subprocess.run([exe, '--plugin-dir', plugin, '--config-dir', cfg, '--stdio'],
                            input=''.join(json.dumps(req) + '\n' for req in requests),
                            capture_output=True, check=True, text=True, timeout=10)
responses = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
matched, quiet, queued, denied, uncertain, invalid = [r['result'] for r in responses if r.get('ok')]
assert matched['code'] == 0 and not matched['timed_out'] and matched['payload']['info']['sequence_id'] == 7, matched
assert quiet['code'] == 0 and quiet['timed_out'] and 'payload' not in quiet, quiet
assert queued['code'] == 0 and 'payload' not in queued, queued
assert denied['code'] == -1 and 'add_subscribe' in denied['error'], denied
assert uncertain['code'] == -2 and not uncertain['timed_out'], uncertain
assert invalid['code'] != 0 and 'dev_id' in invalid['error'], invalid
print('Cloud RPC: first publish, matched reply, telemetry-only timeout, no-wait, subscription failure, no replay and invalid target passed')
