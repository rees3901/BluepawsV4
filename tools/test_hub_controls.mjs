import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
import {handleHubSettings} from '../supabase/functions/ingest-position/hub-presence.ts';
import {hubControlFeedback} from '../web/src/lib/hubControlFeedback.ts';
import {hub,renderHub} from './shared_device_card_fixture.mjs';

const request={format:'hub_settings',ingest_path:'hub_self',gateway_guid16:'0010'};
function database(overrides={}) {
  const calls=[];
  const responses={
    gateway_ingest_credentials:{data:{gateway_guid16:16}},
    gateways:{data:{household_id:'family-a'}},
    hub_presence:{data:{settings_revision:7,desired_ble_enabled:false,display_name:'Hub',
      home_emoji:'🏡',portable_emoji:'📱',marker_colour:'#38bdf8'}},
    ...overrides,
  };
  return {calls,db:{from(table) {
    assert.ok(Object.hasOwn(responses,table));
    const q={select(fields){calls.push([table,'select',fields]);return q;},
      eq(field,value){calls.push([table,field,value]);return q;},
      async maybeSingle(){return responses[table];}};
    return q;
  },rpc(){assert.fail('Settings read must not call ingestion or claim commands');}}};
}
test('settings poll is read-only, gateway-authenticated and Family-scoped',async()=>{
  const {db,calls}=database();
  const response=await handleHubSettings(db,request,'synthetic-only','test');
  assert.equal(response.status,200);
  const result=await response.json();
  assert.equal(result.settings.revision,7);
  assert.equal(result.settings.ble_enabled,false);
  assert.equal(result.received_at,undefined);
  assert.equal(result.accepted,undefined);
  assert(calls.some(c=>c[0]==='gateway_ingest_credentials'&&c[1]==='token_hash'&&/^[a-f0-9]{64}$/.test(c[2])));
  for(const table of ['gateways','gateway_ingest_credentials'])
    assert(calls.some(c=>c[0]===table&&c[1]==='enabled'&&c[2]===true));
  assert(calls.some(c=>c[0]==='hub_presence'&&c[1]==='household_id'&&c[2]==='family-a'));
  assert(calls.some(c=>c[0]==='hub_presence'&&c[1]==='gateway_guid16'&&c[2]===16));
});
test('settings poll rejects wrong credentials, disabled gateways and malformed identities',async()=>{
  for(const [table,response,expected] of [
    ['gateway_ingest_credentials',{data:null},401],['gateways',{data:null},401],
    ['gateways',{data:{household_id:null}},401],
    ['gateway_ingest_credentials',{error:{code:'fail'}},503],
    ['gateways',{error:{code:'fail'}},503],['hub_presence',{error:{code:'fail'}},503],
  ]) {
    const m=database({[table]:response});
    assert.equal((await handleHubSettings(m.db,request,'test','test')).status,expected);
    if(table==='gateway_ingest_credentials') assert(!m.calls.some(c=>c[0]==='hub_presence'));
  }
  for(const patch of [{gateway_guid16:'0000'},{gateway_guid16:'03E9'},{gateway_guid16:16},
    {ingest_path:'lora_hub'},{format:'tlv'}]) {
    const m=database();
    assert.equal((await handleHubSettings(m.db,{...request,...patch},'test','test')).status,400);
    assert.equal(m.calls.length,0);
  }
  const m=database({hub_presence:{data:null}});
  assert.equal((await (await handleHubSettings(m.db,request,'test','test')).json()).settings,null);
});
test('saved is not applied; matching revision AND reported Bluetooth confirm the change',()=>{
  const a={enabled:false,revision:2,startedAt:1000};
  const pending={...hub,settings_revision:2,desired_ble_enabled:false};
  assert.equal(hubControlFeedback(pending,a,2000).state,'pending');
  assert.equal(hubControlFeedback({...pending,applied_revision:2},a,2000).state,'pending');
  assert.equal(hubControlFeedback({...pending,ble_enabled:false},a,2000).state,'pending');
  assert.equal(hubControlFeedback({...pending,ble_enabled:false,applied_revision:2},a,2000).state,'confirmed');
  assert.equal(hubControlFeedback(pending,a,31000).state,'pending');
  assert.match(hubControlFeedback(pending,a,31000).text,/Still waiting/);
  assert.equal(hubControlFeedback(pending,a,91000).state,'failed');
  // Late real acknowledgement replaces the timeout warning; never fake a rollback.
  assert.equal(hubControlFeedback({...pending,ble_enabled:false,applied_revision:2},a,60000).state,'confirmed');
  assert.equal(hubControlFeedback({...pending,settings_revision:3,desired_ble_enabled:true},a,2000).state,'failed');
  assert.equal(hubControlFeedback(pending,null,2000),null);
});
test('cloud hub clears stale Wi-Fi bars at 90 seconds and displays reported, not desired, Bluetooth',()=>{
  assert.match(renderHub({}, {ageSeconds:89}),/Wi-Fi -40 dBm/);
  const stale=renderHub({}, {ageSeconds:90});
  assert.match(stale,/No contact/);
  assert.doesNotMatch(stale,/Wi-Fi -40 dBm/);
  assert.match(stale,/Hub contact lost/);
  assert.match(stale,/disabled="" aria-pressed="true"/);
  const pending=renderHub({desired_ble_enabled:false,settings_revision:2});
  assert.match(pending,/Bluetooth On/);
  assert.match(pending,/not yet confirmed/);
});

