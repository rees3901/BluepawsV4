-- Hub-originated JSON is separate from authenticated collar TLV.
create table public.hub_presence (
  gateway_guid16 integer primary key references public.gateways(gateway_guid16) on delete cascade,
  household_id uuid not null references public.households(id) on delete cascade,
  mode text not null check(mode in ('home','portable','off_grid')),
  received_at timestamptz not null default now(),
  latitude double precision,
  longitude double precision,
  fix_at timestamptz,
  uptime_s bigint not null check(uptime_s between 0 and 4294967295),
  wifi_rssi_dbm integer check(wifi_rssi_dbm between -127 and 0),
  ble_enabled boolean not null,
  ble_advertising boolean not null,
  free_heap integer not null check(free_heap >= 0),
  display_name text not null default 'Home Hub' check(length(display_name) between 1 and 64),
  home_emoji text not null default '🏡' check(length(home_emoji) between 1 and 16),
  portable_emoji text not null default '📱' check(length(portable_emoji) between 1 and 16),
  marker_colour text not null default '#38bdf8' check(marker_colour ~ '^#[0-9a-fA-F]{6}$'),
  desired_ble_enabled boolean not null default true,
  settings_revision bigint not null default 1,
  applied_revision bigint not null default 0,
  constraint hub_text_storage check (
    octet_length(display_name)<=64 and octet_length(home_emoji)<=64 and octet_length(portable_emoji)<=64
    and display_name !~ '[[:cntrl:]]' and home_emoji !~ '[[:cntrl:]]' and portable_emoji !~ '[[:cntrl:]]'),
  constraint hub_fix_pair check (
    (latitude is null and longitude is null and fix_at is null)
    or (latitude is not null and longitude is not null and fix_at is not null
      and latitude between -90 and 90 and longitude between -180 and 180))
);
create index hub_presence_household_idx on public.hub_presence(household_id);
alter table public.hub_presence enable row level security;
revoke all on public.hub_presence from public, anon, authenticated;
grant select on public.hub_presence to authenticated;
grant update(display_name,home_emoji,portable_emoji,marker_colour,desired_ble_enabled) on public.hub_presence to authenticated;
grant select,insert,update on public.hub_presence to service_role;
create policy "Family reads own hubs" on public.hub_presence for select to authenticated
using(exists(select 1 from public.household_members m where m.household_id=hub_presence.household_id and m.user_id=(select auth.uid()) and m.role in ('owner','member')));
create policy "Family edits hub preferences only" on public.hub_presence for update to authenticated
using(exists(select 1 from public.household_members m where m.household_id=hub_presence.household_id and m.user_id=(select auth.uid()) and m.role in ('owner','member')))
with check(exists(select 1 from public.household_members m where m.household_id=hub_presence.household_id and m.user_id=(select auth.uid()) and m.role in ('owner','member')));

-- Invoker trigger only assigns NEW; no privileged data access.
create function private.bluepaws_hub_preferences_revision() returns trigger
language plpgsql security invoker set search_path='' as $$
begin
  new.settings_revision := old.settings_revision + 1;
  return new;
end $$;
revoke all on function private.bluepaws_hub_preferences_revision() from public,anon,authenticated;
create trigger hub_preferences_revision before update of
display_name,home_emoji,portable_emoji,marker_colour,desired_ble_enabled on public.hub_presence
for each row execute function private.bluepaws_hub_preferences_revision();

