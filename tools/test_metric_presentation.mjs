import test from 'node:test';
import assert from 'node:assert/strict';
import {fixtureModule,renderCollar,renderHub} from './shared_device_card_fixture.mjs';
const {distanceMetricClass:distance,contactMetricClass:contact}=fixtureModule('@/lib/metricPresentation');
const {gpsFreshness}=fixtureModule('@/lib/gpsFreshness');
const {mapPopupHtml,refreshMapPopupGps}=fixtureModule('@/lib/mapPopup');
const now=Date.parse('2026-10-19T12:00:00Z');
const base={id:3001,name:'Cat',seq:1,time:now/1000,profile:'PowerSave',status:'Out',error:'None',lat:51,lon:-2,hasGps:true,batt:3900,rssi:-90,snr:8,ingestPath:'lora_hub',lastUpdate:now,positionRecordedAt:new Date(now).toISOString(),gnss:{accuracyM:6,satellites:8,fixAgeS:0,recordedAt:new Date(now).toISOString()},homeHub:{lat:51.01,lon:-2,fixAt:new Date(now).toISOString()}};
const avatar={kind:'emoji',emoji:'🐱',color:'#0099ff'};

test('distance/contact thresholds are strict, and invalid/unknown values stay neutral',()=>{
  for(const [value,css] of [[0,''],[600,''],[600.01,'metric-amber'],[1000,'metric-amber'],[1000.01,'metric-red']]) assert.equal(distance(value),css);
  for(const [value,css] of [[0,''],[1800,''],[1801,'metric-amber'],[7200,'metric-amber'],[7201,'metric-red']]) assert.equal(contact(value),css);
  for(const n of [null,undefined,NaN,Infinity,-1]) {assert.equal(distance(n),'');assert.equal(contact(n),'');}
});

test('all five profiles share the eleven-minute display window without changing GPS calculation',()=>{
  for(const profile of ['Normal','PowerSave','Active','Emergency Lost','Debug']) for(const age of [0,60,659,660,661,3600]) {
    const at=new Date(now-age*1000).toISOString();
    const device={...base,profile,gnss:{...base.gnss,recordedAt:at},positionRecordedAt:at};
    for(const expanded of [true,false]) {
      const card=renderCollar({device,now,expanded,ageSeconds:0});
      const popup=mapPopupHtml(device,avatar,now);
      assert.equal(card.includes('gps-age-badge'),age>660);
      if(age<=660) {
        assert.match(card,/card-coords-link gps-recent/);assert.match(card,/gps-as-at gps-recent/);
        assert.match(popup,/gps-age-badge gps-recent" hidden/);assert.match(popup,/gps-as-at gps-recent/);
      } else assert.doesNotMatch(card,/gps-recent/);
      assert.equal(gpsFreshness(device,now).ageSeconds,age);
      assert.ok(card.includes(at),'original GPS timestamp retained');
    }
  }
  assert.match(renderCollar({device:{...base,gnss:null},now,ageSeconds:0}),/gps-age-badge gps-unknown/);
});

test('cards/popups colour real distance and contact, not rounded text or GPS age',()=>{
  const device={...base,lastUpdate:now-7201_000};
  const card=renderCollar({device,now,ageSeconds:7201,distance:'1.1 km'});
  assert.match(card,/card-dist-group metric-red/);assert.match(card,/card-lastseen-group metric-red/);
  assert.match(card,/Dist From Hub<\/span><span class="value metric-red"/);
  assert.doesNotMatch(card,/gps-age-badge/);
  const popup=mapPopupHtml(device,avatar,now);
  assert.match(popup,/card-dist-group metric-red/);assert.match(popup,/card-lastseen-group metric-red/);
  assert.match(popup,/data-last-contact class="value metric-red"/);
  const unknown=renderCollar({device:{...base,homeHub:null},now,ageSeconds:0});
  assert.doesNotMatch(unknown,/card-dist-group metric-(amber|red)/);
  const offline=renderCollar({device,now,ageSeconds:14401});
  assert.match(offline,/metric-red">No reports for/);
  assert.doesNotMatch(renderHub(),/card-lastseen-group metric-(amber|red)/);
});

test('open popup updates contact colours and neutral GPS styling in place',()=>{
  const nodes=Object.fromEntries(['[data-gps-age]','[data-gps-as-at]','[data-gps-coordinates]','[data-last-contact]','.card-lastseen-group','.card-lastseen-value','[data-home-distance]','.card-dist-group','.card-dist-value'].map(k=>[k,{}]));
  const root={querySelector:key=>nodes[key]};
  refreshMapPopupGps(root,base,now);
  assert.equal(nodes['[data-gps-age]'].hidden,true);
  assert.match(nodes['[data-gps-as-at]'].className,/gps-recent/);
  refreshMapPopupGps(root,base,now+1801_000);
  assert.equal(nodes['[data-gps-age]'].hidden,false);
  assert.match(nodes['.card-lastseen-group'].className,/metric-amber/);
  refreshMapPopupGps(root,base,now+7201_000);
  assert.match(nodes['.card-lastseen-group'].className,/metric-red/);
});
