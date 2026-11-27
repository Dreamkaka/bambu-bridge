import {
  flatten,
  ipv4Little,
  mergeJson,
  present,
  pushSample,
  seriesPoints,
  type Sample,
} from './logic.ts';

function assert(cond: unknown, message: string): asserts cond {
  if (!cond) throw new Error(message);
}

const merged = mergeJson(
  { nozzle_temper: 20, ams: { tray_now: '1', ams: [{ id: '0' }] } },
  { nozzle_temper: 21, ams: { tray_now: '2' } },
);
assert(
  merged && typeof merged === 'object' && !Array.isArray(merged) &&
    merged.nozzle_temper === 21 &&
    merged.ams && typeof merged.ams === 'object' && !Array.isArray(merged.ams) &&
    merged.ams.tray_now === '2' && Array.isArray(merged.ams.ams),
  'merge keeps omitted arrays and overwrites scalars',
);

const leaves = flatten({ nozzle_temper: 1, upload: { oss_url: 'http://secret', progress: 2 } });
const hidden = leaves.find((leaf) => leaf.path === 'upload.oss_url');
assert(hidden?.hidden === true && hidden.value === null, 'secret values are hidden');
assert(!JSON.stringify(leaves).includes('secret'), 'secret text is not retained');
assert(leaves.some((leaf) => leaf.path === 'upload.progress' && leaf.value === 2), 'sibling fields stay');

assert(ipv4Little(167880896) === '192.168.1.10', 'little-endian ipv4');
assert(present('gcode_state', 'RUNNING', false).includes('打印中'), 'running state');
assert(present('mc_print_stage', '1', false).includes('自动调平'), 'stage 1');
assert(present('ams.tray_now', '255', false).includes('无料'), 'empty tray');
assert(present('upload.oss_url', 'http://secret', true) === '已隐藏', 'hidden present');

const samples: Sample[] = [];
pushSample(samples, 10, 0);
assert(seriesPoints(samples) === null, 'one sample is not a trend');
pushSample(samples, 12, 6000);
const points = seriesPoints(samples);
assert(points?.length === 2 && points[1].y < points[0].y, 'two samples draw a rising line');

console.log('web checks ok');
