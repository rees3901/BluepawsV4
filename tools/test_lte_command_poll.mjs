import test from 'node:test';
import assert from 'node:assert/strict';
import { handleDeviceCommands, commandEnvelope } from '../supabase/functions/ingest-position/device-commands.ts';

const payload = { format: 'device_commands', ingest_path: 'cellular_direct', device_id: 1010 };
const queued = { id: 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', command_sequence_id: 17,
  command_type: 'set_profile', command_payload: { profile: 'active' }, expires_at: '2026-08-28T23:00:00Z' };
function mock(options = {}) {
  const filters = [], calls = [];
  return { filters, calls, db: {
    from(table) {
      calls.push(table);
      assert(['device_ingest_credentials', 'devices'].includes(table));
      return { select() { return this; }, eq(k,v) { filters.push([table,k,v]); return this; },
        async maybeSingle() { return table === 'devices'
          ? { data: options.disabled ? null : { household_id: 'family' }, error: options.deviceError }
          : { data: options.badToken ? null : { device_id: 1010 }, error: options.authError }; } };
    },
    async rpc(name, args) {
      calls.push(name);
      assert.equal(name, 'bluepaws_claim_next_device_command');
      assert.deepEqual(args, { requested_device_id: 1010, requested_transport: 'cellular_direct' });
      return { data: options.empty ? [] : [queued], error: options.rpcError };
    },
  } };
}

test('LTE polling scopes hashed bearer to an enabled collar and claims without telemetry or ACK', async () => {
  const m = mock();
  const response = await handleDeviceCommands(m.db, payload, 'synthetic-token', 'test');
  assert.equal(response.status, 200);
  const body = await response.json();
  assert.equal(body.device_id, 1010);
  assert.equal(body.command_pending, true);
  assert.equal(body.command.sequence_id, 17);
  assert.equal(body.command.expires_unix, Date.parse(queued.expires_at) / 1000);
  assert.deepEqual(m.calls, ['device_ingest_credentials','devices','bluepaws_claim_next_device_command']);
  assert(m.filters.some(([t,k,v]) => t === 'device_ingest_credentials' && k === 'device_id' && v === 1010));
  assert(m.filters.some(([t,k,v]) => t === 'devices' && k === 'enabled' && v === true));
  assert.match(m.filters.find(([,k]) => k === 'token_hash')[2], /^[0-9a-f]{64}$/);
  assert.equal(response.headers.get('cache-control'), 'no-store');
});

test('invalid identities and wrong transports never reach credentials or the queue', async () => {
  for (const change of [{device_id:16},{device_id:0},{device_id:65535},{device_id:1010.5},
    {device_id:'1010'},{ingest_path:'lora_hub'},{format:'hub_settings'}]) {
    const m = mock();
    assert.equal((await handleDeviceCommands(m.db, {...payload,...change}, 'token', 'test')).status, 400);
    assert.equal(m.calls.length, 0);
  }
});

test('denied, disabled and unavailable credentials cannot claim commands', async () => {
  for (const [options, status] of [[{badToken:true},401],[{disabled:true},401],
    [{authError:{code:'test'}},503],[{deviceError:{code:'test'}},503]]) {
    const m=mock(options);
    assert.equal((await handleDeviceCommands(m.db,payload,'token','test')).status,status);
    assert(!m.calls.includes('bluepaws_claim_next_device_command'));
  }
  const m=mock({rpcError:{code:'test'}});
  assert.equal((await handleDeviceCommands(m.db,payload,'token','test')).status,503);
});

test('an empty queue returns an explicit empty result, using the same upload envelope', async () => {
  const m=mock({empty:true});
  const body=await (await handleDeviceCommands(m.db,payload,'token','test')).json();
  assert.equal(body.command_pending,false); assert.equal(body.command,null);
  assert.equal(commandEnvelope(null),null);
  assert.deepEqual(commandEnvelope(queued).payload,{profile:'active'});
});
