import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {fixtureModule} from './shared_device_card_fixture.mjs';
const {resolveMapPosition:resolve,mapPosition:point,mapPositionNotice:notice,parseHomeAnchors}=fixtureModule('@/lib/mapPosition');
const {gnssUncertainty}=fixtureModule('@/lib/gnssUncertainty');
const now=Date.parse('2026-10-19T12:00:00Z');
const fix=new Date(now-3600000).toISOString();
const home={id:3001,name:'Podge',profile:'Normal',status:'Home',bleHome:true,homeBeaconSeenAt:now,lastUpdate:now,
  lat:51.01,lon:-2.01,hasGps:true,positionRecordedAt:fix,gnss:{recordedAt:fix,fixAgeS:0,accuracyM:6},
  homeHub:{id:30,lat:51,lon:-2,fixAt:new Date(now).toISOString()}};

test('Home uses the associated hub, not GPS; OUT without a new fix holds it',()=>{
  const before=JSON.stringify(home);
  const anchor=resolve(home,now);
  assert.equal(anchor.source,'home'); assert.equal(anchor.lat,51); assert.equal(anchor.lon,-2);
  const away={...home,status:'Out',bleHome:false,homeBeaconSeenAt:null,lastUpdate:now+60000};
  const held=resolve(away,now+60000,anchor);
  assert.equal(held.source,'home-stale'); assert.equal(held.lat,51);
  assert.deepEqual(point({...home,mapPosition:anchor}),point({...away,mapPosition:held}));
  const offset=point({...home,mapPosition:anchor});
  assert.ok(Math.abs(offset.lat-51)<.0001); assert.ok(Math.abs(offset.lon+2)<.0001);
  assert.notEqual(offset.lat,51);
  assert.match(notice({...away,mapPosition:held}),/Current position unknown/);
  assert.equal(gnssUncertainty({...away,mapPosition:held}),null);
  assert.equal(JSON.stringify(home),before);
  // A new packet carrying the same old fix is not a new position.
  assert.equal(resolve({...away,positionRecordedAt:new Date(now+60000).toISOString(),
    gnss:{...away.gnss,recordedAt:new Date(now+60000).toISOString(),fixAgeS:3660}},now+60000,held).source,'home-stale');
  const fresh=new Date(now+60000).toISOString();
  assert.equal(resolve({...away,positionRecordedAt:fresh,gnss:{...away.gnss,recordedAt:fresh}},now+60000,held),undefined);
});

test('confirmation expiry, missing fixes, hubs, and invalid hub positions are safe',()=>{
  const anchor=resolve(home,now);
  assert.equal(resolve(home,now+661000,anchor).source,'home-stale');
  assert.equal(resolve({...home,hasGps:false,gnss:null},now).source,'home');
  assert.equal(resolve({...home,entity:'hub'},now,anchor),undefined);
  for(const homeHub of [null,{...home.homeHub,lat:null},{...home.homeHub,lat:NaN},{...home.homeHub,lon:181},{...home.homeHub,fixAt:null}])
    assert.equal(resolve({...home,homeHub},now),undefined);
  assert.equal(resolve({...home,status:'Out',bleHome:false,homeHub:{...home.homeHub,id:31}},now,anchor),undefined);
  // HOME text without current beacon evidence is insufficient.
  assert.equal(resolve({...home,homeBeaconSeenAt:null},now),undefined);
});

test('map badge changes Home → stale → fresh with no duplicate adornment',()=>{
  const require=createRequire(new URL('../web/package.json',import.meta.url));
  const ts=require('typescript');
  let badge;
  const pin={querySelector:()=>badge?{remove(){badge=undefined;}}:null,setAttribute(){},append(value){badge=value;}};
  const context=vm.createContext({exports:{},document:{createElement:()=>({setAttribute(){}})},require:name=>fixtureModule(name==='./mapPosition'?'@/lib/mapPosition':'@/lib/gpsFreshness')});
  vm.runInContext(ts.transpileModule(readFileSync(new URL('../web/src/lib/gpsMarker.ts',import.meta.url),'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const update=context.exports.updateGpsMarker;
  const anchor=resolve(home,now);
  update(pin,{...home,mapPosition:anchor},now);
  assert.equal(badge.textContent,'🏠'); assert.match(badge.className,/marker-home-confirmed/);
  const away={...home,status:'Out',bleHome:false,homeBeaconSeenAt:null};
  update(pin,{...away,mapPosition:resolve(away,now,anchor)},now);
  assert.equal(badge.textContent,'🛰'); assert.match(badge.className,/gps-stale/);
  const stamp=new Date(now+1000).toISOString();
  update(pin,{...away,positionRecordedAt:stamp,gnss:{...away.gnss,recordedAt:stamp}},now+1000);
  assert.equal(badge,undefined);
});

test('session recovery retains last Home without trusting malformed cache or another hub',()=>{
  const anchor=resolve(home,now);
  const recovered=parseHomeAnchors(JSON.stringify({3001:anchor}),now);
  assert.equal(resolve({...home,status:'Out',bleHome:false,homeBeaconSeenAt:null},now,recovered[3001]).source,'home-stale');
  for(const json of ['broken','null','[]',JSON.stringify({3001:{...anchor,lat:999}}),JSON.stringify({3001:{...anchor,confirmedAt:now+1}})])
    assert.equal(Object.keys(parseHomeAnchors(json,now)).length,0);
  assert.equal(resolve({...home,status:'Out',bleHome:false,homeHub:{...home.homeHub,id:99}},now,recovered[3001]),undefined);
});

test('Home estimates never rewrite GPS details or enter the GPS breadcrumb trail',()=>{
  const {mapPopupHtml}=fixtureModule('@/lib/mapPopup');
  const {updateTrailPoints}=fixtureModule('@/lib/trailPoints');
  const device={...home,mapPosition:resolve(home,now),batt:3900,rssi:-90,snr:8,ingestPath:'lora_hub',seq:1,error:'None'};
  const html=mapPopupHtml(device,{kind:'emoji',emoji:'🐱',color:'#0099ff'},now);
  assert.match(html,/shown beside the Home Hub, not a GPS fix/);
  assert.match(html,/51\.01000, -2\.01000/);
  const points=updateTrailPoints([],[],device,now);
  assert.equal(points.length,1); assert.equal(points[0].lat,home.lat); assert.equal(points[0].lon,home.lon);
  assert.equal(points[0].recordedAt,fix);
});
