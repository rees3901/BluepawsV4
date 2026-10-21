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
    assert.match(html, /gps-age-badge gps-stale[^>]*>⚠ GPS age: 2h 14 min old/);
    assert.match(html, /Last known coordinates/);
    assert.match(html, /GPS as at/);
    assert.match(html, /datetime="2026-10-15T15:13:55.000Z"/i);
    assert.match(html, /Last contact/);
    assert.match(html, /not a current position/);
    assert.doesNotMatch(html.match(/<div class="card-indicators card-indicators-primary">(.*?)<\/div>/s)[1], /gps-age-badge/);
    assert.match(html.match(/<div class="card-indicators card-indicators-row3">(.*?)<\/div>/s)[1], /gps-age-badge/);
    assert.doesNotMatch(html, /gps-location-notice/);
    assert.doesNotMatch(html.match(/<details class="gnss-indicator">.*?<\/details>/s)[0], /background:#84cc16/);
  }
});

test('the same popup HTML warns in either renderer, even without a hardware fault', () => {
  const html = fixtureModule('@/lib/mapPopup').mapPopupHtml(device, avatar, now);
  assert.match(html, /GPS 2h 14 min old/);
  assert.match(html, /GPS as at/);
  assert.match(html, /Last contact/);
  assert.match(html, /card-coords card-coords-link gps-stale/);
  assert.match(html, /Last known location — not a current position/);
  assert.doesNotMatch(html, /Reported fault/);
  assert.match(html.match(/<div class="card-indicators card-indicators-row3">(.*?)<\/div>/s)[1], /data-gps-age/);
  assert.doesNotMatch(html, /data-gps-notice|gps-location-notice/);
});

