import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {renderHub,renderCollar} from './shared_device_card_fixture.mjs';
import {orderDeviceIds,pinDeviceFirst,moveDeviceBefore} from '../web/src/lib/deviceCardOrder.ts';
import {nextExpandedDeviceCards} from '../web/src/lib/expandedCards.ts';

test('hub and collar share card shell, avatar, expansion and navigation',()=>{
  for (const html of [renderHub(),renderCollar()]) {
    for (const cls of ['device-card expanded','card-summary','card-avatar-wrap','card-reorder-handle',
      'card-pin-button','card-avatar-edit','card-detail-reveal','card-grid','btn-jump','btn-follow','btn-trail'])
      assert.ok(html.includes(cls),cls);
  }
  const html=renderHub();
  assert.match(html,/Wi-Fi -40 dBm/);
  assert.match(html,/Bluetooth On/);
  assert.doesNotMatch(html,/Power Profile|Dist From Hub|btn-cmd|btn-find|battery-indicator|Reported fault|hub-summary|Uptime|Free memory/);
  assert.match(renderCollar(),/Power Profile/);
  assert.match(renderCollar(),/btn-cmd/);
});
test('hub no-fix, collapsed, stale and mode states remain honest',()=>{
  const html=renderHub({latitude:null,longitude:null,fix_at:null});
  assert.match(html,/Waiting for GPS fix/);
  assert.doesNotMatch(html,/google.com\/maps/);
  assert.match(html,/btn-jump" disabled/);
  assert.match(renderHub({}, {expanded:false}),/aria-hidden="true"/);
  assert.match(renderHub({}, {ageSeconds:601}),/device-card stale expanded/);
  assert.match(renderHub({mode:'off_grid'}),/Off-Grid/);
  assert.match(renderHub({home_emoji:'🐈',display_name:'Custom Hub'}),/Custom Hub/);
});
test('hub uses same ordering, pin and four-card expansion rules without ID collisions',()=>{
  const ids=orderDeviceIds([1001,16,-16],[1001,-16]);
  assert.deepEqual(pinDeviceFirst(ids,-16),[-16,1001,16]);
  assert.deepEqual(moveDeviceBefore(ids,-16,1001),[-16,1001,16]);
  assert.deepEqual(nextExpandedDeviceCards([1001,1002,1003,1004],-16),[1002,1003,1004,-16]);
});
test('offline adapter shares renderer but never invents collar telemetry',()=>{
  const context=vm.createContext({});
  vm.runInContext(readFileSync(new URL('../hub/data/hub-presence.js',import.meta.url),'utf8'),context);
  const h=context.HubPresencePanel.view({gateway_guid16:'0010',mode:'portable',latitude:null,longitude:null});
  assert.equal(h.id,-16);assert.equal(h.emoji,'📱');assert.equal(h.hasGps,false);
  for (const key of ['profile','batt','rxWindowMs','errorPresent','verification']) assert.equal(h[key],undefined);
  const js=readFileSync(new URL('../hub/data/app.js',import.meta.url),'utf8');
  assert.match(js,/HubPresencePanel.start\(protectedFetch/);
  assert.match(js,/updateDevice\(data\); \/\/ Shared cards/);
  assert.doesNotMatch(js,/hub-map-icon|HubPresencePanel.point/);
});
