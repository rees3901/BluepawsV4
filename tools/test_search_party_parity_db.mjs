// Isolated Postgres WASM verification for Search Party token scope and payload.
import { readFileSync } from "node:fs";
import assert from "node:assert/strict";
import { PGlite } from "../.pio/feedback-tests/node_modules/@electric-sql/pglite/dist/index.js";

const db = new PGlite();
const family = "00000000-0000-0000-0000-000000000001";
const other = "00000000-0000-0000-0000-000000000002";
const token = "a".repeat(64);
const migration = readFileSync(new URL("../supabase/migrations/20260901143000_search_party_map_parity.sql", import.meta.url), "utf8");

try {
  await db.exec(`
    create role anon; create role authenticated; create role service_role bypassrls;
    create schema extensions; create schema private;
    create function extensions.digest(bytea,text) returns bytea language sql immutable as $$ select $1 $$;
    create table households(id uuid primary key,name text not null);
    insert into households values('${family}','Family A'),('${other}','Family B');
    create table family_search_shares(id uuid primary key,household_id uuid,token_hash bytea,created_by uuid,
      created_at timestamptz,expires_at timestamptz,revoked_at timestamptz,last_used_at timestamptz,use_count integer);
    insert into family_search_shares values('${family}','${family}',convert_to('${token}','UTF8'),'${family}',now(),now()+interval '1 hour',null,null,0);
    create table devices(device_id integer,household_id uuid,display_name text,primary key(device_id));
    insert into devices values(1001,'${family}','Simba'),(2001,'${other}','Other pet');
    create table device_appearances(device_id integer,household_id uuid,avatar_kind text,emoji_value text,
      marker_colour text,avatar_storage_path text);
    insert into device_appearances values
      (1001,'${family}','photo','🐈','#ffaa00','${family}/1001/00000000-0000-0000-0000-000000000010.webp'),
      (2001,'${other}','photo','🐕','#ff0000','${other}/2001/00000000-0000-0000-0000-000000000020.webp');
    create table device_latest_positions_with_home(position_id bigint,device_uid integer,household_id uuid,message_id integer,
      latitude float,longitude float,battery integer,battery_mv integer,status_code integer,power_profile_code integer,
      flags integer,tx_reason integer,ingest_path text,link_type text,link_rssi_dbm float,link_snr_db float,source text,
      recorded_at timestamptz,received_at timestamptz,schema_version integer,home_hub_id integer,
      home_latitude float,home_longitude float,home_fix_at timestamptz);
    insert into device_latest_positions_with_home values
      (1,1001,'${family}',5,51.86,-2.24,95,4170,1,0,1,0,'lora_hub','lora',-72,9,'edge-api',now(),now(),2,16,51.865,-2.238,now()),
      (2,2001,'${other}',1,10,20,90,4100,1,1,1,0,'cellular_direct','lte',-80,5,'edge-api',now(),now(),2,null,null,null,null);
    create table device_latest_positions(device_uid integer,household_id uuid);
    insert into device_latest_positions values(1001,'${family}'),(2001,'${other}');
    create table positions(id bigint,device_uid integer,household_id uuid,message_id integer,latitude float,longitude float,recorded_at timestamptz);
    insert into positions select n,1001,'${family}',n,51.86+n/10000.0,-2.24,now()-make_interval(mins=>6-n)
      from generate_series(1,6) n;
    insert into positions values(20,1001,'${family}',20,1,1,now()-interval '8 days');
    create table hub_presence(gateway_guid16 integer primary key,household_id uuid,display_name text,mode text,
      received_at timestamptz,latitude float,longitude float,fix_at timestamptz,avatar_kind text,avatar_storage_path text,
      home_emoji text,portable_emoji text,marker_colour text,wifi_rssi_dbm integer,ble_enabled boolean,settings_revision bigint);
    insert into hub_presence values
      (16,'${family}','Home Hub','portable',now(),51.865,-2.238,now(),'photo','${family}/16/00000000-0000-0000-0000-000000000016.webp','🏡','📱','#38bdf8',-42,true,9),
      (32,'${other}','Other Hub','home',now(),10,20,now(),'emoji',null,'🏡','📱','#ff0000',-40,true,2);
    create function private.bluepaws_get_search_party_snapshot(text) returns jsonb language sql as $$ select '{}'::jsonb $$;
    create function public.bluepaws_get_search_party_snapshot(share_token text) returns jsonb language sql security definer
      set search_path='' as $$ select private.bluepaws_get_search_party_snapshot(share_token) $$;
  `);
  await db.exec(migration);

  const snapshot = (await db.query("select public.bluepaws_get_search_party_snapshot($1) snapshot", [token])).rows[0].snapshot;
  assert.equal(snapshot.valid, true);
  assert.equal(snapshot.devices.length, 1);
  assert.equal(snapshot.devices[0].display_name, "Simba");
  assert.equal(snapshot.devices[0].avatar_storage_path, undefined);
  assert.equal(snapshot.trails[1001].length, 4);
  assert(snapshot.trails[1001][0].lat < snapshot.trails[1001][3].lat, "trail is chronological");
  assert.equal(snapshot.hubs.length, 1);
  assert.equal(snapshot.hubs[0].mode, "portable");
  assert.equal(snapshot.hubs[0].wifi_rssi_dbm, undefined);
  assert.equal(snapshot.hubs[0].settings_revision, undefined);
  assert.equal(snapshot.hubs[0].avatar_storage_path, undefined);

  await db.exec("set role anon");
  await assert.rejects(db.query("select * from public.bluepaws_resolve_search_party_avatar($1,'collar',1001)", [token]), /permission denied/i);
  await db.exec("reset role");
  await db.exec("set role service_role");
  const collar = await db.query("select * from public.bluepaws_resolve_search_party_avatar($1,'collar',1001)", [token]);
  const hub = await db.query("select * from public.bluepaws_resolve_search_party_avatar($1,'hub',16)", [token]);
  const foreign = await db.query("select * from public.bluepaws_resolve_search_party_avatar($1,'collar',2001)", [token]);
  assert.deepEqual(collar.rows[0], { bucket: "pet-avatars", object_path: `${family}/1001/00000000-0000-0000-0000-000000000010.webp` });
  assert.deepEqual(hub.rows[0], { bucket: "hub-avatars", object_path: `${family}/16/00000000-0000-0000-0000-000000000016.webp` });
  assert.equal(foreign.rows.length, 0);
  await db.exec("reset role");
  await db.exec("update family_search_shares set revoked_at=now()");
  assert.equal((await db.query("select public.bluepaws_get_search_party_snapshot($1) snapshot", [token])).rows[0].snapshot.valid, false);
  console.log("PASS: Search Party snapshot limits trails, exposes safe Home Hub bearing, and keeps avatar paths token/service-role scoped.");
} finally {
  await db.close();
}
