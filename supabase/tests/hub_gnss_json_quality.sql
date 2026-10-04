-- Transactional integration test; leaves existing hub/gateway data unchanged.
begin;
do $$
declare gateway integer; sample public.hub_presence; original_fix timestamptz; other_family uuid;
begin
  select gateway_guid16 into gateway from public.gateways where enabled and household_id is not null limit 1;
  if gateway is null then raise exception 'Test requires an enabled gateway'; end if;
  select * into sample from public.bluepaws_record_hub_presence(
    p_gateway=>gateway,p_mode=>'home',p_lat=>51.9,p_lon=>-2.2,p_fix_age_s=>300,
    p_uptime=>100,p_rssi=>-40,p_ble=>true,p_advertising=>true,p_heap=>150000,p_applied=>0,
    p_gnss_valid=>true,p_sat_count=>8,p_acc_m=>null,p_hdop=>1.2);
  assert sample.sat_count=8 and sample.acc_m is null and sample.hdop=1.2 and sample.gnss_valid,
    'Quality did not round-trip';
  assert sample.fix_at=now()-interval '300 seconds', 'Fix time was refreshed';
  original_fix := sample.fix_at;
  select * into sample from public.bluepaws_record_hub_presence(
    gateway,'home',null,null,null,101,-40,true,true,150000,0,p_gnss_valid=>false);
  assert sample.fix_at=original_fix and sample.sat_count=8 and sample.hdop=1.2,
    'Heartbeat must retain the complete previous fix';
  select * into sample from public.bluepaws_record_hub_presence(
    gateway,'home',52.0,-2.0,5,102,-40,true,true,150000,0);
  assert sample.latitude=52 and sample.gnss_valid is null and sample.sat_count is null
    and sample.acc_m is null and sample.hdop is null, 'Legacy new position must clear old quality';
  select * into sample from public.bluepaws_record_hub_presence(
    gateway,'home',52.0,-2.0,5,103,-40,true,true,150000,0,p_position_simulated=>true,p_gnss_valid=>false);
  select * into sample from public.bluepaws_record_hub_presence(
    gateway,'home',null,null,null,104,-40,true,true,150000,0,p_gnss_valid=>false);
  assert sample.position_simulated, 'No-fix report must retain simulation provenance';
  begin
    perform public.bluepaws_record_hub_presence(
      gateway,'home',52.0,-2.0,5,105,-40,true,true,150000,0,
      p_position_simulated=>true,p_gnss_valid=>true,p_sat_count=>8);
    raise exception 'Simulated GNSS incorrectly accepted';
  exception when sqlstate '22023' then null;
  end;
  begin
    perform public.bluepaws_record_hub_presence(
      gateway,'home',52.0,-2.0,5,105,-40,true,true,150000,0,p_gnss_valid=>true,p_sat_count=>256);
    raise exception 'Invalid satellite count incorrectly accepted';
  exception when check_violation then null;
  end;
  select id into other_family from public.households where id<>sample.household_id limit 1;
  if other_family is not null then
    update public.hub_presence set household_id=other_family where gateway_guid16=gateway;
    select * into sample from public.bluepaws_record_hub_presence(
      gateway,'home',null,null,null,106,-40,true,true,150000,0,p_gnss_valid=>false);
    assert sample.latitude is null and sample.fix_at is null and sample.sat_count is null
      and not sample.position_simulated, 'Family transfer leaked previous location/quality';
  end if;
  assert not has_function_privilege('authenticated',
    'public.bluepaws_record_hub_presence(integer,text,double precision,double precision,integer,bigint,integer,boolean,boolean,integer,bigint,text,integer,integer,boolean,boolean,boolean,integer,double precision,double precision)',
    'EXECUTE'), 'Hub ingestion must remain service-only';
end $$;
rollback;
