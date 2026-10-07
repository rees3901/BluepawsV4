// Isolated PostgreSQL upgrade check; no network or live credentials.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { PGlite } from '../.pio/feedback-tests/node_modules/@electric-sql/pglite/dist/index.js';
const db = new PGlite();
const migration = name => readFileSync(new URL(`../supabase/migrations/${name}`, import.meta.url), 'utf8');
const original = migration('20261007120703_hub_ble_advertising_power.sql');
try {
  await db.exec(`
    create role anon; create role authenticated; create role service_role;
    create schema private;
    create table public.gateways(gateway_guid16 integer primary key, household_id uuid, display_name text, enabled boolean);
    insert into public.gateways values(48,'00000000-0000-0000-0000-000000000001','Test hub',true);
    create table public.hub_presence(
      gateway_guid16 integer primary key, household_id uuid, mode text,
      received_at timestamptz default now(), latitude double precision, longitude double precision,
      fix_at timestamptz, uptime_s bigint, wifi_rssi_dbm integer, ble_enabled boolean,
      ble_advertising boolean, free_heap integer, display_name text, home_emoji text,
      portable_emoji text, marker_colour text, desired_ble_enabled boolean default true,
      settings_revision bigint default 0, applied_revision bigint,
      reporting_profile text, desired_reporting_profile text, control_poll_s integer,
      battery_percent integer, position_simulated boolean, battery_simulated boolean,
      gnss_valid boolean, sat_count integer, acc_m double precision, hdop double precision);
    create function private.bluepaws_hub_preferences_revision() returns trigger language plpgsql as $$
      begin new.settings_revision=old.settings_revision+1; return new; end $$;
    create trigger hub_preferences_revision before update of desired_ble_enabled on public.hub_presence
      for each row execute function private.bluepaws_hub_preferences_revision();
  `);
  // Create only the superseded signature needed by the original upgrade.
  const signature = original.match(/drop function public\.bluepaws_record_hub_presence\(([\s\S]*?)\);/)[1];
  await db.exec(`create function public.bluepaws_record_hub_presence(${signature}) returns setof public.hub_presence language sql as $$ select * from public.hub_presence $$;`);
  await db.exec(original);
  const report = (extra='') => db.query(`select ble_tx_power_dbm${extra.includes('p_ble_power_steps')?',ble_power_steps':''} from public.bluepaws_record_hub_presence(48,'home',null,null,null,100,-50,false,false,100000,11${extra});`);
  await report(',p_ble_tx_power_dbm=>3');
  await db.exec(migration('20261007210402_hub_ble_five_power_levels.sql'));
  assert.equal((await db.query('select ble_power_steps from public.hub_presence')).rows[0].ble_power_steps,3);
  for (const power of [-12,-6,3,6,9]) {
    const r=await report(`,p_ble_tx_power_dbm=>${power},p_ble_power_steps=>5`);
    assert.deepEqual(r.rows[0],{ble_tx_power_dbm:power,ble_power_steps:5});
  }
  for (const power of [-6,6,12]) await assert.rejects(report(`,p_ble_tx_power_dbm=>${power}`));
  await assert.rejects(report(',p_ble_tx_power_dbm=>3,p_ble_power_steps=>4'));
  await report(',p_ble_tx_power_dbm=>3');
  assert.equal((await db.query('select ble_power_steps from public.hub_presence')).rows[0].ble_power_steps,3);
  await report();
  assert.equal((await db.query('select ble_power_steps from public.hub_presence')).rows[0].ble_power_steps,null);
  const permissions=(await db.query(`select
    has_column_privilege('authenticated','public.hub_presence','desired_ble_tx_power_dbm','UPDATE') as desired,
    has_column_privilege('authenticated','public.hub_presence','ble_power_steps','UPDATE') as capability,
    has_function_privilege('authenticated',(select oid from pg_proc where proname='bluepaws_record_hub_presence'),'EXECUTE') as report`)).rows[0];
  assert.deepEqual(permissions,{desired:true,capability:false,report:false});
  await db.exec('update public.hub_presence set desired_ble_tx_power_dbm=6');
  assert.equal((await db.query('select settings_revision from public.hub_presence')).rows[0].settings_revision,1);
  console.log('BLE power migration passed: old/new reports, all five powers, invalid rejection, permissions and revision.');
} finally { await db.close(); }