-- Called exclusively after gateway bearer authentication in ingest-position.
-- Family is resolved from the gateway, never trusted from JSON input.
create function public.bluepaws_record_hub_presence(
  p_gateway integer, p_mode text, p_lat double precision, p_lon double precision,
  p_fix_age_s integer, p_uptime bigint, p_rssi integer, p_ble boolean,
  p_advertising boolean, p_heap integer, p_applied bigint
) returns setof public.hub_presence
language plpgsql security invoker set search_path='' as $$
declare family uuid; hub_name text;
begin
  select g.household_id,g.display_name into family,hub_name from public.gateways g
    where g.gateway_guid16=p_gateway and g.enabled;
  if family is null then raise exception using errcode='42501',message='Gateway unavailable'; end if;
  if (p_lat is null) <> (p_lon is null)
    or (p_lat is not null and (p_fix_age_s is null or p_fix_age_s not between 0 and 604800))
    or p_applied is null or p_applied < 0
  then raise exception using errcode='22023',message='Invalid hub report'; end if;
  return query insert into public.hub_presence as h
    (gateway_guid16,household_id,mode,latitude,longitude,fix_at,uptime_s,wifi_rssi_dbm,
      ble_enabled,ble_advertising,free_heap,display_name,applied_revision)
    values(p_gateway,family,p_mode,p_lat,p_lon,
      case when p_lat is not null then now()-make_interval(secs=>p_fix_age_s) end,
      p_uptime,p_rssi,p_ble,p_advertising,p_heap,coalesce(nullif(hub_name,''),'Home Hub'),p_applied)
    on conflict(gateway_guid16) do update set
      household_id=excluded.household_id, mode=excluded.mode, received_at=now(),
      latitude=case when h.household_id<>excluded.household_id then excluded.latitude else coalesce(excluded.latitude,h.latitude) end,
      longitude=case when h.household_id<>excluded.household_id then excluded.longitude else coalesce(excluded.longitude,h.longitude) end,
      fix_at=case when h.household_id<>excluded.household_id then excluded.fix_at else coalesce(excluded.fix_at,h.fix_at) end,
      uptime_s=excluded.uptime_s,wifi_rssi_dbm=excluded.wifi_rssi_dbm,
      ble_enabled=excluded.ble_enabled,ble_advertising=excluded.ble_advertising,
      free_heap=excluded.free_heap,applied_revision=excluded.applied_revision
    returning h.*;
end $$;
revoke all on function public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint) from public,anon,authenticated;
grant execute on function public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint) to service_role;

-- Read-only Family snapshot. Invoker RLS applies independently to every table.
-- No raw TLV, credentials, reset-reason interpretation, or command claiming.
create or replace function public.bluepaws_collar_feedback(requested_household_id uuid)
returns table(device_id integer, observation_id bigint, flags integer,
  rx_window_remaining_ms integer, command jsonb)
language sql stable security invoker set search_path = ''
as $$
  select d.device_id, o.id, o.flags::integer,
    case when p.heard_at is null or p.heard_at > statement_timestamp() then 0
    else greatest(0, least(10000, floor(extract(epoch from
      (p.heard_at + interval '10 seconds' - statement_timestamp())) * 1000)))::integer end,
    case when c.id is null then null else jsonb_build_object(
      'id', c.id, 'device_id', c.device_id, 'command_type', c.command_type,
      'command_payload', c.command_payload, 'status', c.status,
      'requested_at', c.requested_at, 'expires_at', c.expires_at
    ) end
  from public.devices d
  left join lateral (
    select obs.id, obs.flags, obs.recorded_at, obs.effective_seen_at
    from public.observations obs
    where obs.device_guid16 = d.device_id and obs.household_id = d.household_id
    order by obs.effective_seen_at desc, obs.id desc limit 1
  ) o on true
  left join lateral (
    -- Fresh radio/server reception, NOT the collar GNSS clock.
    -- Earliest successful path; duplicate deliveries never restart the window.
    -- Conservatively require original hub time on LoRa. Missing/future clocks
    -- cannot manufacture a new receive opportunity from the cloud upload time.
    select least(path.first_received_at,
      case when path.ingest_path = 'lora_hub'
        then pg_catalog.to_timestamp(path.gateway_rx_time_unix::double precision)
        else path.first_received_at end) as heard_at
    from public.observation_paths path
    where path.observation_id = o.id and not path.offline_replay
      and (path.ingest_path = 'cellular_direct' or path.gateway_rx_time_unix > 0)
    order by path.first_received_at, path.id limit 1
  ) p on true
  left join lateral (
    select dc.* from public.device_commands dc
    where dc.household_id = d.household_id and dc.device_id = d.device_id
      and dc.requested_at > statement_timestamp() - interval '15 minutes'
    order by dc.requested_at desc, dc.id desc limit 1
  ) c on true
  where d.household_id = requested_household_id;
$$;
revoke all on function public.bluepaws_collar_feedback(uuid) from public, anon;
grant execute on function public.bluepaws_collar_feedback(uuid) to authenticated;