test('a recent fix hides age clutter and uses neutral coordinate/timestamp text', () => {
  const current = { ...device, gnss: { ...device.gnss, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() };
  const html = renderCollar({ device: current, now, ageSeconds: 0 });
  assert.doesNotMatch(html, /gps-age-badge/);
  assert.match(html, /card-coords card-coords-link gps-recent/);
  assert.match(html, /value gps-as-at gps-recent/);
  assert.doesNotMatch(html, /class="gps-location-notice/);
  assert.match(html, /background:#84cc16/);
});

test('on-schedule Power Save and Home presence never relabel old coordinates as current', () => {
  const at = new Date(now - 20 * 60_000).toISOString();
  const scheduled = { ...device, gnss: { ...device.gnss, recordedAt: at }, positionRecordedAt: at };
  const home = { ...device, status: 'Home', bleHome: true, homeBeaconSeenAt: now, lastReportReason: 7 };
  for (const collar of [scheduled, home]) {
    const card = renderCollar({ device: collar, now, ageSeconds: 0 });
    const popup = fixtureModule('@/lib/mapPopup').mapPopupHtml(collar, avatar, now);
    for (const html of [card, popup]) {
      if (collar !== home) assert.match(html, /gps-age-badge gps-fresh/);
      assert.match(html, /Last known coordinates/);
      assert.match(html, /not a current position/);
      assert.doesNotMatch(html, /Recent GPS fix/);
      assert.match(html, /GPS as at/);
    }
    if (collar !== home) assert.match(card, /GPS age: 20 min old/);
    if (collar === home) {
      assert.match(card, /class="card-status status-home" title="The collar recently detected/);
      assert.doesNotMatch(card, /home-beacon-confirmed|>Home confirmed</);
      assert.doesNotMatch(card, /gps-age-badge/);
      assert.match(popup, /gps-age-badge gps-aging" hidden/);
    }
  }
});

test('recent Home beacon confirmation stays independent when GPS hits the four-hour red cap', () => {
  const at = new Date(now - 4 * 3600_000).toISOString();
  const home = { ...device, status: 'Home', bleHome: true, homeBeaconSeenAt: now,
    lastReportReason: 7, positionRecordedAt: at, gnss: { ...device.gnss, recordedAt: at } };
  for (const html of [renderCollar({device:home,now,ageSeconds:0}), fixtureModule('@/lib/mapPopup').mapPopupHtml(home,avatar,now)]) {
    assert.match(html, /class="card-status status-home"[^>]*title="The collar recently detected/);
    assert.doesNotMatch(html, /home-beacon-confirmed|>Home confirmed</);
    assert.doesNotMatch(html, /gps-age-badge gps-stale"(?! hidden)/);
    assert.match(html, /gps-as-at gps-stale/);
    assert.match(html, /Last known coordinates/);
    assert.match(html, /not a current position/);
  }
});

test('open popup independently expires Home confirmation and updates GPS age without replacing actions', () => {
  const nodes = Object.fromEntries(['home-beacon','gps-age','gps-as-at','gps-coordinates','gps-coordinate-label','last-contact'].map(key => [`[data-${key}]`, {}]));
  const root = { querySelector: selector => nodes[selector] };
  const at = new Date(now - 4 * 3600_000).toISOString();
  const home = {...device,status:'Home',bleHome:true,homeBeaconSeenAt:now,lastReportReason:7,
    positionRecordedAt:at,gnss:{...device.gnss,recordedAt:at}};
  const refresh = fixtureModule('@/lib/mapPopup').refreshMapPopupGps;
  refresh(root,home,now);
  assert.match(nodes['[data-home-beacon]'].title,/recently detected/);
  assert.equal(nodes['[data-gps-age]'].textContent,'⚠ GPS age: 4h 0 min old');
  assert.equal(nodes['[data-gps-age]'].hidden,true);
  assert.match(nodes['[data-gps-age]'].className,/gps-stale/);
  refresh(root,home,now+1861_000);
  assert.equal(nodes['[data-gps-age]'].hidden,false);
  assert.match(nodes['[data-home-beacon]'].title,/recent Home beacon confirmation is unavailable/);
  assert.match(nodes['[data-gps-age]'].className,/gps-stale/);
  assert.equal(nodes['[data-gps-coordinate-label]'].textContent,'Last known coordinates');
});

test('compact age follows confirmed Home evidence across profiles without altering expanded GPS time', () => {
  const { gpsReportingSchedule } = fixtureModule('@/lib/gpsReporting');
  const { gpsFreshness } = fixtureModule('@/lib/gpsFreshness');
  for (const profile of ['PowerSave','Normal','Active','Debug']) {
    const home = {...device,profile,status:'Home',bleHome:true,homeBeaconSeenAt:now,lastReportReason:7};
    const expires = now + gpsReportingSchedule(profile).contactWindowSeconds * 1000;
    for (const expanded of [false,true]) for (const awakeSeconds of [0,8]) {
      for (const at of [now,expires]) {
        const html = renderCollar({device:home,now:at,expanded,awakeSeconds,ageSeconds:0});
        assert.doesNotMatch(html,/gps-age-badge/);
        assert.match(html,/GPS as at/);
        assert.match(html,/datetime="2026-10-15T15:13:55.000Z"/i);
      }
      for (const [collar,at] of [[home,expires+1000],[{...home,status:'Out'},now],[{...home,bleHome:false},now],[{...home,homeBeaconSeenAt:undefined},now]]) {
        assert.match(renderCollar({device:collar,now:at,expanded,awakeSeconds,ageSeconds:0}),/gps-age-badge/);
        assert.equal(gpsFreshness(collar,at).fixAt,gpsFreshness(home,now).fixAt);
      }
    }
  }
  const lost = {...device,profile:'Emergency Lost',status:'Home',bleHome:true,homeBeaconSeenAt:now};
  assert.match(renderCollar({device:lost,now,ageSeconds:0}),/gps-age-badge/);
  const nodes = Object.fromEntries(['gps-age','gps-as-at'].map(key => [`[data-${key}]`,{}]));
  const root = {querySelector:selector=>nodes[selector]};
  const home = {...device,status:'Home',bleHome:true,homeBeaconSeenAt:now};
  const refresh = fixtureModule('@/lib/mapPopup').refreshMapPopupGps;
  refresh(root,home,now);
  const stamp = nodes['[data-gps-as-at]'].textContent;
  assert.equal(nodes['[data-gps-age]'].hidden,true);
  refresh(root,{...home,status:'Out'},now);
  assert.equal(nodes['[data-gps-age]'].hidden,false);
  assert.equal(nodes['[data-gps-as-at]'].textContent,stamp);
  refresh(root,home,now);
  assert.equal(nodes['[data-gps-age]'].hidden,true);
});

test('map GPS adornment starts only after eleven minutes, independently of card/profile warning bands', () => {
  let badge;
  const require = createRequire(new URL('../web/package.json', import.meta.url));
  const ts = require('typescript');
  const source = readFileSync(new URL('../web/src/lib/gpsMarker.ts', import.meta.url), 'utf8');
  const context = vm.createContext({ exports: {}, document: { createElement: () => ({ setAttribute() {} }) }, require: name => fixtureModule(name === './mapPosition' ? '@/lib/mapPosition' : '@/lib/gpsFreshness') });
  vm.runInContext(ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS } }).outputText, context);
  const pin = { querySelector() {}, setAttribute() {}, append(value) { badge = value; } };
  const update=context.exports.updateGpsMarker;
  for(const profile of ['PowerSave','Normal','Active','Emergency Lost','Debug']) {
    for(const age of [0,59,60,300,599,600,601,659,660,661,1200]) {
      badge=undefined;
      const at=new Date(now-age*1000).toISOString();
      const collar={...device,profile,positionRecordedAt:at,gnss:{...device.gnss,recordedAt:at}};
      update(pin,collar,now);
      assert.equal(!!badge,age>660,`${profile}, age ${age}s`);
      if(age>=60) assert.match(pin.title,/not a current position/);
    }
  }
  // Explicit packet failures still warn on cards, but don't bypass map decluttering.
  const at=new Date(now-60_000).toISOString();
  badge=undefined;
  update(pin,{...device,gpsFailedReports:3,positionRecordedAt:at,gnss:{...device.gnss,recordedAt:at}},now);
  assert.equal(badge,undefined);
  assert.match(pin.title,/not a current position/);
  for(const gnss of [null,{...device.gnss,fixAgeS:65535}]) {
    badge=undefined;
    update(pin,{...device,gnss},now);
    assert.ok(badge,'unknown GPS age remains visible');
  }
  badge=undefined;
  update(pin,{...device,entity:'hub',gnss:null},now);
  assert.equal(badge,undefined);
});

test('recent-fix quiet display does not reset underlying failed GPS report warnings', () => {
  const at = new Date(now - 60_000).toISOString();
  for (const [gpsFailedReports, css] of [[1,'gps-aging'],[3,'gps-stale']]) {
    const collar = { ...device, positionRecordedAt: at, gnss: { ...device.gnss, recordedAt: at },
      lastReportReason: 7, gpsFailedReports };
    assert.equal(fixtureModule('@/lib/gpsFreshness').gpsFreshness(collar,now).className,css);
    assert.doesNotMatch(renderCollar({ device: collar, now, ageSeconds: 0 }), /gps-age-badge/);
    assert.match(fixtureModule('@/lib/mapPopup').mapPopupHtml(collar, avatar, now), /gps-age-badge gps-recent" hidden/);
  }
});

test('open vector popup updates GPS and contact in place, without replacing actions', () => {
  const nodes = Object.fromEntries(['gps-age', 'gps-as-at', 'gps-coordinates', 'gps-coordinate-label', 'last-contact'].map(key => [`[data-${key}]`, {}]));
  const root = { querySelector: selector => nodes[selector] };
  const refresh = fixtureModule('@/lib/mapPopup').refreshMapPopupGps;
  refresh(root, device, now);
  assert.equal(nodes['[data-gps-age]'].textContent, '⚠ GPS age: 2h 14 min old');
  assert.match(nodes['[data-gps-age]'].title, /GPS 2h 14 min old/);
  assert.equal(nodes['[data-last-contact]'].textContent, 'just now');
  assert.match(nodes['[data-gps-as-at]'].title, /not a current position/);
  const stamp = nodes['[data-gps-as-at]'].textContent;
  refresh(root, { ...device, lastUpdate: now + 30_000 }, now + 30_000);
  assert.equal(nodes['[data-last-contact]'].textContent, 'just now');
  assert.equal(nodes['[data-gps-as-at]'].textContent, stamp);
  refresh(root, { ...device, gnss: { ...device.gnss, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() }, now);
  assert.match(nodes['[data-gps-as-at]'].title, /Recent GPS fix/);
  assert.equal(nodes['[data-gps-coordinates]'].className, 'card-coords card-coords-link gps-recent');
  assert.equal(nodes['[data-gps-age]'].hidden, true);
  refresh(root, { ...device, gnss: { ...device.gnss, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() }, now+661_000);
  assert.equal(nodes['[data-gps-age]'].hidden, false);
});

test('collar GPS age follows distance, contact and awake state on the lower metrics row', () => {
  for (const expanded of [true, false]) for (const awakeSeconds of [0, 8]) for (const portableMode of [true, false]) {
    const html = renderCollar({device, now, expanded, ageSeconds:0, awakeSeconds, portableMode});
    const primary = html.match(/class="card-indicators card-indicators-primary">(.*?)<div class="card-indicators card-indicators-row3">/s)[1];
    const lower = html.match(/class="card-indicators card-indicators-row3">(.*?)<\/div>/s)[1];
    assert.doesNotMatch(primary, /gps-age-badge/);
    assert.match(lower, /card-dist-group.*card-lastseen-group.*collar-awake.*gps-age-badge/s);
    assert.match(lower, awakeSeconds ? /💡/ : /💤/);
    assert.match(lower, /⚠ GPS age: 2h 14 min old/);
  }
});

test('mobile wrapped telemetry is not a scroll container and closed GNSS details take no space', () => {
  const css = readFileSync(new URL('../web/src/app/web.css', import.meta.url), 'utf8');
  assert.match(css, /#deviceCards \.card-indicators-primary \{ overflow: visible; max-width: 100%; \}/);
  assert.doesNotMatch(css, /#deviceCards \.card-indicators-primary \{ overflow-x: auto/);
  assert.match(css, /\.gnss-indicator:not\(\[open\]\) > \.gnss-detail \{ display: none; \}/);
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
  const context=vm.createContext({exports:{},document:{createElement:()=>new Element()},require:name=>fixtureModule(name === './mapPosition' ? '@/lib/mapPosition' : '@/lib/gpsFreshness')});
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const pin=new Element();
  const update=context.exports.updateGpsMarker;
  update(pin,device,now); update(pin,device,now+1000);
  assert.equal(pin.children.length,1);
  assert.equal(pin.children[0].textContent,'🛰');
  assert.match(pin.children[0].className,/marker-gps-warning gps-stale/);
  assert.match(pin.title,/GPS 2h 14 min old/);
  assert.match(pin.attributes['aria-label'],/not a current position/);
  const current={...device,positionRecordedAt:new Date(now).toISOString(),gnss:{...device.gnss,recordedAt:new Date(now).toISOString()}};
  update(pin,current,now);
  assert.equal(pin.children.length,0);
  // The clock alone adds the badge after eleven minutes, without a new report.
  update(pin,current,now+600_000);
  assert.equal(pin.children.length,0);
  update(pin,current,now+601_000);
  assert.equal(pin.children.length,0);
  update(pin,current,now+660_000);
  assert.equal(pin.children.length,0);
  update(pin,current,now+661_000);
  assert.equal(pin.children.length,1);
  const fresh={...device,positionRecordedAt:new Date(now+661_000).toISOString(),gnss:{...device.gnss,recordedAt:new Date(now+661_000).toISOString()}};
  update(pin,fresh,now+661_000);
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
  assert.match(summary, /class="sig-label gnss-count">x8<\/span>/);
  assert.match(summary, /class="gnss-satellites"><span class="gnss-emoji"[^>]*>🛰️<\/span><span class="sig-label gnss-count">x8<\/span><\/span>/);
  const css = readFileSync(new URL('../web/src/app/web.css', import.meta.url), 'utf8');
  assert.match(css, /\.gnss-satellites \.gnss-count \{ margin-left: 0; \}/);
  assert.match(summary, />acc:6m<\/span>/);
  assert.doesNotMatch(summary, /Sats:|Est\. acc:/);
  assert.match(html, /satellites: 8/);
  assert.match(html, /Estimated accuracy at the last GPS fix: 6 m/);
  const unknown=renderCollar({device:{...device,gnss:null},now,ageSeconds:0});
  assert.match(unknown, />acc:—<\/span>/);
  assert.match(unknown, />⚠ GPS age: —<\/span>/);
});
