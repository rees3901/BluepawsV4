-- Additive, backwards-compatible foundation for household ownership and
-- private Supabase Realtime Broadcast delivery. The existing public view and
-- temporary anonymous position policy remain until the separate cutover.

set lock_timeout = '5s';
set statement_timeout = '120s';

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;

create table public.households (
  id uuid primary key default gen_random_uuid(),
  name text not null,
  kind text not null default 'customer',
  created_by uuid references auth.users(id) on delete set null,
  created_at timestamptz not null default now(),
  constraint households_name_length check (length(btrim(name)) between 1 and 80),
  constraint households_kind_check check (kind in ('customer', 'test', 'quarantine'))
);

create unique index households_single_test_idx
  on public.households (kind)
  where kind = 'test';

create unique index households_single_quarantine_idx
  on public.households (kind)
  where kind = 'quarantine';

create table public.household_members (
  household_id uuid not null references public.households(id) on delete cascade,
  user_id uuid not null references auth.users(id) on delete cascade,
  role text not null default 'member',
  joined_at timestamptz not null default now(),
  primary key (household_id, user_id),
  constraint household_members_role_check check (role in ('owner', 'member'))
);

create index household_members_user_household_idx
  on public.household_members (user_id, household_id);

insert into public.households (name, kind)
values
  ('Bluepaws Test Household', 'test'),
  ('Unassigned Legacy Devices', 'quarantine')
on conflict do nothing;

alter table public.devices
  add column household_id uuid references public.households(id);

do $$
declare
  test_household_id uuid;
  quarantine_household_id uuid;
begin
  select id into strict test_household_id
  from public.households
  where kind = 'test';

  select id into strict quarantine_household_id
  from public.households
  where kind = 'quarantine';

  update public.devices
  set household_id = case
    when device_id between 1001 and 1005 then test_household_id
    else quarantine_household_id
  end
  where household_id is null;
end
$$;

alter table public.devices alter column household_id set not null;
create index devices_household_id_idx on public.devices (household_id, device_id);

alter table public.positions
  add column household_id uuid references public.households(id);

update public.positions as position
set household_id = device.household_id
from public.devices as device
where device.device_id = position.device_uid
  and position.household_id is null;

alter table public.positions alter column household_id set not null;

create index positions_household_device_recorded_idx
  on public.positions (household_id, device_uid, recorded_at desc, message_id desc);

alter table public.devices drop constraint if exists devices_device_id_range;
alter table public.devices
  add constraint devices_device_id_range check (device_id between 1 and 65535);

alter table public.positions drop constraint if exists positions_device_uid_range;
alter table public.positions
  add constraint positions_device_uid_range check (device_uid between 1 and 65535);

create table public.device_latest_positions (
  device_uid integer primary key references public.devices(device_id) on delete cascade,
  household_id uuid not null references public.households(id) on delete cascade,
  position_id bigint not null,
  message_id integer not null,
  latitude double precision not null,
  longitude double precision not null,
  battery integer,
  source text not null,
  recorded_at timestamptz not null,
  received_at timestamptz not null,
  schema_version smallint not null,
  constraint latest_positions_device_uid_range check (device_uid between 1 and 65535),
  constraint latest_positions_battery_range check (battery is null or battery between 0 and 100)
);

create index device_latest_positions_household_device_idx
  on public.device_latest_positions (household_id, device_uid);

insert into public.device_latest_positions (
  device_uid,
  household_id,
  position_id,
  message_id,
  latitude,
  longitude,
  battery,
  source,
  recorded_at,
  received_at,
  schema_version
)
select distinct on (device_uid)
  device_uid,
  household_id,
  id,
  message_id,
  latitude,
  longitude,
  battery,
  source,
  recorded_at,
  received_at,
  schema_version
from public.positions
order by device_uid, recorded_at desc, message_id desc;

create or replace function private.set_position_household()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  select device.household_id
  into new.household_id
  from public.devices as device
  where device.device_id = new.device_uid
    and device.enabled = true;

  if new.household_id is null then
    raise exception 'device is disabled or has no household';
  end if;

  return new;
end;
$$;

create or replace function private.sync_latest_position()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  insert into public.device_latest_positions (
    device_uid,
    household_id,
    position_id,
    message_id,
    latitude,
    longitude,
    battery,
    source,
    recorded_at,
    received_at,
    schema_version
  )
  values (
    new.device_uid,
    new.household_id,
    new.id,
    new.message_id,
    new.latitude,
    new.longitude,
    new.battery,
    new.source,
    new.recorded_at,
    new.received_at,
    new.schema_version
  )
  on conflict (device_uid) do update
  set
    household_id = excluded.household_id,
    position_id = excluded.position_id,
    message_id = excluded.message_id,
    latitude = excluded.latitude,
    longitude = excluded.longitude,
    battery = excluded.battery,
    source = excluded.source,
    recorded_at = excluded.recorded_at,
    received_at = excluded.received_at,
    schema_version = excluded.schema_version
  where
    excluded.recorded_at > public.device_latest_positions.recorded_at
    or (
      excluded.recorded_at = public.device_latest_positions.recorded_at
      and excluded.message_id > public.device_latest_positions.message_id
    );

  return new;
