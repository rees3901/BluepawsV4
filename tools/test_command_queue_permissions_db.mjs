// Isolated PostgreSQL/WASM regression test: no network, credentials or hardware.
// npm install --prefix .pio/feedback-tests --no-package-lock --ignore-scripts @electric-sql/pglite
// node tools/test_command_queue_permissions_db.mjs
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { PGlite } from '../.pio/feedback-tests/node_modules/@electric-sql/pglite/dist/index.js';

const db = new PGlite();
const uuid = (n) => `00000000-0000-0000-0000-${String(n).padStart(12, '0')}`;
const family = uuid(1), otherFamily = uuid(2);
const owner = uuid(10), member = uuid(11), outsider = uuid(12);
const guest = uuid(13), removed = uuid(14), viewer = uuid(15);
const migration = async (name) => db.exec(readFileSync(new URL(`../supabase/migrations/${name}.sql`, import.meta.url), 'utf8'));
const asUser = async (user, fn, role = 'authenticated') => {
  await db.query("select set_config('request.jwt.claim.sub', $1, false)", [user ?? '']);
  await db.exec(`set role ${role}`);
  try { return await fn(); } finally { await db.exec('reset role'); }
};
const queue = (user, { device = 1001, type = 'set_profile', payload = { profile: 'active' }, expiry = '10 minutes', schema = 'public', role } = {}) =>
  asUser(user, () => db.query(`select * from ${schema}.bluepaws_queue_device_command($1, $2, $3::jsonb, $4::interval)`,
    [device, type, JSON.stringify(payload), expiry]), role);
const denied = (call, message, code = '42501') => assert.rejects(call, (e) => e.code === code && message.test(e.message));
const count = async () => Number((await db.query('select count(*) as count from public.device_commands')).rows[0].count);

