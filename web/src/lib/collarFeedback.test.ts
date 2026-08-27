import assert from 'node:assert/strict';
import test from 'node:test';
import {commandMessage, receiveDeadline, type CommandFeedback} from './collarFeedback.ts';
const start = Date.parse('2026-08-27T10:00:00Z');
const command: CommandFeedback = {id:'test',device_id:1001,command_type:'set_profile',command_payload:{profile:'active'},status:'sent',requested_at:new Date(start).toISOString(),expires_at:new Date(start+600000).toISOString()};
test('cloud feedback uses actual expiry and clears fifteen minutes after submission', () => {
  assert.equal(commandMessage(command,start+599999)?.pending,true);
  assert.equal(commandMessage(command,start+600000)?.status,'expired');
  assert.equal(commandMessage(command,start+900000),null);
  assert.equal(commandMessage({...command,status:'acked'},start+600000)?.status,'acked');
  assert.match(commandMessage(command,start)!.text,/profile → Active/);
});
test('cloud receive window subtracts latency and cannot restart for a cached report', () => {
  assert.equal(receiveDeadline(9000,10000,1000),18000);
  assert.equal(receiveDeadline(9000,12000,1000,18000),18000);
  assert.equal(receiveDeadline(9000,12000,1000,0),0);
  for (const invalid of [null,undefined,0,-1,NaN,10001,'10000']) assert.equal(receiveDeadline(invalid,100,0),0);
});
