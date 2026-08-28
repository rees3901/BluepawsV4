-- Metadata only: retain IDs, credentials, Family ownership and telemetry.
set lock_timeout = '5s';
set statement_timeout = '120s';

grant update (display_name) on public.devices to authenticated;
create policy "Family members rename collars"
  on public.devices for update to authenticated
  using (exists (
    select 1 from public.household_members m
    where m.household_id = devices.household_id
      and m.user_id = (select auth.uid()) and m.role in ('owner', 'member')
  ))
  with check (exists (
    select 1 from public.household_members m
    where m.household_id = devices.household_id
      and m.user_id = (select auth.uid()) and m.role in ('owner', 'member')
  ));

-- Invoker security preserves existing RLS. Neither identities nor credentials
-- can be changed via this entrypoint; both metadata writes succeed or neither.
create function public.bluepaws_save_device_marker(
  requested_device_id integer,
  requested_household_id uuid,
  requested_name text,
  requested_avatar_kind text,
  requested_emoji text,
  requested_colour text,
  requested_storage_path text default null
) returns void
language plpgsql security invoker set search_path = ''
as $$
declare
  friendly_name text := btrim(requested_name);
begin
  if auth.uid() is null then
    raise exception 'Sign in to customise this marker' using errcode = '42501';
  end if;
  if friendly_name is null or char_length(friendly_name) not between 1 and 80
     or friendly_name ~ '[[:cntrl:]]' then
    raise exception 'Enter a single-line name of 1 to 80 characters' using errcode = '22023';
  end if;
  update public.devices set display_name = friendly_name
    where device_id = requested_device_id and household_id = requested_household_id;
  if not found then
    raise exception 'Device is unavailable for this Family' using errcode = '42501';
  end if;
  insert into public.device_appearances (
    device_id, household_id, avatar_kind, emoji_value, marker_colour, avatar_storage_path
  ) values (
    requested_device_id, requested_household_id, requested_avatar_kind,
    requested_emoji, requested_colour, requested_storage_path
  )
  on conflict (device_id) do update set
    avatar_kind = excluded.avatar_kind,
    emoji_value = excluded.emoji_value,
    marker_colour = excluded.marker_colour,
    avatar_storage_path = excluded.avatar_storage_path;
end;
$$;
revoke all on function public.bluepaws_save_device_marker(integer,uuid,text,text,text,text,text) from public, anon;
grant execute on function public.bluepaws_save_device_marker(integer,uuid,text,text,text,text,text) to authenticated;

-- Notify other Family dashboards without changing last-seen or waking the collar.
create trigger devices_broadcast_name
  after update of display_name on public.devices
  for each row when (old.display_name is distinct from new.display_name)
  execute function private.broadcast_device_presence();

-- Keep the existing token, expiry and Family checks unchanged; expose the same
-- friendly name on the read-only helper map, without granting new write access.
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
  left join public.device_latest_positions as latest
    on latest.household_id = household.id
  left join public.device_appearances as appearance
    on appearance.household_id = latest.household_id
   and appearance.device_id = latest.device_uid
  where household.id = matched_share.household_id
  group by household.id, household.name;

  return snapshot;
end;
$$;