try {
  await db.exec(`
    create role authenticated; create role anon; create role service_role;
    create schema auth; create schema private;
    create function auth.uid() returns uuid language sql stable as $$
      select nullif(current_setting('request.jwt.claim.sub', true), '')::uuid;
    $$;
    grant usage on schema auth, private to authenticated;
    grant usage on schema auth, private to anon;
    create table auth.users(id uuid primary key);
    create table public.households(id uuid primary key);
    create table public.devices(device_id integer primary key, household_id uuid references households, enabled boolean not null);
    create table public.household_members(household_id uuid references households, user_id uuid references auth.users, role text,
      primary key(household_id, user_id));
    grant select on household_members to authenticated;
    insert into households values ('${family}'), ('${otherFamily}');
    insert into auth.users values ('${owner}'), ('${member}'), ('${outsider}'), ('${guest}'), ('${removed}'), ('${viewer}');
    insert into devices values (1001,'${family}',true), (2001,'${otherFamily}',true), (1002,'${family}',false);
    insert into household_members values
      ('${family}','${owner}','owner'), ('${family}','${member}','member'),
      ('${family}','${guest}','guest_viewer'), ('${family}','${removed}','member'),
      ('${family}','${viewer}','viewer'), ('${otherFamily}','${outsider}','owner');
    delete from household_members where user_id='${removed}';
  `);
  await migration('20260824181500_add_device_command_queue');
  await migration('20260826102709_harden_one_hour_device_commands');
  await migration('20260827100000_fix_command_function_ambiguity');

  // Reproduce the reported failure through the SAME public RPC and role as the web app.
  await denied(queue(owner), /permission denied for function bluepaws_queue_device_command/);
  assert.equal(await count(), 0);
  console.log('PASS: original authenticated web RPC permission failure reproduced');

  await migration('20260827213901_fix_cloud_command_queue_permissions');
  // Reapplying the migration must be harmless.
  await migration('20260827213901_fix_cloud_command_queue_permissions');
  const first = (await queue(owner)).rows[0];
  assert.equal(first.status, 'pending');
  assert.equal(first.command_sequence_id, 1);
  assert.equal(first.device_id, 1001);
  const second = (await queue(member, { payload: { profile: 'power_save' } })).rows[0];
  assert.equal(second.command_sequence_id, 2);
  const saved = (await db.query('select * from device_commands where id=$1', [second.id])).rows[0];
  assert.equal(saved.requested_by, member);
  assert.equal(saved.household_id, family);
  assert.equal(new Date(saved.expires_at) - new Date(saved.requested_at), 600000, 'UI ten-minute expiry unchanged');
  const superseded = (await db.query('select status, last_error from device_commands where id=$1', [first.id])).rows[0];
  assert.equal(superseded.status, 'cancelled');
  assert.equal(superseded.last_error, 'superseded_by_new_profile_command');

  const beforeDenials = await count();
  for (const schema of ['public', 'private']) {
    await denied(queue(null, { schema }), /Authentication required/);
    await denied(queue(owner, { schema, role: 'anon' }), /permission denied/);
    for (const user of [outsider, guest, removed, viewer]) {
      await denied(queue(user, { schema }), /Family membership required/);
    }
    await denied(queue(owner, { schema, device: 2001 }), /Family membership required/);
  }
  await denied(queue(owner, { device: 1002 }), /Device not found/, 'P0002');
  await denied(queue(owner, { device: 9999 }), /Device not found/, 'P0002');
  await denied(queue(member, { type: 'reboot', payload: {} }), /Owner role required/);
  await denied(queue(member, { type: 'debug_cadence', payload: { enabled: true, interval_s: 30 } }), /Owner role required/);
  await denied(queue(member, { payload: { profile: 'debug' } }), /Owner role required/);
  await denied(queue(owner, { payload: { profile: 'not-a-profile' } }), /valid profile/, '22023');
  await denied(queue(owner, { type: 'not-a-command' }), /Unsupported command type/, '22023');
  for (const expiry of [null, '0 seconds', '25 hours']) {
    await denied(queue(owner, { expiry }), /Command expiry/, '22023');
  }
  assert.equal(await count(), beforeDenials, 'rejected calls never insert');
  assert.equal((await db.query('select status from device_commands where id=$1', [second.id])).rows[0].status, 'pending', 'rejected calls never supersede');
  await queue(owner, { type: 'reboot', payload: {} });

  // The fix only opens the guarded writer, NOT arbitrary writes/sequence/claim/ACK helpers.
  await asUser(owner, async () => {
    await denied(db.query('insert into device_commands default values'), /permission denied/);
    await denied(db.query("update device_commands set status='acked'"), /permission denied/);
    await denied(db.query('delete from device_commands'), /permission denied/);
    await denied(db.query('select private.bluepaws_next_command_sequence(1001)'), /permission denied/);
    await denied(db.query("select private.bluepaws_validate_command_payload('request_status','{}')"), /permission denied/);
    await denied(db.query("select public.bluepaws_claim_next_device_command(1001,'lora_hub')"), /permission denied/);
    await denied(db.query('select public.bluepaws_ack_device_command(1001,1)'), /permission denied/);
  });
  const acl = (await db.query(`select n.nspname, p.prosecdef, p.proconfig,
    has_function_privilege('authenticated',p.oid,'EXECUTE') as member_execute,
    has_function_privilege('anon',p.oid,'EXECUTE') as anon_execute
    from pg_proc p join pg_namespace n on n.oid=p.pronamespace
    where p.proname='bluepaws_queue_device_command' order by n.nspname`)).rows;
  assert.equal(acl.length, 2);
  assert.equal(acl.find((r) => r.nspname === 'public').prosecdef, false, 'public wrapper stays invoker');
  for (const row of acl) {
    assert.equal(row.member_execute, true);
    assert.equal(row.anon_execute, false);
    assert.deepEqual(row.proconfig, ['search_path=""']);
  }

  // A sent-but-unacknowledged command remains available to either authenticated
  // return path. Both transports receive the same identity; the first valid ACK
  // closes it, and a late duplicate ACK cannot apply or reopen anything.
  await db.exec('set role service_role');
  try {
    const cellular = (await db.query("select * from public.bluepaws_claim_next_device_command($1,'cellular_direct')", [1001])).rows[0];
    const lora = (await db.query("select * from public.bluepaws_claim_next_device_command($1,'lora_hub')", [1001])).rows[0];
    assert.equal(cellular.id, second.id);
    assert.equal(lora.id, second.id);
    assert.equal(cellular.command_sequence_id, second.command_sequence_id);
    assert.equal(lora.command_sequence_id, second.command_sequence_id);
    // PGlite's synthetic service_role does not carry PostgreSQL BYPASSRLS, so
    // inspect the row as the test owner between privileged RPC calls.
    await db.exec('reset role');
    const offered = (await db.query('select status, attempts from device_commands where id=$1', [second.id])).rows[0];
    assert.equal(offered.status, 'sent');
    assert.equal(offered.attempts, 2);
    await db.exec('set role service_role');
    const acknowledged = (await db.query('select * from public.bluepaws_ack_device_command($1,$2)',
      [1001, second.command_sequence_id])).rows[0];
    assert.equal(acknowledged.status, 'acked');
    assert.equal((await db.query('select * from public.bluepaws_ack_device_command($1,$2)',
      [1001, second.command_sequence_id])).rows.length, 0, 'late duplicate ACK is idempotent');
    await denied(db.query("select * from public.bluepaws_claim_next_device_command(1001,'sms')"),
      /Unsupported command transport/, '22023');
  } finally { await db.exec('reset role'); }
  console.log('PASS: queue permissions, expiry/supersession, least privilege and cross-path command/ACK convergence');
} finally { await db.close(); }
