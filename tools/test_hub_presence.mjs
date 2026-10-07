// No network/hardware: test the real Edge handler and map adapter.
import test from 'node:test';
import assert from 'node:assert/strict';
import {parseHubPresence,handleHubPresence} from '../supabase/functions/ingest-position/hub-presence.ts';
import {hubAvatar,hubMapDevice} from '../web/src/lib/hubPresence.ts';
const payload={format:'hub_status',ingest_path:'hub_self',gateway_guid16:'0010',mode:'home',
  latitude:51.9,longitude:-2.2,fix_age_s:5,uptime_s:100,wifi_rssi_dbm:-40,
  ble_enabled:true,ble_advertising:true,free_heap:150000,applied_revision:0,
  battery_percent:92,position_simulated:true,battery_simulated:true};
test('hub report validates identity, position, bounds and separate transport',()=>{
  assert.equal(parseHubPresence(payload).p_gateway,16);
  assert.equal(parseHubPresence(payload).p_reporting_profile,'normal','legacy cadence remains honest');
  assert.equal(parseHubPresence(payload).p_control_poll_s,null);
  assert.equal(parseHubPresence(payload).p_battery_percent,92);
  assert.equal(parseHubPresence(payload).p_position_simulated,true);
  assert.equal(parseHubPresence(payload).p_battery_simulated,true);
  for(const profile of ['normal','power_save','active']) {
    const parsed=parseHubPresence({...payload,reporting_profile:profile,control_poll_s:5});
    assert.equal(parsed.p_reporting_profile,profile); assert.equal(parsed.p_control_poll_s,5);
  }
  assert.equal(parseHubPresence({...payload,latitude:null,longitude:null,position_simulated:false}).p_lat,null);
  for(const change of [{gateway_guid16:'03E9'},{gateway_guid16:'0000'},{ingest_path:'lora_hub'},
    {latitude:NaN},{latitude:null},{longitude:181},{fix_age_s:-1},{free_heap:-1},
    {ble_enabled:'true'},{mode:'bad'},{applied_revision:Infinity},
    {reporting_profile:'lost_alert'},{reporting_profile:'debug'},{reporting_profile:null},
    {control_poll_s:0},{control_poll_s:61},{control_poll_s:1.5},
    {battery_percent:-1},{battery_percent:101},{battery_percent:1.5},
    {position_simulated:'true'},{battery_simulated:'true'},
    {latitude:null,longitude:null,position_simulated:true},
    {battery_percent:null,battery_simulated:true}])
    assert.throws(()=>parseHubPresence({...payload,...change}));
});
function mock(credential,err=null) {
  const filters=[],calls=[];
  const query={select(){return this;},eq(k,v){filters.push([k,v]);return this;},
    async maybeSingle(){return {data:credential,error:err};}};
  const db={from(table){assert.equal(table,'gateway_ingest_credentials');return query;},
    async rpc(name,args){calls.push(name);assert.equal(name,'bluepaws_record_hub_presence');
      assert.equal(args.p_gateway,16);
      return {data:[{received_at:'2026-08-27',settings_revision:2,desired_ble_enabled:false,
        display_name:'Hub',home_emoji:'🏡',portable_emoji:'📱',marker_colour:'#38bdf8'}],error:null};}};
  return {db,filters,calls};
}
test('gateway token is hashed and scoped; self heartbeat never claims collar commands',async()=>{
  const m=mock({gateway_guid16:16});
  const r=await handleHubPresence(m.db,payload,'synthetic-test-token','test');
  assert.equal(r.status,200);assert.deepEqual(m.calls,['bluepaws_record_hub_presence']);
  assert(m.filters.some(([k,v])=>k==='gateway_guid16'&&v===16));
  assert(m.filters.some(([k,v])=>k==='enabled'&&v===true));
  assert.match(m.filters.find(([k])=>k==='token_hash')[1],/^[0-9a-f]{64}$/);
  assert.equal((await r.json()).settings.ble_enabled,false);
  for(const [cred,err,status] of [[null,null,401],[null,{code:'test'},503]]){
    const rejected=mock(cred,err);
    assert.equal((await handleHubPresence(rejected.db,payload,'bad','test')).status,status);
    assert.equal(rejected.calls.length,0);
  }
});
test('hub avatars follow mode and overrides, and never collide with collar IDs',()=>{
  const h={gateway_guid16:16,display_name:'Hub',mode:'home',home_emoji:'',portable_emoji:'',
    latitude:null,longitude:null,received_at:'2026-08-27',fix_at:null,marker_colour:'#38bdf8'};
  assert.equal(hubAvatar(h).emoji,'🏡');
  assert.equal(hubAvatar({...h,mode:'portable'}).emoji,'📱');
  assert.equal(hubAvatar({...h,mode:'off_grid'}).emoji,'📱');
  assert.equal(hubAvatar({...h,home_emoji:'🐈'}).emoji,'🐈');
  assert.equal(hubMapDevice(h).id,-16);
  assert.equal(hubMapDevice(h).hasGps,false);
  assert.equal(hubMapDevice({...h,latitude:0,longitude:0}).hasGps,true);
  const simulated=hubMapDevice({...h,latitude:51.9,longitude:-2.2,battery_percent:92,
    position_simulated:true,battery_simulated:true});
  assert.equal(simulated.batteryPercent,92);
  assert.match(simulated.source,/Simulated test position/);
});

