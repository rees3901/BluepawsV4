// Isolated PostgreSQL/WASM test, never connects to Supabase.
// npm install --prefix .pio/feedback-tests --no-package-lock --ignore-scripts @electric-sql/pglite
// node tools/test_collar_feedback_db.mjs
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { PGlite } from '../.pio/feedback-tests/node_modules/@electric-sql/pglite/dist/index.js';
const db = new PGlite();
const family = '00000000-0000-0000-0000-000000000001';
const other = '00000000-0000-0000-0000-000000000002';
try {
  // Minimal columns used by the real migration, with RLS on every source.
  await db.exec(`
    create role authenticated; create role anon; create role service_role bypassrls;
    create schema auth;
    create function auth.uid() returns uuid language sql as $$ select current_setting('test.user')::uuid $$;
    grant usage on schema auth to authenticated;
    create schema private; create schema realtime;
    create table households(id uuid primary key, access_version integer);
    insert into households values('${family}',1),('${other}',1);
    create table gateways(gateway_guid16 integer primary key, household_id uuid, display_name text, enabled boolean);
    create table household_members(household_id uuid, user_id uuid, role text);
    insert into gateways values(16,'${family}','Home Hub',true),(32,'${other}','Other Hub',true);
    insert into household_members values('${family}','${family}','owner'),('${other}','${other}','member');
    grant select on gateways to service_role;
    select set_config('test.user','${family}',false);
    create table realtime.test_events(topic text, event text);
    create function realtime.broadcast_changes(text,text,text,text,text,record,record)
      returns void language plpgsql as $$ begin insert into realtime.test_events values($1,$2); end; $$;
    create table devices(device_id integer primary key, household_id uuid);
    create table observations(id bigint primary key, device_guid16 integer, household_id uuid, flags smallint,
      recorded_at timestamptz, effective_seen_at timestamptz);
    create table observation_paths(id bigint primary key, observation_id bigint, ingest_path text,
      first_received_at timestamptz, gateway_rx_time_unix bigint, offline_replay boolean);
    create table device_commands(id uuid primary key, device_id integer, household_id uuid,
      command_type text, command_payload jsonb, status text, requested_at timestamptz, expires_at timestamptz);
    alter table devices enable row level security;
    alter table observations enable row level security;
    alter table observation_paths enable row level security;
    alter table device_commands enable row level security;
    create policy family_devices on devices to authenticated using (household_id = current_setting('test.family')::uuid);
    create policy family_obs on observations to authenticated using (household_id = current_setting('test.family')::uuid);
    create policy family_cmd on device_commands to authenticated using (household_id = current_setting('test.family')::uuid);
    create policy family_paths on observation_paths to authenticated using (exists(select 1 from observations o where o.id=observation_id));
    grant select on all tables in schema public to authenticated;
    insert into devices values (1001,'${family}'),(2001,'${other}');
    set test.family = '${family}';
  `);
  await db.exec(readFileSync(new URL('../supabase/migrations/20260827143000_add_collar_feedback_snapshot.sql', import.meta.url),'utf8'));
  await db.exec(readFileSync(new URL('../supabase/migrations/20260827215926_add_hub_presence_and_receive_activity.sql', import.meta.url),'utf8'));
  const read = async (id=family) => {
    await db.exec('set role authenticated');
    try { return (await db.query('select * from bluepaws_collar_feedback($1)',[id])).rows; }
    finally { await db.exec('reset role'); }
  };
  assert.equal((await read())[0].rx_window_remaining_ms,0,'no observation must not light bulb');
  assert.deepEqual(await read(other),[],'RLS hides another Family');
  await db.exec(`insert into observations values(1,1001,'${family}',0,now()-interval '2 seconds',now()-interval '2 seconds');
    insert into observation_paths values(1,1,'lora_hub',now(),floor(extract(epoch from now()-interval '2 seconds'))::bigint,false);`);
  let row = (await read())[0];
  assert(row.rx_window_remaining_ms>0 && row.rx_window_remaining_ms<=8000,'live window uses original time');
  assert.equal(row.flags,0);
  await db.exec('update observation_paths set offline_replay=true');
  assert.equal((await read())[0].rx_window_remaining_ms,0,'replay never lights bulb');
  await db.exec(`update observation_paths set offline_replay=false, first_received_at=now()-interval '1 hour'`);
  assert.equal((await read())[0].rx_window_remaining_ms,0,'duplicate path upload cannot renew first receipt');
  await db.exec(`update observation_paths set first_received_at=now(), gateway_rx_time_unix=null`);
  assert.equal((await read())[0].rx_window_remaining_ms,0,'missing LoRa clock is conservative');
  await db.exec(`update observation_paths set ingest_path='cellular_direct'; update observations set flags=128`);
  row=(await read())[0]; assert(row.rx_window_remaining_ms>0); assert.equal(row.flags,128,'real header fault preserved');
  await db.exec(`update observations set recorded_at=now()+interval '1 minute'`);
  assert((await read())[0].rx_window_remaining_ms>0,'fresh reception, not the collar clock, drives the window');
  await db.exec(`update observations set recorded_at=now()-interval '1 year'`);
  assert((await read())[0].rx_window_remaining_ms>0,'old GNSS/collar clock does not suppress fresh reception');
  await db.exec(`insert into device_commands values('${family}',1001,'${family}','set_profile','{"profile":"active"}','acked',now()-interval '11 minutes',now()-interval '1 minute')`);
  assert.equal((await read())[0].command.status,'acked','ACK remains visible after expiry');
  assert.equal((await db.query('select event from realtime.test_events')).rows[0].event,'COMMAND_CHANGED');
  await db.exec(`update device_commands set requested_at=now()-interval '16 minutes'`);
  assert.equal((await read())[0].command,null,'feedback retention is bounded');
  await db.exec('set role service_role');
  const report = async (id,lat=51.9,lon=-2.2,applied=0) =>
    (await db.query("select * from bluepaws_record_hub_presence($1,'home',$2,$3,5,123,-45,true,true,100000,$4)",[id,lat,lon,applied])).rows[0];
  let hub=await report(16); await report(32);
  assert.equal(hub.home_emoji,'🏡'); assert.equal(hub.portable_emoji,'📱');
  const fix=hub.fix_at;
  hub=await report(16,null,null);
  assert.equal(hub.latitude,51.9); assert.deepEqual(hub.fix_at,fix,'no-fix heartbeat preserves location age');
  await assert.rejects(report(48),/Gateway unavailable/);
  await db.exec('reset role; set role authenticated');
  assert.equal((await db.query('select * from hub_presence')).rows.length,1,'Family isolation');
  await db.exec("update hub_presence set home_emoji='🐈',desired_ble_enabled=false where gateway_guid16=16");
  hub=(await db.query('select * from hub_presence')).rows[0];
  assert.equal(hub.settings_revision,2,'preferences get a delivery revision');
  await assert.rejects(db.exec('update hub_presence set latitude=1'),/permission denied/);
  await assert.rejects(report(16),/permission denied/);
  await db.exec('reset role; set role service_role');
  hub=await report(16,null,null,2);
  assert.equal(hub.home_emoji,'🐈','telemetry cannot overwrite preferences');
  assert.equal(hub.applied_revision,2); assert.equal(hub.desired_ble_enabled,false);
  await db.exec(`reset role; delete from household_members where user_id='${family}'; set role authenticated`);
  assert.equal((await db.query('select * from hub_presence')).rows.length,0,'revocation takes effect');
  await db.exec('reset role; set role anon');
  await assert.rejects(db.query('select * from bluepaws_collar_feedback($1)',[family]),/permission denied/);
  console.log('PASS: feedback SQL, live/replay/duplicate/clock/fault/retention and invoker RLS isolation');
} catch(e) { console.error('FAIL:',e.message); process.exitCode=1; } finally { await db.close(); }
