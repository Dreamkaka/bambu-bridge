import {
  flatten,
  ipv4Little,
  mergeJson,
  present,
  pushSample,
  seriesPoints,
  type Sample,
} from './logic.ts';
import { amsEnvironment, mountedTools, rackToolLabel, toolAddress } from './overview-hardware.ts';

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

const env = amsEnvironment({ temp: '26.5', humidity: '4', humidity_raw: '45' });
assert(env.temperature === 26.5 && env.humidity === 45, 'AMS displays temperature and measured RH, not the grade as a percent');
const legacyEnv = amsEnvironment({ temp: -1, humidity: 3, humidity_raw: -1 });
assert(legacyEnv.temperature === null && legacyEnv.humidity === null && legacyEnv.level === 3, 'invalid AMS readings fall back to humidity grade');
assert(amsEnvironment({ humidity_raw: 0 }).humidity === 0, 'zero RH is a valid reading');

const picked = { id: 0, addr_id: 2 };
const racked = { id: 0x10, addr_id: 0 };
const holding = mountedTools({ device: {
  nozzle: { exist: (1 << 16) | 1, info: [racked, picked], src_id: 0x13, tar_id: 0x10 },
  extruder: { info: [{ id: 0, hnow: 0, info: 8 }] },
} });
const address = toolAddress(picked);
assert(holding.mounted.size === 1 && holding.mounted.has(1) && address !== null && rackToolLabel(address) === 'T03', 'mounted position and rack address are different identifiers');
const returned = mountedTools({ device: {
  nozzle: { info: [racked, { id: 0x12, addr_id: 2 }], tar_id: 0x12 },
  extruder: { info: [{ id: 0, hnow: 0, info: 0 }] },
} });
assert(returned.mounted.size === 0 && returned.empty, 'a rack target is not a mounted nozzle');
const blocked = mountedTools({ device: { nozzle: { info: [picked], exist: 0 }, extruder: { info: [{ id: 0, info: 8 }] } } });
assert(blocked.mounted.size === 0 && !blocked.empty, 'exist mask rejects a stale mounted position without falsely declaring the clamp empty');
assert(!mountedTools({ nozzle_temper: 20 }).empty, 'legacy temperature alone does not indicate an empty clamp');

console.log('web checks ok');
