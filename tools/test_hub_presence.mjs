// No network/hardware: test the real Edge handler and map adapter.
import test from 'node:test';
import assert from 'node:assert/strict';
import {parseHubPresence,handleHubPresence} from '../supabase/functions/ingest-position/hub-presence.ts';
import {hubAvatar,hubMapDevice} from '../web/src/lib/hubPresence.ts';
const payload={format:'hub_status',ingest_path:'hub_self',gateway_guid16:'0010',mode:'home',
  latitude:51.9,longitude:-2.2,fix_age_s:5,uptime_s:100,wifi_rssi_dbm:-40,
  ble_enabled:true,ble_advertising:true,free_heap:150000,applied_revision:0};
test('hub report validates identity, position, bounds and separate transport',()=>{
  assert.equal(parseHubPresence(payload).p_gateway,16);
  assert.equal(parseHubPresence(payload).p_reporting_profile,'normal','legacy cadence remains honest');
  assert.equal(parseHubPresence(payload).p_control_poll_s,null);
  for(const profile of ['normal','power_save','active']) {
    const parsed=parseHubPresence({...payload,reporting_profile:profile,control_poll_s:5});
    assert.equal(parsed.p_reporting_profile,profile); assert.equal(parsed.p_control_poll_s,5);
  }
  assert.equal(parseHubPresence({...payload,latitude:null,longitude:null}).p_lat,null);
  for(const change of [{gateway_guid16:'03E9'},{gateway_guid16:'0000'},{ingest_path:'lora_hub'},
    {latitude:NaN},{latitude:null},{longitude:181},{fix_age_s:-1},{free_heap:-1},
    {ble_enabled:'true'},{mode:'bad'},{applied_revision:Infinity},
    {reporting_profile:'lost_alert'},{reporting_profile:'debug'},{reporting_profile:null},
    {control_poll_s:0},{control_poll_s:61},{control_poll_s:1.5}])
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
});
