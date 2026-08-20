-- Require an intended helper email before creating a search-party bearer link.
-- This keeps the Owner UI aligned with Family invites and gives a clear audit
-- trail, while access remains controlled by the short-lived bearer token.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.family_search_shares
  add column if not exists helper_email text;

update public.family_search_shares
set helper_email = 'legacy-search-helper@example.invalid'
where helper_email is null;

alter table public.family_search_shares
  alter column helper_email set not null;

do $$
begin
  if not exists (
    select 1
    from pg_constraint
    where conname = 'family_search_shares_email_normalized'
      and conrelid = 'public.family_search_shares'::regclass
  ) then
    alter table public.family_search_shares
      add constraint family_search_shares_email_normalized
      check (
        helper_email = lower(btrim(helper_email))
        and length(helper_email) between 3 and 320
        and position('@' in helper_email) > 1
      );
  end if;
end
$$;

create or replace function private.bluepaws_create_search_party_share(
  requested_household_id uuid,
  helper_email text
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
  normalized_email text := lower(btrim(helper_email));
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

  if length(normalized_email) not between 3 and 320 or position('@' in normalized_email) <= 1 then
    raise exception using errcode = '22023', message = 'A valid helper email address is required';
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
    helper_email,
    token_hash,
    created_by,
    expires_at
  )
  values (
    requested_household_id,
    normalized_email,
    extensions.digest(convert_to(raw_token, 'UTF8'), 'sha256'),
    caller_id,
    expiry
  )
  returning id into created_share_id;

  return query select created_share_id, raw_token, expiry;
end;
$$;

create or replace function public.bluepaws_create_search_party_share(
  requested_household_id uuid,
  helper_email text
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
  from private.bluepaws_create_search_party_share(requested_household_id, helper_email);
$$;

drop function if exists public.bluepaws_create_search_party_share(uuid);
drop function if exists private.bluepaws_create_search_party_share(uuid);

revoke all on function private.bluepaws_create_search_party_share(uuid, text)
  from public, anon, authenticated, service_role;
grant execute on function private.bluepaws_create_search_party_share(uuid, text)
  to authenticated;

revoke all on function public.bluepaws_create_search_party_share(uuid, text)
  from public, anon, authenticated;
grant execute on function public.bluepaws_create_search_party_share(uuid, text)
  to authenticated;

comment on column public.family_search_shares.helper_email is
  'Owner-entered intended helper email for audit and share prefill; bearer-link access is not email-authenticated.';
comment on function public.bluepaws_create_search_party_share(uuid, text) is
  'Creates a four-hour read-only Family map link for an authenticated Family Owner and intended helper email.';
