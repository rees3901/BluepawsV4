import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
const context = vm.createContext({performance});
vm.runInContext(readFileSync(new URL('../hub/data/feedback.js', import.meta.url), 'utf8'), context);
const {createStore, receiveWindow, profileLabel} = context.HubFeedback;
const command = (extra = {}) => ({device:1001, cmdSeq:1, type:'profile', profile:'Active', status:'queued', age_ms:0, ...extra});

test('local pending expires at ten minutes and disappears at fifteen', () => {
  let now = 0; const store = createStore(() => now);
  store.accept(command());
  assert.match(store.latest(1001).text, /Command pending: profile → Active/);
  now = 599999; assert.equal(store.latest(1001).pending, true);
  now = 600000; assert.equal(store.latest(1001).status, 'expired');
  now = 900000; assert.equal(store.latest(1001), null);
});
test('ACK wins over late snapshots; feedback is per collar and sequence', () => {
  let now = 1000; const store = createStore(() => now);
  store.accept(command()); now += 100;
  store.accept(command({status:'acked', age_ms:100}));
  store.accept(command({status:'transmitted', age_ms:200}));
  assert.equal(store.latest(1001).status, 'acked');
  assert.equal(store.latest(1002), null);
  store.accept(command({cmdSeq:2, age_ms:0, profile:'Lost Alert'}));
  assert.equal(store.latest(1001).seq, 2);
  assert.match(store.latest(1001).text, /Lost Alert/);
});
test('reconnecting preserves submission age, not a new timeout', () => {
  const store = createStore(() => 1);
  store.accept(command({age_ms:600001}));
  assert.equal(store.latest(1001).status, 'expired');
  assert.equal(store.accept(command({device:0})), false);
});
test('connection reset drops old ACKs before a restarted hub reuses a sequence', () => {
  const store = createStore(() => 10000);
  store.accept(command({status:'acked',age_ms:5000}));
  store.reset();
  assert.equal(store.latest(1001),null);
  store.accept(command());
  assert.equal(store.latest(1001).status,'queued');
});
test('only explicit radio freshness lights the bulb; snapshots cannot extend it', () => {
  const dev = {};
  receiveWindow(dev, {localId:1}, 0); assert.equal(dev.rxUntil, 0);
  receiveWindow(dev, {localId:2,rxWindowMs:10000}, 100); assert.equal(dev.rxUntil, 10100);
  receiveWindow(dev, {localId:2,rxWindowMs:10000}, 5100); assert.equal(dev.rxUntil, 10100);
  receiveWindow(dev, {localId:2,rxWindowMs:0}, 6000); assert.equal(dev.rxUntil, 0);
  receiveWindow(dev, {localId:2,rxWindowMs:10000}, 7000); assert.equal(dev.rxUntil, 0);
  receiveWindow(dev, {localId:3,rxWindowMs:10000}, 8000); assert.equal(dev.rxUntil, 18000);
  assert.equal(profileLabel('Lost Alert'), 'Lost Alert');
});
