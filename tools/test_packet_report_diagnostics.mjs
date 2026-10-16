import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {fixtureModule,renderReport} from './shared_device_card_fixture.mjs';
const {packetReportDiagnostics:diagnostics}=fixtureModule('@/lib/packetReportDiagnostics');
const {buildDeviceReport,buildCurrentDeviceReport,deviceReportsToCsv}=fixtureModule('@/lib/deviceReports');
const field=(rows,name)=>rows.find(r=>r.field===name);
const observation={id:1,device_guid16:3001,msg_seq_id:12,time_unix:1770000000,recorded_at:'2026-10-16T12:00:00Z',received_at:'2026-10-16T12:00:01Z',status:0,power_profile:0,flags:0,tx_reason:0,gnss_valid:false,latitude:null,longitude:null,batt_mv:0,acc_m:0,fix_age_s:65535,sat_count:255};

test('all 256 flag patterns across all 8 message categories remain packet-specific',()=>{
  for(let flags=0;flags<256;flags++) for(let reason=0;reason<8;reason++) {
    const rows=diagnostics(flags,reason,{});
    for(const [name,mask] of [['Home beacon (packet)',8],['Stale GPS (packet)',64],['Reported error (packet)',128]]) {
      assert.equal(field(rows,name).data,flags&mask?'Set in this packet':'Not set in this packet');
    }
    if(!(flags&64)) assert.match(field(rows,'Stale GPS (packet)').meaning,/not proof of a fresh fix/);
    if((flags&192)===192) assert.match(field(rows,'Reported error (packet)').meaning,/do not establish its root cause/);
    if(!(flags&128)) assert.match(field(rows,'Reported error (packet)').meaning,/does not guarantee/);
    if((flags&8)&&reason===1) assert.match(field(rows,'Home beacon (packet)').meaning,/ACK does not establish a new beacon/);
    assert.ok(rows.every(r=>r.data&&r.description&&r.meaning));
  }
});

test('optional values decode when present; zero is not mistaken for absence',()=>{
  const rows=diagnostics(0,4,{fw_ver:0x0102,reset_reason:0,uptime_s:0});
  assert.equal(field(rows,'Firmware version').data,'1.2');
  assert.equal(field(rows,'Reset reason').data,'0x00');
  assert.equal(field(rows,'Reported uptime').data,'0 seconds');
  assert.match(field(rows,'Reset reason').meaning,/depends on the board and firmware/);
  assert.match(field(rows,'Reported uptime').meaning,/Sleep may reset or pause/);
  assert.equal(field(diagnostics(0,0,{fw_ver:65535,uptime_s:4294967295}),'Firmware version').data,'255.255');
  assert.equal(field(diagnostics(0,0,{fw_ver:65535,uptime_s:4294967295}),'Reported uptime').data,'4294967295 seconds');
  assert.equal(field(diagnostics(0,0,{fw_ver:0}),'Firmware version').data,'0.0');
  assert.equal(field(diagnostics(0,1,{acked_msg_seq_id:0}),'Acknowledged command').data,'Sequence 0');
});

test('missing/malformed TLVs and invalid flags never imply measured negative states',()=>{
  for(const data of [undefined,null,[],true,'bad',{}, {fw_ver:'256',reset_reason:-1,uptime_s:NaN,acked_msg_seq_id:65536}, {fw_ver:1.5,reset_reason:256,uptime_s:4294967296}]) {
    const rows=diagnostics(undefined,4,data);
    assert.equal(field(rows,'Home beacon (packet)').data,'Not supplied');
    for(const name of ['Firmware version','Reset reason','Reported uptime']) assert.match(field(rows,name).data,/Not supplied|Invalid value/);
  }
  for(const flags of [-1,256,1.5,NaN,'8',null]) assert.equal(field(diagnostics(flags,0,{}),'Raw packet flags').data,'Not supplied');
  assert.equal(field(diagnostics(0,0,Object.create({fw_ver:256,uptime_s:1})),'Firmware version'),undefined);
});

