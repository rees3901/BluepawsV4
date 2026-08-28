-- Resolve the logical home hub independently of the reporting transport.
-- No stored distance: moving/updating a hub must also update existing reports.
set lock_timeout = '5s';
set statement_timeout = '30s';

create view public.device_latest_positions_with_home
with (security_invoker = true) as
select latest.*, home.gateway_guid16 as home_hub_id,
  home.latitude as home_latitude, home.longitude as home_longitude,
  home.fix_at as home_fix_at
from public.device_latest_positions latest
left join public.observations observation
  on observation.id = latest.observation_id
  and observation.household_id = latest.household_id
left join lateral (
  select h.gateway_guid16, h.latitude, h.longitude, h.fix_at
  from public.hub_presence h
  where h.household_id = latest.household_id and (
    h.gateway_guid16 = observation.destination_id16
    or ((observation.destination_id16 is null or observation.destination_id16 in (0, 65535))
      and h.mode = 'home'
      and 1 = (select count(*) from public.hub_presence presence
        where presence.household_id = latest.household_id and presence.mode = 'home'))
  )
) home on true;

revoke all on public.device_latest_positions_with_home from public, anon, authenticated;
grant select on public.device_latest_positions_with_home to authenticated, service_role;
comment on view public.device_latest_positions_with_home is
  'RLS-protected latest positions with the same-Family logical hub. Legacy/cloud-addressed packets use a home hub only when unambiguous; never a static map origin or a receiving relay.';

-- Preserve the existing token/expiry checks for read-only shared maps.
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
  set
    last_used_at = now(),
    use_count = use_count + 1
  where id = matched_share.id;

  select jsonb_build_object(
    'valid', true,
    'shareId', matched_share.id,
    'householdId', household.id,
    'familyName', household.name,
    'expiresAt', matched_share.expires_at,
    'devices', coalesce(
      jsonb_agg(
        jsonb_build_object(
          'position_id', latest.position_id,
          'device_uid', latest.device_uid,
          'display_name', (select d.display_name from public.devices d
            where d.device_id = latest.device_uid and d.household_id = latest.household_id),
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
        )
        order by latest.device_uid
      ) filter (where latest.device_uid is not null),
      '[]'::jsonb
    )
  )
  into snapshot
  from public.households as household
  left join public.device_latest_positions_with_home as latest
    on latest.household_id = household.id
  left join public.device_appearances as appearance
    on appearance.household_id = latest.household_id
   and appearance.device_id = latest.device_uid
  where household.id = matched_share.household_id
  group by household.id, household.name;

  return snapshot;
end;
$$;
