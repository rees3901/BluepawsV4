import test from 'node:test';
import assert from 'node:assert/strict';
import { HUB_BLE_POWERS, isHubBlePower, supportsHubBlePower } from './hubBluetooth.ts';

test('five presets retain default and limits and gate additions by reported capability',()=>{
  assert.deepEqual(HUB_BLE_POWERS.map(p=>p.value),[-12,-6,3,6,9]);
  for (const steps of [undefined,null,3]) {
    assert.equal(supportsHubBlePower(-6,steps),false);
    assert.equal(supportsHubBlePower(6,steps),false);
    for (const value of [-12,3,9]) assert.equal(supportsHubBlePower(value,steps),true);
  }
  for (const value of [-12,-6,3,6,9]) assert.equal(supportsHubBlePower(value,5),true);
  for (const value of [0,12,'6',null,NaN]) {
    assert.equal(isHubBlePower(value),false); assert.equal(supportsHubBlePower(value,5),false);
  }
});
