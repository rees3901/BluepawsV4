import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import vm from 'node:vm';
import { renderCollar, fixtureModule } from './shared_device_card_fixture.mjs';

const now = Date.parse('2026-10-15T17:28:04Z');
const device = { id: 3001, name: 'Test collar', profile: 'PowerSave', status: 'Out', error: 'None',
  lat: 51, lon: -2, hasGps: true, batt: 3900, rssi: -90, snr: 8, ingestPath: 'lora_hub',
  lastUpdate: now, positionRecordedAt: '2026-10-15T15:13:55Z',
  gnss: { accuracyM: 6, satellites: 8, fixAgeS: 0, recordedAt: '2026-10-15T15:13:55Z' } };
const avatar = { kind: 'emoji', emoji: '🐱', color: '#0099ff' };

test('expanded and collapsed cards cannot confuse new contact with old GPS', () => {
  for (const expanded of [false, true]) {
    const html = renderCollar({ device, now, expanded, ageSeconds: 0 });
    assert.match(html, /gps-age-badge gps-stale[^>]*>⚠ age 2h 14m/);
    assert.match(html, /Last known coordinates/);
    assert.match(html, /GPS as at/);
    assert.match(html, /datetime="2026-10-15T15:13:55.000Z"/i);
    assert.match(html, /Last contact/);
    assert.match(html, /not a current position/);
    assert.match(html.match(/<div class="card-indicators card-indicators-row3">(.*?)<\/div>/s)[1], /gps-age-badge/);
    assert.doesNotMatch(html, /gps-location-notice/);
    assert.doesNotMatch(html.match(/<details class="gnss-indicator">.*?<\/details>/s)[0], /background:#84cc16/);
  }
});

test('the same popup HTML warns in either renderer, even without a hardware fault', () => {
  const html = fixtureModule('@/lib/mapPopup').mapPopupHtml(device, avatar, now);
  assert.match(html, /GPS 2h 14m old/);
  assert.match(html, /GPS as at/);
  assert.match(html, /Last contact/);
  assert.match(html, /card-coords card-coords-link gps-stale/);
  assert.match(html, /Last known location — not a current position/);
  assert.doesNotMatch(html, /Reported fault/);
  assert.match(html.match(/<div class="card-indicators card-indicators-row3">(.*?)<\/div>/s)[1], /data-gps-age/);
  assert.doesNotMatch(html, /data-gps-notice|gps-location-notice/);
});

test('a fresh fix removes stale notices and restores green GPS information', () => {
  const current = { ...device, gnss: { ...device.gnss, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() };
  const html = renderCollar({ device: current, now, ageSeconds: 0 });
  assert.match(html, /gps-age-badge gps-fresh/);
  assert.doesNotMatch(html, /class="gps-location-notice/);
  assert.match(html, /background:#84cc16/);
});

test('open vector popup updates GPS and contact in place, without replacing actions', () => {
  const nodes = Object.fromEntries(['gps-age', 'gps-as-at', 'gps-coordinates', 'gps-coordinate-label', 'last-contact'].map(key => [`[data-${key}]`, {}]));
  const root = { querySelector: selector => nodes[selector] };
  const refresh = fixtureModule('@/lib/mapPopup').refreshMapPopupGps;
  refresh(root, device, now);
  assert.equal(nodes['[data-gps-age]'].textContent, '⚠ age 2h 14m');
  assert.match(nodes['[data-gps-age]'].title, /GPS 2h 14m old/);
  assert.equal(nodes['[data-last-contact]'].textContent, 'just now');
  assert.match(nodes['[data-gps-as-at]'].title, /not a current position/);
  const stamp = nodes['[data-gps-as-at]'].textContent;
  refresh(root, { ...device, lastUpdate: now + 30_000 }, now + 30_000);
  assert.equal(nodes['[data-last-contact]'].textContent, 'just now');
  assert.equal(nodes['[data-gps-as-at]'].textContent, stamp);
  refresh(root, { ...device, gnss: { ...device.gnss, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() }, now);
  assert.match(nodes['[data-gps-as-at]'].title, /Recent GPS fix/);
  assert.equal(nodes['[data-gps-coordinates]'].className, 'card-coords card-coords-link gps-fresh');
});

test('both map implementations apply the shared age badge without changing pin anchors', () => {
  for (const name of ['TrackingMap', 'MapLibreMap']) {
    const source = readFileSync(new URL(`../web/src/components/${name}.tsx`, import.meta.url), 'utf8');
    assert.match(source, /updateGpsMarker\(/);
  }
  const source = readFileSync(new URL('../web/src/lib/gpsMarker.ts', import.meta.url), 'utf8');
  assert.doesNotMatch(source, /\.style|setLngLat|setLatLng/);
});

test('marker badges never accumulate and disappear on a new fix; the pin is not moved', () => {
  class Element {
    children=[]; attributes={}; className='';
    querySelector() { return this.children.find(e=>e.className.includes('marker-gps-warning')); }
    setAttribute(key,value) { this.attributes[key]=value; }
    append(child) { child.parent=this; this.children.push(child); }
    remove() { this.parent.children=this.parent.children.filter(e=>e!==this); }
  }
  const require=createRequire(new URL('../web/package.json',import.meta.url));
  const ts=require('typescript');
  const source=readFileSync(new URL('../web/src/lib/gpsMarker.ts',import.meta.url),'utf8');
  const context=vm.createContext({exports:{},document:{createElement:()=>new Element()},require:()=>fixtureModule('@/lib/gpsFreshness')});
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const pin=new Element();
  const update=context.exports.updateGpsMarker;
  update(pin,device,now); update(pin,device,now+1000);
  assert.equal(pin.children.length,1);
  assert.equal(pin.children[0].textContent,'🛰');
  assert.match(pin.children[0].className,/marker-gps-warning gps-stale/);
  assert.match(pin.title,/GPS 2h 14m old/);
  assert.match(pin.attributes['aria-label'],/not a current position/);
  const current={...device,positionRecordedAt:new Date(now).toISOString(),gnss:{...device.gnss,recordedAt:new Date(now).toISOString()}};
  update(pin,current,now);
  assert.equal(pin.children.length,0);
});

test('offline cards keep the compact GPS age next to the contact summary', () => {
  const html=renderCollar({device,now:now+4*3600_000,ageSeconds:4*3600});
  assert.match(html.match(/<div class="card-offline-summary card-contact-gps-row">(.*?)<\/div>/s)[1], /gps-age-badge/);
  assert.doesNotMatch(html,/gps-location-notice/);
});

test('GNSS copy is compact while detailed satellite and accuracy explanations remain available', () => {
  const html=renderCollar({device,now,ageSeconds:0});
  const summary=html.match(/<summary class="signal-indicator"[^>]*>(.*?)<\/summary>/s)[1];
  assert.match(summary, /class="sig-label">:8<\/span>/);
  assert.match(summary, />acc:6m<\/span>/);
  assert.doesNotMatch(summary, /Sats:|Est\. acc:/);
  assert.match(html, /satellites: 8/);
  assert.match(html, /Estimated accuracy at the last GPS fix: 6 m/);
  const unknown=renderCollar({device:{...device,gnss:null},now,ageSeconds:0});
  assert.match(unknown, />acc:—<\/span>/);
  assert.match(unknown, />⚠ age —<\/span>/);
});
