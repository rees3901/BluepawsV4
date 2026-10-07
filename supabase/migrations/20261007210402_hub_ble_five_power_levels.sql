-- Widen presets without fabricating support on existing three-level firmware.
alter table public.hub_presence
  drop constraint hub_presence_desired_ble_tx_power_dbm_check,
  drop constraint hub_presence_ble_tx_power_dbm_check,
  add constraint hub_presence_desired_ble_tx_power_dbm_check check (desired_ble_tx_power_dbm in (-12,-6,3,6,9)),
  add constraint hub_presence_ble_tx_power_dbm_check check (ble_tx_power_dbm in (-12,-6,3,6,9)),
  add column ble_power_steps smallint check (ble_power_steps in (3,5));

-- A prior non-null controller report proves only the original three levels.
update public.hub_presence set ble_power_steps=3 where ble_tx_power_dbm is not null;

drop function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean,boolean,integer,double precision,double precision,integer
);
create function public.bluepaws_record_hub_presence(
  p_gateway integer, p_mode text, p_lat double precision, p_lon double precision,
  p_fix_age_s integer, p_uptime bigint, p_rssi integer, p_ble boolean,
  p_advertising boolean, p_heap integer, p_applied bigint,
  p_reporting_profile text default 'normal', p_control_poll_s integer default null,
  p_battery_percent integer default null, p_position_simulated boolean default false,
  p_battery_simulated boolean default false, p_gnss_valid boolean default null,
  p_sat_count integer default null, p_acc_m double precision default null,
  p_hdop double precision default null, p_ble_tx_power_dbm integer default null, p_ble_power_steps integer default null
) returns setof public.hub_presence
language plpgsql security invoker set search_path='' as $$
declare family uuid; hub_name text;
begin
  select g.household_id,g.display_name into family,hub_name from public.gateways g
    where g.gateway_guid16=p_gateway and g.enabled;
  if family is null then raise exception using errcode='42501',message='Gateway unavailable'; end if;
  if (p_lat is null) <> (p_lon is null)
    or (p_lat is not null and (p_fix_age_s is null or p_fix_age_s not between 0 and 604800))
    or (p_ble_tx_power_dbm is not null and p_ble_tx_power_dbm not in (-12,-6,3,6,9))
    or (p_ble_power_steps is not null and p_ble_power_steps not in (3,5))
    or (p_ble_tx_power_dbm in (-6,6) and p_ble_power_steps is distinct from 5)
    or p_applied is null or p_applied < 0
    or (p_battery_percent is not null and p_battery_percent not between 0 and 100)
    or (p_position_simulated and p_lat is null)
    or (p_battery_simulated and p_battery_percent is null)
    or (p_gnss_valid is true and (p_lat is null or p_position_simulated))
    or ((p_sat_count is not null or p_acc_m is not null or p_hdop is not null) and p_gnss_valid is distinct from true)
  then raise exception using errcode='22023',message='Invalid hub report'; end if;
  return query insert into public.hub_presence as h
    (gateway_guid16,household_id,mode,latitude,longitude,fix_at,uptime_s,wifi_rssi_dbm,
      ble_enabled,ble_advertising,free_heap,display_name,applied_revision,
      reporting_profile,control_poll_s,battery_percent,position_simulated,battery_simulated,
      gnss_valid,sat_count,acc_m,hdop,ble_tx_power_dbm,ble_power_steps)
    values(p_gateway,family,p_mode,p_lat,p_lon,
      case when p_lat is not null then now()-make_interval(secs=>p_fix_age_s) end,
      p_uptime,p_rssi,p_ble,p_advertising,p_heap,
      coalesce(nullif(hub_name,''),'Home Hub'),p_applied,p_reporting_profile,
      p_control_poll_s,p_battery_percent,p_position_simulated,p_battery_simulated,
      p_gnss_valid,p_sat_count,p_acc_m,p_hdop,p_ble_tx_power_dbm,coalesce(p_ble_power_steps,case when p_ble_tx_power_dbm is not null then 3 end))
    on conflict(gateway_guid16) do update set
      household_id=excluded.household_id, mode=excluded.mode, received_at=now(),
      -- Keep position, provenance and quality together. A heartbeat with no
      -- fix retains the last snapshot, but a Family transfer must clear it.
      latitude=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.latitude else h.latitude end,
      longitude=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.longitude else h.longitude end,
      fix_at=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.fix_at else h.fix_at end,
      position_simulated=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.position_simulated else h.position_simulated end,
      gnss_valid=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.gnss_valid else h.gnss_valid end,
      sat_count=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.sat_count else h.sat_count end,
      acc_m=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.acc_m else h.acc_m end,
      hdop=case when h.household_id<>excluded.household_id or excluded.latitude is not null then excluded.hdop else h.hdop end,
      uptime_s=excluded.uptime_s,wifi_rssi_dbm=excluded.wifi_rssi_dbm,
      ble_power_steps=excluded.ble_power_steps,ble_tx_power_dbm=excluded.ble_tx_power_dbm,ble_enabled=excluded.ble_enabled,ble_advertising=excluded.ble_advertising,
      free_heap=excluded.free_heap,applied_revision=excluded.applied_revision,
      reporting_profile=excluded.reporting_profile,control_poll_s=excluded.control_poll_s,
      battery_percent=excluded.battery_percent,battery_simulated=excluded.battery_simulated
    returning h.*;
end $$;

revoke all on function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean,boolean,integer,double precision,double precision,integer,integer
) from public,anon,authenticated;
grant execute on function public.bluepaws_record_hub_presence(
  integer,text,double precision,double precision,integer,bigint,integer,
  boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean,boolean,integer,double precision,double precision,integer,integer
) to service_role;
