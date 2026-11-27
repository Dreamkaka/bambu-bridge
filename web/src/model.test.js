import assert from 'node:assert/strict';
import test from 'node:test';
import {
  actionGates, applyEvent, asNumber, canCommand, createModel, displayJob, formatPercent,
  heatGauge, markUnconfirmed, mergeReport, noticeText, recordTrace, validDeviceId, validIpv4, validPassword,
} from './model.js';

test('incremental reports keep earlier readings and confirm only the source that arrived', () => {
  const model = createModel();
  assert.equal(applyEvent(model, {
    event: 'printer_status',
    payload: { dev_id: 'online', source: 'cloud', report: { command: 'push_status', nozzle_temper: 215, mc_percent: 40 } },
  }), 'online 经云端上报了新的状态');
  applyEvent(model, {
    event: 'printer_status',
    payload: { dev_id: 'online', source: 'cloud', report: { command: 'push_status', gcode_state: 'PAUSE' } },
  });
  const slot = model.reports.online.cloud;
  assert.equal(slot.confirmed, true);
  assert.equal(slot.data.nozzle_temper, 215);
  assert.equal(slot.data.gcode_state, 'PAUSE');
  assert.equal(model.traces.online.length, 1);
  assert.equal(model.traces.online[0].nozzle, 215);
  assert.equal(model.traces.online[0].percent, 40);
  assert.equal(model.traces.online[0].bed, null);
  markUnconfirmed(model);
  assert.equal(model.reports.online.cloud.confirmed, false);
  const job = displayJob(slot.data);
  assert.equal(job.nozzle, '215°C');
  assert.equal(job.stateLabel, '已暂停');
  assert.equal(job.percent.text, '40%');
  assert.equal(actionGates(model.reports.online.cloud).resume, true);
  model.reports.online.cloud.confirmed = true;
  assert.deepEqual(actionGates(model.reports.online.cloud), { pause: false, resume: true, stop: true });
});

test('merge replaces nested objects one level at a time and drops prototype keys', () => {
  const prev = { ams: { humidity: '4', ams: [{ id: '0' }] }, nozzle_temper: 200 };
  const next = mergeReport(prev, JSON.parse('{"__proto__":{"polluted":true},"ams":{"humidity":"9"},"nozzle_temper":180}'));
  assert.equal(next.nozzle_temper, 180);
  assert.equal(next.ams.humidity, '9');
  assert.equal(next.ams.ams[0].id, '0');
  assert.equal(prev.nozzle_temper, 200);
  assert.equal(Object.hasOwn(next, '__proto__'), false);
  assert.equal(Object.prototype.polluted, undefined);
});

test('missing and non-numeric readings stay empty', () => {
  const job = displayJob({ nozzle_temper: 'hot', mc_percent: null, print_error: 0, mc_remaining_time: 65 });
  assert.equal(job.nozzle, null);
  assert.equal(job.percent, null);
  assert.equal(job.nozzleValue, null);
  assert.equal(job.error, null);
  assert.deepEqual(job.remainParts, { big: '1', unit: '小时', sub: '5 分钟' });
  assert.equal(job.remain, '1 小时 5 分钟');
  assert.equal(displayJob(null), null);
  assert.equal(formatPercent('1e2'), null);
  assert.equal(asNumber('215.5'), 215.5);
});

test('commands follow login, lan link, and confirmed motion state', () => {
  const model = createModel();
  model.selectedId = 'online';
  model.status = { agent_ready: true, logged_in: false };
  assert.equal(canCommand(model), false);
  model.status.logged_in = true;
  assert.equal(canCommand(model), true);
  model.source = 'lan';
  assert.equal(canCommand(model), false);
  model.lanLink = { devId: 'online', status: 0 };
  assert.equal(canCommand(model), true);
  assert.deepEqual(actionGates({ confirmed: true, data: { gcode_state: 'IDLE' } }), {
    pause: false, resume: false, stop: false,
  });
  assert.equal(actionGates({ confirmed: false, data: { gcode_state: 'IDLE' } }).pause, true);
});

test('device, address, and access code match the web API limits', () => {
  assert.equal(validDeviceId('lan-fixture'), true);
  assert.equal(validDeviceId('bad/id'), false);
  assert.equal(validDeviceId('a'.repeat(65)), false);
  assert.equal(validIpv4('192.0.2.10'), true);
  assert.equal(validIpv4('192.168.001.1'), false);
  assert.equal(validIpv4('10.0.0.256'), false);
  assert.equal(validPassword('fixture-pass'), true);
  assert.equal(validPassword(''), false);
  assert.equal(validPassword('字'), false);
  assert.equal(validPassword('a'.repeat(129)), false);
});

test('socket events and notices use only fields the bridge sends', () => {
  const model = createModel();
  model.status = { logged_in: false, agent_ready: true };
  model.ws = 'open';
  model.wsEverOpen = true;
  assert.match(noticeText(model).text, /还没有登录/);
  model.fault = { text: '插件没有接受这次请求。' };
  assert.equal(noticeText(model).text, '插件没有接受这次请求。');
  assert.equal(applyEvent(model, { event: 'login_result', payload: { success: false } }), '登录没有完成');
  assert.equal(applyEvent(model, { event: 'cloud_connection', payload: { return_code: 5, reason_code: 0 } }), '云端连接回报 5，登录已过期');
  assert.equal(applyEvent(model, { event: 'lan_connection', payload: { dev_id: 'lan-fixture', status: 0 } }), '局域网 lan-fixture 已连接');
  assert.equal(applyEvent(model, { event: 'ready', payload: {} }), null);
  assert.equal(model.lanLink.status, 0);
});

test('history keeps changes, repeats a steady reading after a gap, and caps the series', () => {
  const model = createModel();
  assert.equal(recordTrace(model, 'bad/id', { nozzle_temper: 1 }, 0), false);
  assert.equal(recordTrace(model, 'online', { nozzle_temper: 'hot', mc_percent: null }, 0), false);
  assert.equal(recordTrace(model, 'online', { nozzle_temper: 200, bed_temper: 40, mc_percent: 10 }, 1000), true);
  assert.equal(recordTrace(model, 'online', { nozzle_temper: 200, bed_temper: 40, mc_percent: 10 }, 10000), false);
  assert.equal(recordTrace(model, 'online', { nozzle_temper: 210, bed_temper: 40, mc_percent: 12 }, 11000), true);
  assert.equal(recordTrace(model, 'online', { nozzle_temper: 210, bed_temper: 40, mc_percent: 12 }, 30000), true);
  assert.equal(model.traces.online.length, 3);
  assert.equal(model.traces.online[2].nozzle, 210);
  assert.equal(model.traces.online[2].percent, 12);
  assert.equal(heatGauge(180, 200).alert, false);
  assert.equal(heatGauge(180, 200).share, 0.9);
  assert.equal(heatGauge(230, 200).alert, true);
  assert.equal(heatGauge(230, 200).share, 1);
  assert.equal(heatGauge(40, 0), null);
  const many = createModel();
  for (let i = 0; i < 60; i += 1) recordTrace(many, 'online', { mc_percent: i }, i * 20000);
  assert.equal(many.traces.online.length, 48);
  assert.equal(many.traces.online[0].percent, 12);
  assert.equal(many.traces.online.at(-1).percent, 59);
});
