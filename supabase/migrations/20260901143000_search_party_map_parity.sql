-- Search-party links remain read-only, short-lived bearer links. Extend their
-- sanitized snapshot with the same limited trail used by the Family map and a
-- safe Home Hub presence projection. Private avatar object paths are resolved
-- separately and only for the service-role Edge Function.
set lock_timeout = '5s';
set statement_timeout = '30s';

create or replace function private.bluepaws_get_search_party_snapshot(
  share_token text
)
returns jsonb
language plpgsql
security definer
set search_path = ''
as $$
declare
  matched_share public.family_search_shares%rowtype;
  snapshot jsonb;
begin
  if share_token is null or share_token !~ '^[0-9a-f]{64}$' then
    return jsonb_build_object('valid', false, 'reason', 'invalid');
  end if;

  select share.*
  into matched_share
  from public.family_search_shares as share
  where share.token_hash = extensions.digest(convert_to(share_token, 'UTF8'), 'sha256')
    and share.revoked_at is null
    and share.expires_at > now()
  for update;

  if not found then
    return jsonb_build_object('valid', false, 'reason', 'expired_or_revoked');
  end if;

  update public.family_search_shares
  set last_used_at = now(), use_count = use_count + 1
  where id = matched_share.id;

  select jsonb_build_object(
    'valid', true,
    'shareId', matched_share.id,
    'householdId', household.id,
    'familyName', household.name,
    'expiresAt', matched_share.expires_at,
    'devices', coalesce((
      select jsonb_agg(jsonb_build_object(
        'position_id', latest.position_id,
        'device_uid', latest.device_uid,
        'display_name', device.display_name,
        'household_id', latest.household_id,
        'message_id', latest.message_id,
        'latitude', latest.latitude,
        'longitude', latest.longitude,
        'battery', latest.battery,
        'battery_mv', latest.battery_mv,
        'status_code', latest.status_code,
        'power_profile_code', latest.power_profile_code,
        'flags', latest.flags,
        'tx_reason', latest.tx_reason,
        'ingest_path', latest.ingest_path,
        'link_type', latest.link_type,
        'link_rssi_dbm', latest.link_rssi_dbm,
        'link_snr_db', latest.link_snr_db,
        'source', latest.source,
        'recorded_at', latest.recorded_at,
        'received_at', latest.received_at,
        'schema_version', latest.schema_version,
        'home_hub_id', latest.home_hub_id,
        'home_latitude', latest.home_latitude,
        'home_longitude', latest.home_longitude,
        'home_fix_at', latest.home_fix_at,
        'avatar_kind', appearance.avatar_kind,
        'emoji_value', appearance.emoji_value,
        'marker_colour', appearance.marker_colour
      ) order by latest.device_uid)
      from public.device_latest_positions_with_home latest
      left join public.devices device
        on device.household_id = latest.household_id
       and device.device_id = latest.device_uid
      left join public.device_appearances appearance
        on appearance.household_id = latest.household_id
       and appearance.device_id = latest.device_uid
      where latest.household_id = household.id
    ), '[]'::jsonb),
    'trails', coalesce((
      select jsonb_object_agg(history.device_uid::text, history.points)
      from (
        select latest.device_uid, (
          select jsonb_agg(jsonb_build_object(
            'lat', recent.latitude,
            'lon', recent.longitude,
            'recordedAt', recent.recorded_at
          ) order by recent.recorded_at, recent.id)
          from (
            select position.id, position.latitude, position.longitude, position.recorded_at
            from public.positions position
            where position.household_id = household.id
              and position.device_uid = latest.device_uid
              and position.recorded_at >= now() - interval '7 days'
            order by position.recorded_at desc, position.message_id desc
            limit 4
          ) recent
        ) points
        from public.device_latest_positions latest
        where latest.household_id = household.id
      ) history
      where history.points is not null
    ), '{}'::jsonb),
    'hubs', coalesce((
      select jsonb_agg(jsonb_build_object(
        'gateway_guid16', hub.gateway_guid16,
        'display_name', hub.display_name,
        'mode', hub.mode,
        'received_at', hub.received_at,
        'latitude', hub.latitude,
        'longitude', hub.longitude,
        'fix_at', hub.fix_at,
        'avatar_kind', hub.avatar_kind,
        'home_emoji', hub.home_emoji,
        'portable_emoji', hub.portable_emoji,
        'marker_colour', hub.marker_colour
      ) order by hub.gateway_guid16)
      from public.hub_presence hub
      where hub.household_id = household.id
    ), '[]'::jsonb)
  )
  into snapshot
  from public.households household
  where household.id = matched_share.household_id;

  return snapshot;
end;
$$;

-- This is intentionally absent from anon/authenticated grants. The Edge
-- Function uses service_role only after receiving the share token, then issues
-- a short-lived signed Storage URL without exposing the object path in the
-- public snapshot.
create or replace function public.bluepaws_resolve_search_party_avatar(
  share_token text,
  requested_entity text,
  requested_id integer
)
returns table(bucket text, object_path text)
language plpgsql
security definer
set search_path = ''
as $$
declare
  matched_household uuid;
begin
  if share_token is null or share_token !~ '^[0-9a-f]{64}$'
    or requested_entity not in ('collar', 'hub')
    or requested_id is null or requested_id not between 1 and 65535 then
    return;
  end if;

  select share.household_id into matched_household
  from public.family_search_shares share
  where share.token_hash = extensions.digest(convert_to(share_token, 'UTF8'), 'sha256')
    and share.revoked_at is null
    and share.expires_at > now();

  if matched_household is null then return; end if;

  if requested_entity = 'collar' then
    return query
      select 'pet-avatars'::text, appearance.avatar_storage_path
      from public.device_appearances appearance
      join public.devices device
        on device.device_id = appearance.device_id
       and device.household_id = appearance.household_id
      where appearance.household_id = matched_household
        and appearance.device_id = requested_id
        and appearance.avatar_kind = 'photo'
        and appearance.avatar_storage_path is not null;
  else
    return query
      select 'hub-avatars'::text, hub.avatar_storage_path
      from public.hub_presence hub
      where hub.household_id = matched_household
        and hub.gateway_guid16 = requested_id
        and hub.avatar_kind = 'photo'
        and hub.avatar_storage_path is not null;
  end if;
end;
$$;

revoke all on function private.bluepaws_get_search_party_snapshot(text)
  from public, anon, authenticated, service_role;
revoke all on function public.bluepaws_get_search_party_snapshot(text)
  from public, anon, authenticated, service_role;
grant execute on function public.bluepaws_get_search_party_snapshot(text)
  to anon, authenticated;

revoke all on function public.bluepaws_resolve_search_party_avatar(text, text, integer)
  from public, anon, authenticated, service_role;
grant execute on function public.bluepaws_resolve_search_party_avatar(text, text, integer)
  to service_role;

comment on function public.bluepaws_get_search_party_snapshot(text) is
  'Validates a search-party bearer token and returns sanitized current collars, four-point trails and read-only Home Hub presence.';
comment on function public.bluepaws_resolve_search_party_avatar(text, text, integer) is
  'Service-role-only resolver for a token-authorized private Search Party avatar object.';
