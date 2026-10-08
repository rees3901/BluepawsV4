import assert from 'node:assert/strict';
import test from 'node:test';
import {commandMessage, receiveDeadline, type CommandFeedback} from './collarFeedback.ts';
import {collarFault, loadFaultReports} from './collarFault.ts';
const start = Date.parse('2026-08-27T10:00:00Z');
const command: CommandFeedback = {id:'test',device_id:1001,command_type:'set_profile',command_payload:{profile:'active'},status:'sent',requested_at:new Date(start).toISOString(),expires_at:new Date(start+600000).toISOString()};
test('cloud feedback uses actual expiry and clears fifteen minutes after expiry', () => {
  assert.equal(commandMessage(command,start+599999)?.pending,true);
  assert.equal(commandMessage(command,start+600000)?.status,'expired');
  assert.equal(commandMessage(command,start+1500000),null);
  assert.equal(commandMessage({...command,status:'acked'},start+600000)?.status,'acked');
  assert.match(commandMessage(command,start)!.text,/profile → Active/);
});

test('hour-long commands stay visible through sleeping collar check-ins and only queued profiles can cancel', () => {
  const long = {...command, expires_at:new Date(start+3600000).toISOString()};
  for (const status of ['pending','sent']) {
    assert.equal(commandMessage({...long,status},start+1800000)?.pending,true);
    assert.equal(commandMessage({...long,status},start+3599999)?.pending,true);
    assert.equal(commandMessage({...long,status},start+3600000)?.status,'expired');
  }
  assert.equal(commandMessage({...long,status:'pending'},start+1800000)?.cancellable,true);
  assert.equal(commandMessage(long,start+1800000)?.cancellable,false);
  assert.equal(commandMessage({...long,status:'pending',command_type:'led_find'},start)?.cancellable,false);
  assert.equal(commandMessage({...long,status:'cancelled',status_at:new Date(start+1800000).toISOString()},start+1800001)?.status,'cancelled');
  assert.equal(commandMessage(long,start+4500000),null);
});
test('cloud receive window subtracts latency and cannot restart for a cached report', () => {
  assert.equal(receiveDeadline(9000,10000,1000),18000);
  assert.equal(receiveDeadline(9000,12000,1000,18000),18000);
  assert.equal(receiveDeadline(9000,12000,1000,0),0);
  for (const invalid of [null,undefined,0,-1,NaN,10001,'10000']) assert.equal(receiveDeadline(invalid,100,0),0);
});

test('fault reasons use explicit same-report flags, not radio quality or reset guesses', () => {
  assert.equal(collarFault({flags:0xc1,txReason:0})?.label,'Reported fault — stale GPS');
  assert.equal(collarFault({flags:0x85,txReason:0})?.label,'Reported fault — cause unspecified');
  assert.equal(collarFault({flags:0x80,txReason:0})?.label,'Reported fault — GPS fix unavailable');
  assert.equal(collarFault({flags:0xc4,txReason:4})?.label,'Reported fault — stale GPS');
  assert.match(collarFault({flags:0xc4,txReason:4})!.title,/stale GPS/);
  assert.doesNotMatch(collarFault({flags:0xc4,txReason:4})!.title,/low battery/);
  for (const txReason of [1,2,6,7,undefined,null]) {
    assert.equal(collarFault({flags:0x88,txReason})?.label,'Reported fault — cause unspecified');
  }
  assert.equal(collarFault({flags:0x81,resetReason:2})?.label,'Reported fault — cause unspecified');
  assert.match(collarFault({flags:0x81,resetReason:2})!.title,/Reset diagnostic 0x02.*previous reset/);
  for (const resetReason of [-1,256,NaN,1.5]) assert.doesNotMatch(collarFault({flags:0x81,resetReason})!.title,/Reset diagnostic/);
  for (let flags=0;flags<128;flags++) assert.equal(collarFault({flags,txReason:0,resetReason:3},true),null);
  for (const flags of [-1,256,NaN,1.5]) assert.equal(collarFault({flags}),null);
  assert.equal(collarFault(null,true)?.label,'Reported fault — cause unspecified');
});

test('fault detail loader batches exact faulty observation IDs and rejects mismatched reports', async () => {
  const rows=[{device_id:1001,observation_id:1,flags:128},{device_id:1002,observation_id:2,flags:193},
    {device_id:1003,observation_id:3,flags:0},{device_id:1004,observation_id:null,flags:null}];
  const reports=await loadFaultReports(rows,async ids=>{
    assert.deepEqual(ids,[1,2]);
    return {error:null,data:[
      {id:1,device_guid16:1001,flags:128,tx_reason:7,reset_reason:null},
      {id:2,device_guid16:1002,flags:193,tx_reason:4,reset_reason:2},
      {id:99,device_guid16:1001,flags:128,tx_reason:0}, // older report
      {id:1,device_guid16:2001,flags:128,tx_reason:0}, // different collar
      {id:1,device_guid16:1001,flags:192,tx_reason:0}, // flags disagree
    ]};
  });
  assert.deepEqual(reports,{1001:{flags:128,txReason:7,resetReason:null},1002:{flags:193,txReason:4,resetReason:2}});
  assert.equal(collarFault(reports[1001])?.label,'Reported fault — cause unspecified');
  assert.deepEqual(await loadFaultReports(rows.slice(2),()=>{throw Error('No read expected');}),{});
});

test('missing/denied diagnostics never reuse old detail or hide the reported fault', async () => {
  const rows=[{device_id:1001,observation_id:1,flags:128}];
  for (const read of [async()=>({data:null,error:'denied'}),async()=>({data:[],error:null}),async()=>{throw Error('offline');}]) {
    assert.deepEqual(await loadFaultReports(rows,read),{});
    assert.equal(collarFault({flags:rows[0].flags})?.label,'Reported fault — cause unspecified');
  }
});


test('command progress distinguishes hub collection from collar confirmation', () => {
  assert.match(commandMessage({...command,status:'pending'},start)!.text,/Queued/);
  assert.match(commandMessage({...command,status:'pending'},start)!.help,/next listens/);
  assert.match(commandMessage(command,start)!.text,/Sent to hub/);
  assert.match(commandMessage(command,start)!.help,/not yet confirmed/);
  assert.match(commandMessage({...command,status:'acked'},start)!.text,/Confirmed by collar/);
  assert.equal(commandMessage({...command,status:'acked'},start)!.pending,false);
});
