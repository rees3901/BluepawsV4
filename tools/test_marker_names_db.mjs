// Real migration/RLS tests in isolated Postgres WASM; no hosted data is touched.
import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import {PGlite} from '../.pio/feedback-tests/node_modules/@electric-sql/pglite/dist/index.js';
const db = new PGlite();
const family='00000000-0000-0000-0000-000000000001', other='00000000-0000-0000-0000-000000000002';
const member='00000000-0000-0000-0000-000000000003', guest='00000000-0000-0000-0000-000000000004';
const migration=name=>readFileSync(new URL('../supabase/migrations/'+name,import.meta.url),'utf8');
try {
  await db.exec(`
    create role authenticated; create role anon; create role service_role bypassrls;
    create schema auth; create schema private; create schema storage; create schema realtime;
    create table auth.users(id uuid primary key);
    insert into auth.users values('${family}'),('${other}'),('${member}'),('${guest}');
    create function auth.uid() returns uuid language sql as $$ select nullif(current_setting('test.user',true),'')::uuid $$;
    grant usage on schema auth to authenticated;
    create table public.households(id uuid primary key,access_version integer,name text);
    insert into households values('${family}',1,'Family A'),('${other}',1,'Family B');
    create table public.household_members(household_id uuid,user_id uuid,role text);
    insert into household_members values('${family}','${family}','owner'),('${family}','${member}','member'),
      ('${other}','${other}','owner'),('${family}','${guest}','guest_viewer');
    alter table household_members enable row level security;
    create policy own_memberships on household_members for select to authenticated using(user_id=auth.uid());
    grant select on household_members to authenticated;
    create table public.devices(device_id integer primary key,household_id uuid,display_name text not null,
      last_seen_at timestamptz default '2026-08-27T12:00:00Z',last_seen_status_code integer default 0,
      last_seen_power_profile_code integer default 1,last_seen_tx_reason integer default 7,last_seen_battery_mv integer default 3900);
    insert into devices(device_id,household_id,display_name) values(1001,'${family}','Device 1001'),(2001,'${other}','Device 2001');
    alter table devices enable row level security;
    create policy read_devices on devices for select to authenticated using(exists(
      select 1 from household_members m where m.household_id=devices.household_id and m.user_id=auth.uid()));
    grant select on devices to authenticated;
    create table storage.buckets(id text primary key,name text,public boolean,file_size_limit bigint,allowed_mime_types text[]);
    create table storage.objects(bucket_id text,name text);
    alter table storage.objects enable row level security;
    create function storage.foldername(text) returns text[] language sql immutable as $$ select string_to_array($1,'/') $$;
    create table realtime.events(event text,record jsonb);
    create function realtime.broadcast_changes(text,text,text,text,text,record,record) returns void language plpgsql
      as $$ begin insert into realtime.events values($2,to_jsonb($6)); end; $$;
    create table public.family_search_shares(id uuid,household_id uuid,token_hash bytea,revoked_at timestamptz,
      expires_at timestamptz,last_used_at timestamptz,use_count integer);
    -- Identity stub for digest: tests token selection/isolation, not SHA-256 itself.
    create schema extensions;
    create function extensions.digest(bytea,text) returns bytea language sql immutable as $$ select $1 $$;
    create table public.device_latest_positions(position_id bigint,device_uid integer,household_id uuid,
      message_id integer,latitude float,longitude float,battery integer,battery_mv integer,status_code integer,
      power_profile_code integer,flags integer,tx_reason integer,ingest_path text,link_type text,
      link_rssi_dbm float,link_snr_db float,source text,recorded_at timestamptz,received_at timestamptz,schema_version integer);
    insert into device_latest_positions(device_uid,household_id) values(1001,'${family}'),(2001,'${other}');
  `);
  await db.exec(migration('20260812120000_add_device_appearances.sql'));
  await db.exec(migration('20260824093721_broadcast_device_presence.sql'));
  await db.exec(migration('20260828084628_add_marker_friendly_names.sql'));
  const runAs=async(user,sql,params=[],role='authenticated')=>{
    await db.exec('begin');
    try {
      await db.query("select set_config('test.user',$1,true)",[user]);
      await db.exec('set local role '+role);
      const result=await db.query(sql,params);
      await db.exec('commit');return result;
    }catch(e){await db.exec('rollback');throw e;}
  };
  const save=(user=family,name='Mittens',colour='#0099ff',id=1001,house=family,path=null)=>
    runAs(user,'select public.bluepaws_save_device_marker($1,$2,$3,$4,$5,$6,$7)',
      [id,house,name,path?'photo':'emoji','🐱',colour,path]);
  await save(family,'  Mittens  ');
  assert.equal((await db.query('select display_name from devices where device_id=1001')).rows[0].display_name,'Mittens');
  assert.equal((await db.query('select count(*)::int n from device_appearances')).rows[0].n,1);
  const event=(await db.query("select record from realtime.events where event='DEVICE_PRESENCE'")).rows[0].record;
  assert.equal(event.display_name,'Mittens'); assert.equal(event.last_seen_battery_mv,3900);
  assert.ok(event.last_seen_at.startsWith('2026-08-27T12:00:00'));
  await save(member,"Mittens & O'Paws 🐈");
  for(const operation of [
    ()=>save(other),()=>save(guest),()=>save(family,'Wrong','#0099ff',2001,other),
    ()=>save(family,'No device','#0099ff',9999),()=>save(family,'Bad colour','invalid'),
    ()=>save(family,'Bad photo','#0099ff',1001,family,other+'/2001/photo.webp'),
    ()=>save(family,''),()=>save(family,'   '),()=>save(family,'x'.repeat(81)),()=>save(family,'line\nname'),
    ()=>runAs('','select public.bluepaws_save_device_marker(1001,$1,$2,$3,$4,$5,null)',[family,'Anon','emoji','🐱','#0099ff'],'anon'),
    ()=>runAs(family,'update devices set device_id=999 where device_id=1001'),
    ()=>runAs(family,'update devices set household_id=$1 where device_id=1001',[other]),
  ]) await assert.rejects(operation);
  assert.equal((await db.query('select display_name from devices where device_id=1001')).rows[0].display_name,"Mittens & O'Paws 🐈",'failed saves roll back the name');
  assert.equal((await db.query('select marker_colour from device_appearances where device_id=1001')).rows[0].marker_colour,'#0099ff');
  await db.exec(`delete from household_members where user_id='${member}'`);
  await assert.rejects(()=>save(member));
  await db.exec('update devices set last_seen_at=now() where device_id=1001');
  assert.equal((await db.query('select display_name from devices where device_id=1001')).rows[0].display_name,"Mittens & O'Paws 🐈");
  // Home distance projection shares the existing marker/search-party fixtures.
  await db.exec(`
    alter table device_latest_positions add column observation_id bigint;
    create table observations(id bigint primary key,household_id uuid,destination_id16 integer);
    create table hub_presence(gateway_guid16 integer primary key,household_id uuid,mode text,
      latitude float,longitude float,fix_at timestamptz);
    insert into observations values(1,'${family}',16),(2,'${other}',32);
    update device_latest_positions set observation_id=case when device_uid=1001 then 1 else 2 end;
    insert into hub_presence values(16,'${family}','home',51.9059,-2.2395,now()),(32,'${other}','home',10,20,now());
    alter table device_latest_positions enable row level security;
    alter table observations enable row level security;
    alter table hub_presence enable row level security;
  `);
  for(const table of ['device_latest_positions','observations','hub_presence']) await db.exec(`
    create policy family_read on ${table} for select to authenticated using(exists(
      select 1 from household_members m where m.household_id=${table}.household_id and m.user_id=auth.uid()));
    grant select on ${table} to authenticated;
  `);
  await db.exec(migration('20260828203228_home_hub_distance.sql'));
  const home=async()=> (await runAs(family,'select home_hub_id,home_latitude from device_latest_positions_with_home')).rows;
  assert.deepEqual(await home(),[{home_hub_id:16,home_latitude:51.9059}]);
  assert.equal((await runAs(other,'select device_uid from device_latest_positions_with_home')).rows[0].device_uid,2001);
  await assert.rejects(()=>runAs('','select * from device_latest_positions_with_home',[],'anon'));
  await assert.rejects(()=>runAs(family,'update device_latest_positions_with_home set home_latitude=99'));
  await db.exec('update observations set destination_id16=32 where id=1');
  assert.equal((await home())[0].home_hub_id,null,'foreign hub cannot be selected or replaced by a guessed hub');
  await db.exec('update observations set destination_id16=0 where id=1');
  assert.equal((await home())[0].home_hub_id,16,'cloud-addressed packet uses the single Family home hub');
  await db.exec(`insert into hub_presence values(48,'${family}','home',30,40,now())`);
  assert.equal((await home())[0].home_hub_id,null,'multiple home hubs are ambiguous');
  await db.exec('update observations set destination_id16=16 where id=1');
  assert.equal((await home())[0].home_hub_id,16,'explicit destination wins even with multiple hubs');
  await db.exec('update hub_presence set latitude=51.91 where gateway_guid16=16');
  assert.equal((await home())[0].home_latitude,51.91,'hub movement updates old reports without ingest');
  await db.exec('update hub_presence set latitude=null,longitude=null,fix_at=null where gateway_guid16=16');
  assert.equal((await home())[0].home_latitude,null,'missing hub fix stays unknown');
  await db.exec('delete from hub_presence where gateway_guid16=48');
  await db.exec('update observations set destination_id16=null where id=1');
  assert.equal((await home())[0].home_hub_id,16,'legacy destination uses the unambiguous home');
  await db.exec("update hub_presence set mode='portable' where gateway_guid16=16");
  assert.equal((await home())[0].home_hub_id,null,'legacy packet cannot assume a portable hub is home');
  await db.exec('update observations set destination_id16=16 where id=1');
  assert.equal((await home())[0].home_hub_id,16,'explicit logical hub still works in portable mode');
  console.log('PASS: home-hub projection, transport independence, moving/missing fixes, ambiguous and foreign hubs, invoker RLS and denied anonymous/writes.');
  const token='a'.repeat(64);
  await db.query("insert into family_search_shares values($1,$1,convert_to($2,'UTF8'),null,now()+interval '1 hour',null,0)",[family,token]);
  const snapshot=async value=>(await db.query('select private.bluepaws_get_search_party_snapshot($1) snapshot',[value])).rows[0].snapshot;
  const shared=await snapshot(token);
  assert.equal(shared.valid,true);assert.equal(shared.devices.length,1);
  assert.equal(shared.devices[0].home_hub_id,16);assert.equal(shared.devices[0].home_latitude,null);
  assert.equal(shared.devices[0].device_uid,1001);assert.equal(shared.devices[0].display_name,"Mittens & O'Paws 🐈");
  assert.equal((await snapshot('bad')).valid,false);
  assert.equal((await snapshot('b'.repeat(64))).valid,false);
  await db.exec('update family_search_shares set revoked_at=now()');
  assert.equal((await snapshot(token)).valid,false);
  await db.exec("update family_search_shares set revoked_at=null,expires_at=now()-interval '1 second'");
  assert.equal((await snapshot(token)).valid,false);
  console.log('PASS: Family-scoped atomic marker saves; names/identity/presence preserved; guest, revoked, foreign, invalid and anonymous writes rejected.');
}finally{await db.close();}
