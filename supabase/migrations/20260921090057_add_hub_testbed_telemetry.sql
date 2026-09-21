-- Optional hub power data and provenance for the P4 daughterboard testbed.
-- The simulation flags prevent temporary fallback values being presented as
-- physical sensor readings. Existing hubs remain valid with null/false data.
alter table public.hub_presence
  add column battery_percent smallint check (battery_percent between 0 and 100),
  add column position_simulated boolean not null default false,
  add column battery_simulated boolean not null default false;

-- Replace rather than overload so PostgREST continues to expose one
-- unambiguous RPC. New parameters are optional for older hub firmware.
drop function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer
);

create function public.bluepaws_record_hub_presence(
  p_gateway integer, p_mode text, p_lat double precision, p_lon double precision,
  p_fix_age_s integer, p_uptime bigint, p_rssi integer, p_ble boolean,
  p_advertising boolean, p_heap integer, p_applied bigint,
  p_reporting_profile text default 'normal', p_control_poll_s integer default null,
  p_battery_percent integer default null, p_position_simulated boolean default false,
  p_battery_simulated boolean default false
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
    or (p_battery_percent is not null and p_battery_percent not between 0 and 100)
    or (p_position_simulated and p_lat is null)
    or (p_battery_simulated and p_battery_percent is null)
  then raise exception using errcode='22023',message='Invalid hub report'; end if;
  return query insert into public.hub_presence as h
    (gateway_guid16,household_id,mode,latitude,longitude,fix_at,uptime_s,wifi_rssi_dbm,
      ble_enabled,ble_advertising,free_heap,display_name,applied_revision,
      reporting_profile,control_poll_s,battery_percent,position_simulated,battery_simulated)
    values(p_gateway,family,p_mode,p_lat,p_lon,
      case when p_lat is not null then now()-make_interval(secs=>p_fix_age_s) end,
      p_uptime,p_rssi,p_ble,p_advertising,p_heap,
      coalesce(nullif(hub_name,''),'Home Hub'),p_applied,p_reporting_profile,
      p_control_poll_s,p_battery_percent,p_position_simulated,p_battery_simulated)
    on conflict(gateway_guid16) do update set
      household_id=excluded.household_id, mode=excluded.mode, received_at=now(),
      latitude=case when h.household_id<>excluded.household_id then excluded.latitude else coalesce(excluded.latitude,h.latitude) end,
      longitude=case when h.household_id<>excluded.household_id then excluded.longitude else coalesce(excluded.longitude,h.longitude) end,
      fix_at=case when h.household_id<>excluded.household_id then excluded.fix_at else coalesce(excluded.fix_at,h.fix_at) end,
      uptime_s=excluded.uptime_s,wifi_rssi_dbm=excluded.wifi_rssi_dbm,
      ble_enabled=excluded.ble_enabled,ble_advertising=excluded.ble_advertising,
      free_heap=excluded.free_heap,applied_revision=excluded.applied_revision,
      reporting_profile=excluded.reporting_profile,control_poll_s=excluded.control_poll_s,
      battery_percent=excluded.battery_percent,
      position_simulated=excluded.position_simulated,
      battery_simulated=excluded.battery_simulated
    returning h.*;
end $$;

revoke all on function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean
) from public,anon,authenticated;
grant execute on function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean
) to service_role;

comment on column public.hub_presence.position_simulated is
  'True only for explicitly configured testbed fallback positions; never inferred.';
comment on column public.hub_presence.battery_simulated is
  'True only for explicitly configured testbed fallback battery readings; never inferred.';
