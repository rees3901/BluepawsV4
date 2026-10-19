import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import vm from 'node:vm';
import {fixtureModule,hub,renderReport} from './shared_device_card_fixture.mjs';
const {createCollarReportExport:collar,createHubReportExport:hubExport}=fixtureModule('@/lib/reportExport');
const {buildDeviceReport,buildCurrentDeviceReport}=fixtureModule('@/lib/deviceReports');
const now=Date.parse('2026-10-19T12:00:00Z');
const observation={id:1,device_guid16:3001,msg_seq_id:0,time_unix:now/1000,recorded_at:new Date(now).toISOString(),received_at:new Date(now+2000).toISOString(),status:0,power_profile:0,flags:9,tx_reason:0,gnss_valid:true,latitude:51.90674,longitude:-2.24099,batt_mv:0,acc_m:6,fix_age_s:120,sat_count:13,tlv_data:{fw_ver:258,reset_reason:4,uptime_s:123,unknown_optional:27}};
const path={observation_id:1,ingest_path:'lora_hub',link_type:'lora',gateway_guid16:30,link_rssi_dbm:-84,link_snr_db:-5,first_received_at:observation.received_at,last_received_at:observation.received_at,receipt_count:2};
const device={id:3001,name:'Podge'};
const report=buildDeviceReport(observation,path);

test('JSON preserves typed readings, original TLVs, routes and independent GPS time without repeated prose',()=>{
  const r={...report,paths:[path,{...path,ingest_path:'cellular_direct',link_type:'lte',gateway_guid16:null}]};
  const result=collar(device,[r],'json',now);
  const json=JSON.parse(result.content), row=json.reports[0].readings;
  assert.match(result.filename,/podge_3001_reports_.*\.json$/);assert.match(result.mimeType,/application\/json/);
  assert.equal(json.schema_version,1);assert.equal(json.device.id,3001);
  assert.equal(row.battery_mv,null);assert.equal(json.reports[0].raw_observation.batt_mv,0);
  assert.equal(row.radio_rssi_dbm,-84);assert.equal(row.radio_snr_db,-5);assert.equal(row.sequence,0);
  assert.equal(row.gps_measured_at_utc,new Date(now-120000).toISOString());
  assert.equal(row.received_at_utc,new Date(now+2000).toISOString());assert.equal(row.power_profile,'PowerSave');
  assert.equal(json.reports[0].raw_observation.tlv_data.unknown_optional,27);
  assert.equal(json.reports[0].accepted_routes.length,2);assert.equal(json.summary.accepted_packets,1);
  assert.equal(json.summary.packets_with_home_beacon,1);
  assert.doesNotMatch(result.content,/field_descriptions|reading_meanings|What this means|Several satellites were/);
});

test('check-ins, GNSS sentinels, malformed fixes and dashboard fallback never invent fresh measurements',()=>{
  for(const tx_reason of [1,2,5,6,7]) {
    const row=JSON.parse(collar(device,[buildDeviceReport({...observation,tx_reason},path)],'json',now).content).reports[0].readings;
    assert.equal(row.gps_measured_at_utc,null);
  }
  for(const override of [{fix_age_s:65535},{gnss_valid:false},{latitude:100},{latitude:null},{packetAvailable:false}]) {
    const json=JSON.parse(collar(device,[buildDeviceReport({...observation,...override},path)],'json',now).content);
    assert.equal(json.reports[0].readings.gps_measured_at_utc,null);
  }
  const snap=buildCurrentDeviceReport({id:3001,seq:9,time:now/1000,hasGps:true,lat:51,lon:-2,batt:0,rssi:-80,snr:2,bleHome:true,profile:'Normal',status:'Home',lastUpdate:now,ingestPath:'lora_hub'});
  const json=JSON.parse(collar(device,[snap],'json',now).content);
  assert.equal(json.summary.dashboard_snapshots,1);assert.equal(json.summary.accepted_packets,0);
  assert.equal(json.reports[0].raw_observation,null);assert.equal(json.reports[0].accepted_routes.length,0);
  assert.equal(json.reports[0].readings.home_beacon_seen_in_packet,null);
  assert.equal(json.reports[0].readings.receipt_count_on_selected_route,null);
});

test('ordering, coverage, identity and download limits are explicit',()=>{
  const earlier=buildDeviceReport({...observation,id:2,received_at:new Date(now-60000).toISOString()},null);
  const json=JSON.parse(collar(device,[report,earlier],'json',now).content);
  assert.equal(json.reports[0].readings.observation_id,2);
  assert.equal(json.summary.largest_received_report_gap_s,62);
  assert.equal(json.coverage.requested_max_reports,1000);assert.equal(json.coverage.limit_reached,false);
  assert.equal(JSON.parse(collar(device,Array(1000).fill(report),'json',now).content).coverage.limit_reached,true);
  assert.throws(()=>collar(device,[]),/No reports/);
  assert.throws(()=>collar({id:3004,name:'Simba'},[report]),/identity/);
  assert.throws(()=>collar(device,Array(1001).fill(report)),/limit/);
});