test('ACK references and reset diagnostics are not relabelled as current faults or completed actions',()=>{
  assert.match(field(diagnostics(128,0,{reset_reason:4}),'Reset reason').meaning,/outside a boot report/);
  assert.match(field(diagnostics(0,0,{acked_msg_seq_id:100}),'Acknowledged command').meaning,/outside an ACK/);
  assert.match(field(diagnostics(0,1,{acked_msg_seq_id:100}),'Acknowledged command').meaning,/does not prove a physical action completed/);
  assert.match(field(diagnostics(0,1,{}),'Acknowledged command').meaning,/cannot be linked/);
  for(const name of ['Acknowledged command','Firmware version','Reset reason']) assert.equal(field(diagnostics(0,0,{}),name),undefined);
});

test('dashboard fallback never invents original flags or optional TLVs',()=>{
  const report=buildCurrentDeviceReport({id:3001,seq:1,time:1770000000,status:'Home',profile:'Normal',bleHome:true,hasGps:true,lat:51,lon:-2,batt:3700,lastUpdate:1770000000000,ingestPath:null});
  assert.equal(field(report.rows,'Home beacon (packet)').data,'Not supplied');
  assert.equal(field(report.rows,'Raw packet flags').data,'Not supplied');
  assert.match(field(report.rows,'Reported uptime').data,/Not supplied/);
  assert.equal(field(diagnostics(255,4,{fw_ver:256,uptime_s:10},false),'Firmware version').data,'Not supplied');
});

test('advanced rows are collapsed separately, while CSV retains every diagnostic',()=>{
  const report=buildDeviceReport({...observation,tx_reason:4,flags:192,tlv_data:{fw_ver:256,reset_reason:4,uptime_s:9}},null);
  const html=renderReport(report,'collar');
  assert.match(html,/<details class="previous-reports report-diagnostics"><summary>Advanced packet diagnostics/);
  assert.doesNotMatch(html,/<details[^>]*\bopen/);
  const beforeAdvanced=html.split('<details class="previous-reports report-diagnostics">')[0];
  assert.match(beforeAdvanced,/Stale GPS \(packet\)/); assert.match(beforeAdvanced,/Firmware version/);
  assert.doesNotMatch(beforeAdvanced,/Raw packet flags|Reset reason|Reported uptime/);
  const csv=deviceReportsToCsv([report]);
  for(const name of ['Raw packet flags','Reset reason','Firmware version']) assert.ok(csv.includes(name));
});

test('cloud report query selects stored TLVs and retains its device filter',async()=>{
  const require=createRequire(new URL('../web/package.json',import.meta.url));
  const ts=require('typescript'); let selected,filtered;
  const query={select(columns){selected=columns;return this;},eq(key,value){filtered=[key,value];return this;},order(){return this;},limit(){return Promise.resolve({data:[{...observation,tx_reason:4,tlv_data:{fw_ver:258,reset_reason:0,uptime_s:5}}],error:null});}};
  const client={from(table){return table==='observations'?query:{select(){return this;},in(){return this;},order(){return Promise.resolve({data:[],error:null});}};}};
  const source=readFileSync(new URL('../web/src/lib/deviceReports.ts',import.meta.url),'utf8');
  const context=vm.createContext({exports:{},console,require(name){return name==='@/lib/supabase/client'?{createClient:()=>client}:fixtureModule(name.startsWith('./')?'@/lib/'+name.slice(2).replace(/\.ts$/,''):name);}});
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  const reports=await context.exports.loadDeviceReports(3001);
  assert.match(selected,/(?:^|,)tlv_data(?:,|$)/);
  assert.deepEqual(filtered,['device_guid16',3001]);
  assert.equal(field(reports[0].rows,'Firmware version').data,'1.2');
});