test('hub profile confirmation needs reported profile and revision; Power Save changes contact grace',()=>{
  const a={profile:'power_save',revision:3,startedAt:1000};
  const h={...hub,reporting_profile:'normal',desired_reporting_profile:'power_save',settings_revision:3};
  assert.equal(hubControlFeedback({...h,applied_revision:3},a,2000).state,'pending');
  assert.equal(hubControlFeedback({...h,reporting_profile:'power_save'},a,2000).state,'pending');
  assert.equal(hubControlFeedback({...h,reporting_profile:'power_save',applied_revision:3},a,2000).state,'confirmed');
  for(const [profile,grace] of [['power_save',210],['normal',90],['active',60]]) {
    assert.match(renderHub({reporting_profile:profile},{ageSeconds:grace-1}),/Wi-Fi -40 dBm/);
    assert.match(renderHub({reporting_profile:profile},{ageSeconds:grace}),/No contact/);
  }
  const ble={enabled:false,revision:3,startedAt:1000};
  assert.equal(hubControlFeedback({...h,reporting_profile:'power_save'},ble,180000).state,'pending');
  assert.equal(hubControlFeedback({...h,reporting_profile:'power_save'},ble,211000).state,'failed');
});

function localHarness() {
  let state={gateway_guid16:'0010',mode:'home',ble_enabled:true,ble_settled:true};
  let unreachable=false;
  const updates=[],feedbacks=[],timers=new Map(),intervals=[];
  let n=0;
  const ctx=vm.createContext({AbortController,document:{hidden:false,addEventListener(){}},
    setInterval(fn){intervals.push(fn);},setTimeout(fn,ms){timers.set(++n,{fn,ms});return n;},
    clearTimeout(id){timers.delete(id);},fetch:async()=> {
      if(unreachable) throw Error('network lost');
      return {ok:true,json:async()=>({...state})};
    }});
  vm.runInContext(readFileSync(new URL('../hub/data/hub-presence.js',import.meta.url),'utf8'),ctx);
  const panel=ctx.HubPresencePanel;
  panel.start(async(url,options)=>{
    assert.equal(url,'/api/hub-preferences');
    state={...state,...JSON.parse(options.body),ble_settled:false};
    return {ok:true};
  },v=>updates.push(v),id=>feedbacks.push(id));
  return {panel,updates,feedbacks,timers,intervals,
    settle(){state.ble_settled=true;},disconnect(){unreachable=true;}};
}
const drain=()=>new Promise(resolve=>setImmediate(resolve));
test('local API save waits for actual BLE task; status failure never renews contact',async()=>{
  const h=localHarness();await drain();
  h.panel.toggleBluetooth();await drain();
  assert.equal(h.panel.feedback().state,'pending');
  assert(h.feedbacks.every(id=>id===-16));
  h.settle();await h.intervals[0]();await drain();
  assert.equal(h.panel.feedback().state,'confirmed');
  assert.equal(h.updates.at(-1).hub.ble_enabled,false);
  const count=h.updates.length;
  h.disconnect();await h.intervals[0]();
  assert.equal(h.updates.length,count);
});
test('local BLE confirmation times out with an actionable warning',async()=>{
  const h=localHarness();await drain();
  h.panel.toggleBluetooth();await drain();
  h.timers.values().find(t=>t.ms===8000).fn();
  assert.equal(h.panel.feedback().state,'failed');
  assert.match(h.panel.feedback().text,/not confirmed/);
});
