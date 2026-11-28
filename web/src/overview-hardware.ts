import { num, obj, type Json } from './logic.ts';

type Report = Record<string, Json>;

export function amsEnvironment(record: Report) {
  const temp = num(record.temp);
  const humidity = num(record.humidity_raw);
  const level = num(record.humidity);
  return {
    temperature: temp !== null && temp >= 0 ? temp : null,
    humidity: humidity !== null && humidity >= 0 && humidity <= 100 ? humidity : null,
    level: level !== null && Number.isInteger(level) && level >= 1 && level <= 5 ? level : null,
  };
}

export function toolAddress(nozzle: Report) {
  const address = num(nozzle.addr_id);
  return address !== null && Number.isSafeInteger(address) && address >= 0 && address < 255 ? address : null;
}

export function rackToolLabel(address: number) {
  return `T${String(address + 1).padStart(2, '0')}`;
}

// Bambu Studio's DevNozzleSystem treats id=0x1n as a rack position and
// id=0x0n as an extruder position. addr_id stays with the nozzle when moved.
export function mountedTools(report: Report) {
  const device = obj(report.device);
  const nozzle = obj(device?.nozzle);
  const nozzles = Array.isArray(nozzle?.info) ? nozzle.info : [];
  const extruder = obj(device?.extruder);
  const extruders = Array.isArray(extruder?.info) ? extruder.info : [];
  const exist = num(nozzle?.exist);
  const hasExistBits = exist !== null && Number.isInteger(exist) && exist >= -0x80000000 && exist <= 0xffffffff;
  const installed = new Map<number, boolean>();
  for (const raw of extruders) {
    const item = obj(raw);
    const id = num(item?.id);
    const flags = num(item?.info);
    if (id !== null && flags !== null && Number.isInteger(flags) && flags >= 0) installed.set(id, (flags & 8) !== 0);
  }

  const mounted = new Set<number>();
  let addressedRack = false;
  for (const [index, raw] of nozzles.entries()) {
    const item = obj(raw);
    if (!item) continue;
    const id = num(item.id);
    if (id === null || !Number.isInteger(id) || id < 0 || id >= 0x20) continue;
    addressedRack ||= id >= 0x10 && toolAddress(item) !== null;
    if (id >= 0x10 || (hasExistBits && ((exist >>> id) & 1) === 0)) continue;
    if (installed.get(id) === false) continue;
    mounted.add(index);
  }
  // A reported rack shows where every nozzle is. Without that topology or
  // explicit extruder flags, absence of a mounted reading is not an empty clamp.
  const empty = mounted.size === 0 && ![...installed.values()].includes(true) &&
    (addressedRack || (installed.size > 0 && installed.size === extruders.length));
  return { mounted, empty };
}