test('hub GNSS JSON accepts partial quality and leaves legacy values unknown',()=>{
  const real={...payload,position_simulated:false,gnss_valid:true,sat_count:8,acc_m:null,hdop:1.2};
  const parsed=parseHubPresence(real);
  assert.equal(parsed.p_gnss_valid,true);
  assert.equal(parsed.p_sat_count,8);
  assert.equal(parsed.p_acc_m,null,'HDOP must not be converted to metres');
  assert.equal(parsed.p_hdop,1.2);
  assert.equal(parseHubPresence({...real,acc_m:4.5}).p_acc_m,4.5);
  assert.equal(parseHubPresence({...real,sat_count:0}).p_sat_count,0);
  for (const field of ['p_gnss_valid','p_sat_count','p_acc_m','p_hdop'])
    assert.equal(parseHubPresence(payload)[field],null);
  assert.equal(parseHubPresence({...payload,gnss_valid:false}).p_gnss_valid,false);
});

test('hub GNSS rejects invalid types, bounds and quality without a real fix',()=>{
  const real={...payload,position_simulated:false,gnss_valid:true,sat_count:8,acc_m:5,hdop:1.2};
  for(const change of [{gnss_valid:'true'},{gnss_valid:false},{gnss_valid:null},
    {sat_count:-1},{sat_count:256},{sat_count:1.5},{sat_count:'8'},
    {acc_m:0},{acc_m:-1},{acc_m:65535},{acc_m:Infinity},{acc_m:NaN},{acc_m:'5'},
    {hdop:0},{hdop:-1},{hdop:10000},{hdop:NaN},{hdop:'1.2'},
    {latitude:null,longitude:null},{position_simulated:true}])
    assert.throws(()=>parseHubPresence({...real,...change}));
});

test('hub map adapter keeps fix age separate from heartbeat age and nullable quality',()=>{
  const hub={gateway_guid16:16,display_name:'Hub',mode:'home',latitude:51.9,longitude:-2.2,
    received_at:'2026-10-04T12:05:00Z',fix_at:'2026-10-04T12:00:00Z',gnss_valid:true,
    sat_count:8,acc_m:null,hdop:1.2};
  const device=hubMapDevice(hub);
  assert.equal(device.gnss.satellites,8);
  assert.equal(device.gnss.accuracyM,null);
  assert.equal(device.gnss.hdop,1.2);
  assert.equal(device.gnss.fixAgeS,300);
  assert.equal(hubMapDevice({...hub,received_at:'2026-10-04T12:06:00Z'}).gnss.fixAgeS,360);
  assert.equal(hubMapDevice({...hub,sat_count:null}).gnss.satellites,null);
  assert.equal(hubMapDevice({...hub,sat_count:0}).gnss.satellites,0);
  for(const change of [{gnss_valid:undefined},{gnss_valid:false},{fix_at:null},
    {position_simulated:true},{latitude:null,longitude:null}])
    assert.equal(hubMapDevice({...hub,...change}).gnss,null);
});

test('authenticated hub handler passes quality unchanged to the existing RPC',async()=>{
  const m=mock({gateway_guid16:16});
  const original=m.db.rpc;
  m.db.rpc=async(name,args)=>{
    assert.equal(args.p_sat_count,8); assert.equal(args.p_acc_m,null);
    assert.equal(args.p_hdop,1.2); assert.equal(args.p_gnss_valid,true);
    return original(name,args);
  };
  const report={...payload,position_simulated:false,gnss_valid:true,sat_count:8,hdop:1.2};
  assert.equal((await handleHubPresence(m.db,report,'synthetic-test-token','test')).status,200);
  assert.equal((await handleHubPresence(m.db,{...report,position_simulated:true},'synthetic-test-token','test')).status,400);
  assert.equal(m.calls.length,1);
});

test('reported BLE power is optional for legacy firmware and restricted to presets',()=>{
  assert.equal(parseHubPresence(payload).p_ble_tx_power_dbm,null);
  assert.equal(parseHubPresence(payload).p_ble_power_steps,null);
  for(const power of [-12,3,9]) assert.equal(parseHubPresence({...payload,ble_tx_power_dbm:power}).p_ble_tx_power_dbm,power);
  for(const power of [-24,0,20,'3',true,NaN,3.5]) assert.throws(()=>parseHubPresence({...payload,ble_tx_power_dbm:power}));
});
test('five-level capability accepts new powers without assuming old firmware support',()=>{
  for (const power of [-12,-6,3,6,9]) {
    const args=parseHubPresence({...payload,ble_tx_power_dbm:power,ble_power_steps:5});
    assert.equal(args.p_ble_tx_power_dbm,power); assert.equal(args.p_ble_power_steps,5);
  }
  assert.equal(parseHubPresence({...payload,ble_tx_power_dbm:3}).p_ble_power_steps,3);
  for (const power of [-6,6]) assert.throws(()=>parseHubPresence({...payload,ble_tx_power_dbm:power}));
  for (const steps of [0,4,'5',true,NaN]) assert.throws(()=>parseHubPresence({...payload,ble_tx_power_dbm:3,ble_power_steps:steps}));
});