test('data CSV has real numeric columns, blank unknowns, units and no advice blobs',()=>{
  const result=collar(device,[report],'csv',now);
  assert.ok(result.content.startsWith('\uFEFF'));assert.match(result.filename,/\.csv$/);
  const [head,line]=result.content.slice(1).split('\r\n');
  const headings=head.split(',').map(s=>s.replaceAll('"','')), values=line.split(',');
  assert.equal(values[headings.indexOf('radio_rssi_dbm')],'-84');
  assert.equal(values[headings.indexOf('radio_snr_db')],'-5');
  assert.equal(values[headings.indexOf('battery_mv')],'""');
  assert.ok(headings.includes('latitude_deg'));assert.ok(headings.includes('gps_fix_age_at_report_s'));
  assert.doesNotMatch(result.content,/description|meaning|advisable|https:\/\/www.google/);
});

test('hub exports are numeric snapshots with simulation flags and a strict data allowlist',()=>{
  const value={...hub,household_id:'private-family',avatar_storage_path:'private/avatar',wifi_rssi_dbm:-69,battery_percent:0,position_simulated:true,uptime_s:0};
  const json=JSON.parse(hubExport(value,'json',now).content);
  assert.equal(json.coverage.scope,'latest_hub_snapshot');assert.equal(json.reports[0].wifi_rssi_dbm,-69);
  assert.equal(json.reports[0].battery_percent,0);assert.equal(json.reports[0].uptime_s,0);
  assert.equal(json.reports[0].position_simulated,true);assert.doesNotMatch(JSON.stringify(json),/private-family|private\/avatar/);
  assert.match(hubExport({...value,mode:'=DANGEROUS()'},'csv',now).content,/"'=DANGEROUS/);
});

test('download helper passes the correct bytes, MIME and filename to a Blob download',async()=>{
  const require=createRequire(new URL('../web/package.json',import.meta.url)),ts=require('typescript');
  let blob,appended=false,clicked=false,removed=false,revoked=false,timer;
  const anchor={click(){clicked=true;},remove(){removed=true;}};
  const context=vm.createContext({exports:{},Blob,URL:{createObjectURL(value){blob=value;return 'blob:synthetic';},revokeObjectURL(value){assert.equal(value,'blob:synthetic');revoked=true;}},document:{createElement(){return anchor;},body:{append(){appended=true;}}},setTimeout(fn,delay){timer={fn,delay};}});
  vm.runInContext(ts.transpileModule(readFileSync(new URL('../web/src/lib/reportExport.ts',import.meta.url),'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const result=collar(device,[report],'json',now);
  context.exports.saveReportExport(result);
  assert.equal(await blob.text(),result.content);assert.equal(blob.type,result.mimeType);
  assert.equal(anchor.download,result.filename);assert.equal(anchor.href,'blob:synthetic');
  assert.ok(appended&&clicked&&removed);assert.equal(timer.delay,1000);assert.equal(revoked,false);timer.fn();assert.equal(revoked,true);
});

test('export controls are explicit and disable downloads while loading',()=>{
  const html=renderReport(report,'collar');
  assert.match(html,/Download JSON/);assert.match(html,/Data CSV/);assert.match(html,/1,000 accepted reports/);
  assert.doesNotMatch(html,/>Download CSV</);
  for(const props of [{loading:true},{exporting:true}]) {
    const busy=renderReport(report,'collar',props);
    assert.match(busy,/<button[^>]*disabled=""[^>]*>(Download JSON|Preparing download…)<\/button>/);
    assert.match(busy,/<button[^>]*disabled=""[^>]*>Data CSV<\/button>/);
  }
});

test('export query retains device filtering, batches routes, and refuses incomplete route retrieval',async()=>{
  const require=createRequire(new URL('../web/package.json',import.meta.url));const ts=require('typescript');
  const calls=[];let fail=false;
  const rows=Array.from({length:205},(_,i)=>({...observation,id:i+1}));
  const client={from(table){const query={table,keys:[],select(){return this;},eq(k,v){calls.push(['eq',k,v]);return this;},order(){return this;},in(k,v){this.keys=v;calls.push(['batch',v.length]);return this;},limit(n){calls.push(['limit',table,n]);return Promise.resolve(table==='observations'?{data:rows.slice(0,n),error:null}:fail?{data:null,error:Error('route unavailable')}:{data:this.keys.map(id=>({...path,observation_id:id})),error:null});}};return query;}};
  const context=vm.createContext({exports:{},console,require:name=>name==='@/lib/supabase/client'?{createClient:()=>client}:fixtureModule(name.startsWith('./')?'@/lib/'+name.slice(2).replace(/\.ts$/,''):name)});
  vm.runInContext(ts.transpileModule(readFileSync(new URL('../web/src/lib/deviceReports.ts',import.meta.url),'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const reports=await context.exports.loadDeviceReports(3001,1000,true);
  assert.equal(reports.length,205);assert.ok(reports.every(r=>r.paths.length===1));
  assert.deepEqual(calls.filter(c=>c[0]==='batch').map(c=>c[1]),[100,100,5]);
  assert.ok(calls.some(c=>JSON.stringify(c)===JSON.stringify(['eq','device_guid16',3001])));
  fail=true;await assert.rejects(()=>context.exports.loadDeviceReports(3001,1000,true),/complete route details/);
  await assert.rejects(()=>context.exports.loadDeviceReports(3001,1001,true),/Invalid report history limit/);
});
