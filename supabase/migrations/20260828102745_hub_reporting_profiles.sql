-- Reporting cadence is independent of Home/Portable/Off-Grid and collar TLV profiles.
-- Legacy firmware reports once per minute; do not label it Power Save until confirmed.
alter table public.hub_presence
  add column reporting_profile text not null default 'normal'
    check (reporting_profile in ('normal','power_save','active')),
  add column desired_reporting_profile text not null default 'power_save'
    check (desired_reporting_profile in ('normal','power_save','active')),
  add column control_poll_s integer check (control_poll_s between 1 and 60);

-- Existing Family RLS remains in force. Browsers can request, never attest applied state.
grant update(desired_reporting_profile) on public.hub_presence to authenticated;
drop trigger hub_preferences_revision on public.hub_presence;
create trigger hub_preferences_revision before update of
display_name,home_emoji,portable_emoji,marker_colour,desired_ble_enabled,desired_reporting_profile
on public.hub_presence for each row execute function private.bluepaws_hub_preferences_revision();
update public.hub_presence set settings_revision=settings_revision+1;

-- Replace, not overload, so PostgREST has one unambiguous RPC.
-- Defaults let the previous Edge Function keep calling with its original arguments.
drop function public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint);
create function public.bluepaws_record_hub_presence(
  p_gateway integer, p_mode text, p_lat double precision, p_lon double precision,
  p_fix_age_s integer, p_uptime bigint, p_rssi integer, p_ble boolean,
  p_advertising boolean, p_heap integer, p_applied bigint,
  p_reporting_profile text default 'normal', p_control_poll_s integer default null
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
      ble_enabled,ble_advertising,free_heap,display_name,applied_revision,reporting_profile,control_poll_s)
    values(p_gateway,family,p_mode,p_lat,p_lon,
      case when p_lat is not null then now()-make_interval(secs=>p_fix_age_s) end,
      p_uptime,p_rssi,p_ble,p_advertising,p_heap,coalesce(nullif(hub_name,''),'Home Hub'),p_applied,p_reporting_profile,p_control_poll_s)
    on conflict(gateway_guid16) do update set
      household_id=excluded.household_id, mode=excluded.mode, received_at=now(),
      latitude=case when h.household_id<>excluded.household_id then excluded.latitude else coalesce(excluded.latitude,h.latitude) end,
      longitude=case when h.household_id<>excluded.household_id then excluded.longitude else coalesce(excluded.longitude,h.longitude) end,
      fix_at=case when h.household_id<>excluded.household_id then excluded.fix_at else coalesce(excluded.fix_at,h.fix_at) end,
      uptime_s=excluded.uptime_s,wifi_rssi_dbm=excluded.wifi_rssi_dbm,
      ble_enabled=excluded.ble_enabled,ble_advertising=excluded.ble_advertising,
      free_heap=excluded.free_heap,applied_revision=excluded.applied_revision,
      reporting_profile=excluded.reporting_profile,control_poll_s=excluded.control_poll_s
    returning h.*;
end $$;
revoke all on function public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint,text,integer) from public,anon,authenticated;
grant execute on function public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint,text,integer) to service_role;