end;
$$;

create or replace function private.broadcast_latest_position()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  perform realtime.broadcast_changes(
    'household:' || new.household_id::text,
    tg_op,
    tg_op,
    tg_table_name,
    tg_table_schema,
    new,
    old
  );
  return null;
end;
$$;

revoke execute on function private.set_position_household() from public, anon, authenticated;
revoke execute on function private.sync_latest_position() from public, anon, authenticated;
revoke execute on function private.broadcast_latest_position() from public, anon, authenticated;

drop trigger if exists positions_set_household on public.positions;
create trigger positions_set_household
  before insert on public.positions
  for each row execute function private.set_position_household();

drop trigger if exists positions_sync_latest on public.positions;
create trigger positions_sync_latest
  after insert on public.positions
  for each row execute function private.sync_latest_position();

drop trigger if exists device_latest_positions_broadcast on public.device_latest_positions;
create trigger device_latest_positions_broadcast
  after insert or update on public.device_latest_positions
  for each row execute function private.broadcast_latest_position();

create or replace function private.handle_new_bluepaws_user()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
  target_household_id uuid;
  household_name text;
begin
  perform pg_advisory_xact_lock(hashtext('bluepaws-first-test-household-owner'));

  select household.id
  into target_household_id
  from public.households as household
  where household.kind = 'test'
    and not exists (
      select 1
      from public.household_members as member
      where member.household_id = household.id
    )
  limit 1;

  if target_household_id is null then
    household_name := coalesce(
      nullif(btrim(new.raw_user_meta_data ->> 'full_name'), ''),
      split_part(coalesce(new.email, 'Bluepaws'), '@', 1)
    ) || '''s household';

    insert into public.households (name, kind, created_by)
    values (left(household_name, 80), 'customer', new.id)
    returning id into target_household_id;
  else
    update public.households
    set created_by = new.id
    where id = target_household_id;
  end if;

  insert into public.household_members (household_id, user_id, role)
  values (target_household_id, new.id, 'owner');

  return new;
end;
$$;

revoke execute on function private.handle_new_bluepaws_user() from public, anon, authenticated;

drop trigger if exists on_auth_user_created_bluepaws on auth.users;
create trigger on_auth_user_created_bluepaws
  after insert on auth.users
  for each row execute function private.handle_new_bluepaws_user();

alter table public.households enable row level security;
alter table public.household_members enable row level security;
alter table public.device_latest_positions enable row level security;

drop policy if exists "Registry is server-only" on public.devices;
drop policy if exists "Household members read households" on public.households;
create policy "Household members read households"
  on public.households for select to authenticated
  using (
    exists (
      select 1 from public.household_members as member
      where member.household_id = households.id
        and member.user_id = (select auth.uid())
    )
  );

drop policy if exists "Members read own memberships" on public.household_members;
create policy "Members read own memberships"
  on public.household_members for select to authenticated
  using (user_id = (select auth.uid()));

drop policy if exists "Household members read devices" on public.devices;
create policy "Household members read devices"
  on public.devices for select to authenticated
  using (
    exists (
      select 1 from public.household_members as member
      where member.household_id = devices.household_id
        and member.user_id = (select auth.uid())
    )
  );

drop policy if exists "Household members read position history" on public.positions;
create policy "Household members read position history"
  on public.positions for select to authenticated
  using (
    exists (
      select 1 from public.household_members as member
      where member.household_id = positions.household_id
        and member.user_id = (select auth.uid())
    )
  );

drop policy if exists "Household members read latest positions" on public.device_latest_positions;
create policy "Household members read latest positions"
  on public.device_latest_positions for select to authenticated
  using (
    exists (
      select 1 from public.household_members as member
      where member.household_id = device_latest_positions.household_id
        and member.user_id = (select auth.uid())
    )
  );

drop policy if exists "Household members receive position broadcasts" on realtime.messages;
create policy "Household members receive position broadcasts"
  on realtime.messages for select to authenticated
  using (
    extension = 'broadcast'
    and exists (
      select 1 from public.household_members as member
      where member.user_id = (select auth.uid())
        and topic = 'household:' || member.household_id::text
    )
  );

revoke all on table public.households from anon, authenticated;
revoke all on table public.household_members from anon, authenticated;
revoke all on table public.devices from anon, authenticated;
revoke all on table public.positions from anon, authenticated;
revoke all on table public.device_latest_positions from anon, authenticated;

grant select on table public.households to authenticated;
grant select on table public.household_members to authenticated;
grant select on table public.devices to authenticated;
grant select on table public.positions to authenticated;
grant select on table public.device_latest_positions to authenticated;

grant select on table public.devices to service_role;
grant select on table public.device_ingest_credentials to service_role;
grant select, insert on table public.positions to service_role;
grant select on table public.device_latest_positions to service_role;

comment on table public.households is 'Customer tenancy boundary for Bluepaws users and devices.';
comment on table public.device_latest_positions is 'Maintained current position per device; changes are broadcast to its private household channel.';
