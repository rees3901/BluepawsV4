import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import vm from 'node:vm';
import {normalizeDeviceName} from '../web/src/lib/deviceName.ts';
import * as rows from '../web/src/lib/telemetryRows.ts';
import {renderCollar} from './shared_device_card_fixture.mjs';
const require=createRequire(new URL('../web/package.json',import.meta.url));
const ts=require('typescript');
function load(path,imports,globals={}) {
  const context=vm.createContext({exports:{},crypto:{randomUUID:()=> 'photo-uuid'},...globals,
    require(name){if(name in imports)return imports[name];throw Error('Unexpected import '+name);}});
  const source=readFileSync(new URL('../web/src/lib/'+path,import.meta.url),'utf8');
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  return context.exports;
}
test('collar name and appearance use one RPC; failed saves clean up only the new photo',async()=>{
  for(const fail of [false,true]){
    const calls=[];
    const db={rpc:async(name,args)=>{calls.push(['rpc',name,args]);return {error:fail?Error('Denied'):null};},
      storage:{from(bucket){return {upload:async(path)=>{calls.push(['upload',bucket,path]);return {error:null};},
        remove:async(paths)=>{calls.push(['remove',...paths]);return {error:null};}};}}};
    const {saveDeviceAppearance:save}=load('deviceAppearances.ts',{'@/lib/supabase/client':{createClient:()=>db},'./deviceName':{normalizeDeviceName}});
    const input={deviceId:1001,householdId:'family',name:' Mittens ',kind:'photo',emoji:'🐱',color:'#0099ff',
      previousStoragePath:'family/1001/old.webp',preparedPhoto:{}};
    if(fail)await assert.rejects(()=>save(input),/Denied/);else await save(input);
    const rpc=calls.find(c=>c[0]==='rpc');
    assert.equal(rpc[1],'bluepaws_save_device_marker');assert.equal(rpc[2].requested_name,'Mittens');
    assert.equal(rpc[2].requested_device_id,1001);assert.equal(rpc[2].requested_household_id,'family');
    assert.equal(calls.at(-1)[1],fail?'family/1001/photo-uuid.webp':'family/1001/old.webp');
    const count=calls.length;await assert.rejects(()=>save({...input,name:' '}));assert.equal(calls.length,count);
  }
});
test('realtime positions, rename broadcasts and refresh preserve names and last-seen',async()=>{
  const callbacks={},listeners={};let latest,reads=0;
  const position={position_id:1,device_uid:1001,household_id:'family',message_id:1,latitude:51,longitude:-2,
    battery:null,battery_mv:3900,status_code:0,power_profile_code:1,flags:1,tx_reason:0,
    ingest_path:'lora_hub',link_type:'lora',link_rssi_dbm:-94,link_snr_db:8,source:'tlv',
    recorded_at:'2026-08-27T12:00:00Z',received_at:'2026-08-27T12:00:01Z',schema_version:2};
  const presence={device_id:1001,household_id:'family',display_name:'Mittens',last_seen_at:position.recorded_at,
    last_seen_status_code:0,last_seen_power_profile_code:1,last_seen_tx_reason:0,last_seen_battery_mv:3900};
  const channel={on(_type,filter,fn){callbacks[filter.event]=fn;return channel;},subscribe(){return channel;}};
  const db={from(table){return {select(){return {eq:async()=>{reads++;return {data:table==='devices'?[presence]:[position],error:null};}};}};},
    realtime:{setAuth:async()=>{}},channel:()=>channel,removeChannel:async()=>{}};
  const win={addEventListener:(name,fn)=>{listeners[name]=fn;},removeEventListener:(name)=>{delete listeners[name];},clearTimeout(){},setTimeout(){return 1;}};
  const doc={visibilityState:'visible',addEventListener(){},removeEventListener(){}};
  const {createRealtimeTelemetrySource}=load('realtimeTelemetry.ts',{'@/lib/supabase/client':{createClient:()=>db},
    '@/lib/telemetryRows':rows,'@/lib/familyRealtime':{familyRealtimeTopic:()=> 'family',nextFamilyAccessVersion:()=> null},
    '@/lib/trailPoints':{VISIBLE_TRAIL_POINT_LIMIT:4}},{window:win,document:doc});
  const stop=createRealtimeTelemetrySource('family',1,[]).subscribe(ds=>{latest=ds;});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(latest[0].name,'Mittens');assert.equal(reads,2);
  callbacks.INSERT({payload:{record:{...position,message_id:2,recorded_at:'2026-08-27T12:01:00Z'}}});
  assert.equal(latest[0].name,'Mittens');
  const activity=latest[0].lastUpdate;
  callbacks.DEVICE_PRESENCE({payload:{record:{...presence,display_name:'Podge'}}});
  assert.equal(latest[0].name,'Podge');assert.equal(latest[0].lastUpdate,activity);
  listeners['bluepaws:device-renamed']({detail:{householdId:'other',deviceId:1001,name:'Wrong'}});
  assert.equal(latest[0].name,'Podge');
  listeners['bluepaws:device-renamed']({detail:{householdId:'family',deviceId:1001,name:'Mittens II'}});
  callbacks.UPDATE({payload:{record:{...position,message_id:3,recorded_at:'2026-08-27T12:02:00Z'}}});
  assert.equal(latest[0].name,'Mittens II');
  stop();assert.equal(listeners['bluepaws:device-renamed'],undefined);
});
test('card shows friendly name prominently and unchanged ID in details',()=>{
  const html=renderCollar();
  assert.match(html,/>Podge<\/span>/);
  assert.match(html,/>Device ID<\/span><span class="value">1001<\/span>/);
  assert.doesNotMatch(html,/card-name">Device 1001/);
});
