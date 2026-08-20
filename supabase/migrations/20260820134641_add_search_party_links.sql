-- Short-lived, read-only bearer links for search parties helping find pets.
-- The URL token is returned only once and stored in Postgres as SHA-256.

set lock_timeout = '5s';
set statement_timeout = '120s';

create table public.family_search_shares (
  id uuid primary key default gen_random_uuid(),
  household_id uuid not null references public.households(id) on delete cascade,
  token_hash bytea not null unique,
  created_by uuid not null references auth.users(id) on delete cascade,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  revoked_at timestamptz,
  last_used_at timestamptz,
  use_count integer not null default 0,
  constraint family_search_shares_expiry_check check (expires_at > created_at),
  constraint family_search_shares_use_count_check check (use_count >= 0)
);

create index family_search_shares_household_created_idx
  on public.family_search_shares (household_id, created_at desc);

create index family_search_shares_active_household_expiry_idx
  on public.family_search_shares (household_id, expires_at desc)
  where revoked_at is null;

alter table public.family_search_shares enable row level security;

create policy "Owners read Family search party shares"
  on public.family_search_shares for select to authenticated
  using (private.current_user_is_household_owner(household_id));

revoke all on table public.family_search_shares from anon, authenticated;
grant select on table public.family_search_shares to authenticated;

create or replace function private.bluepaws_create_search_party_share(
  requested_household_id uuid
)
returns table (
  share_id uuid,
  share_token text,
  share_expires_at timestamptz
)
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  raw_token text := encode(extensions.gen_random_bytes(32), 'hex');
  expiry timestamptz := now() + interval '4 hours';
  created_share_id uuid;
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if not private.current_user_is_household_owner(requested_household_id) then
    raise exception using errcode = '42501', message = 'Only the Family owner can create search party links';
  end if;

  if (
    select count(*)
    from public.family_search_shares as recent_share
    where recent_share.household_id = requested_household_id
      and recent_share.created_at > now() - interval '1 hour'
  ) >= 20 then
    raise exception using errcode = '54000', message = 'Search party link rate limit reached';
  end if;

  insert into public.family_search_shares (
    household_id,
    token_hash,
    created_by,
    expires_at
  )
  values (
    requested_household_id,
    extensions.digest(convert_to(raw_token, 'UTF8'), 'sha256'),
    caller_id,
    expiry
  )
  returning id into created_share_id;

  return query select created_share_id, raw_token, expiry;
end;
$$;

create or replace function private.bluepaws_revoke_search_party_share(
  requested_share_id uuid
)
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
declare
  share_household_id uuid;
begin
  if (select auth.uid()) is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  select share.household_id
  into share_household_id
  from public.family_search_shares as share
  where share.id = requested_share_id
    and share.revoked_at is null
    and share.expires_at > now()
  for update;

  if share_household_id is null
    or not private.current_user_is_household_owner(share_household_id) then
    raise exception using errcode = '42501', message = 'Search party link not found';
  end if;

  update public.family_search_shares
  set revoked_at = now()
  where id = requested_share_id;

  return true;
end;
$$;

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

create or replace function public.bluepaws_create_search_party_share(
  requested_household_id uuid
)
returns table (
  share_id uuid,
  share_token text,
  share_expires_at timestamptz
)
language sql
security invoker
set search_path = ''
as $$
  select *
  from private.bluepaws_create_search_party_share(requested_household_id);
$$;

create or replace function public.bluepaws_revoke_search_party_share(
  requested_share_id uuid
)
returns boolean
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_revoke_search_party_share(requested_share_id);
$$;

create or replace function public.bluepaws_get_search_party_snapshot(
  share_token text
)
returns jsonb
language sql
security definer
set search_path = ''
as $$
  select private.bluepaws_get_search_party_snapshot(share_token);
$$;

revoke all on function private.bluepaws_create_search_party_share(uuid)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_revoke_search_party_share(uuid)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_get_search_party_snapshot(text)
  from public, anon, authenticated, service_role;

grant usage on schema private to authenticated;
grant execute on function private.bluepaws_create_search_party_share(uuid) to authenticated;
grant execute on function private.bluepaws_revoke_search_party_share(uuid) to authenticated;

revoke all on function public.bluepaws_create_search_party_share(uuid)
  from public, anon, authenticated;
revoke all on function public.bluepaws_revoke_search_party_share(uuid)
  from public, anon, authenticated;
revoke all on function public.bluepaws_get_search_party_snapshot(text)
  from public, anon, authenticated;

grant execute on function public.bluepaws_create_search_party_share(uuid) to authenticated;
grant execute on function public.bluepaws_revoke_search_party_share(uuid) to authenticated;
grant execute on function public.bluepaws_get_search_party_snapshot(text) to anon, authenticated;

comment on table public.family_search_shares is
  'Short-lived, revocable, read-only bearer links for helpers during a missing-pet search.';
comment on function public.bluepaws_create_search_party_share(uuid) is
  'Creates a four-hour read-only Family map link for an authenticated Family Owner.';
comment on function public.bluepaws_revoke_search_party_share(uuid) is
  'Revokes an active search-party link when called by the Family Owner.';
comment on function public.bluepaws_get_search_party_snapshot(text) is
  'Validates a search-party bearer token and returns a sanitized current-position snapshot.';
