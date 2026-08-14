-- Establish explicit Family onboarding and invitation foundations without
-- renaming the existing household tenancy boundary.

set lock_timeout = '5s';
set statement_timeout = '120s';

create table public.profiles (
  user_id uuid primary key references auth.users(id) on delete cascade,
  display_name text not null,
  active_household_id uuid references public.households(id) on delete set null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint profiles_display_name_length
    check (length(btrim(display_name)) between 1 and 80)
);

create index profiles_active_household_idx
  on public.profiles (active_household_id)
  where active_household_id is not null;

create table public.household_invitations (
  id uuid primary key default gen_random_uuid(),
  household_id uuid not null references public.households(id) on delete cascade,
  email text not null,
  role text not null default 'member',
  token_hash bytea not null unique,
  invited_by uuid not null references auth.users(id) on delete cascade,
  created_at timestamptz not null default now(),
  expires_at timestamptz not null,
  accepted_at timestamptz,
  accepted_by uuid references auth.users(id) on delete set null,
  revoked_at timestamptz,
  constraint household_invitations_email_normalized
    check (
      email = lower(btrim(email))
      and length(email) between 3 and 320
      and position('@' in email) > 1
    ),
  constraint household_invitations_role_check check (role = 'member'),
  constraint household_invitations_expiry_check check (expires_at > created_at),
  constraint household_invitations_terminal_state_check
    check (not (accepted_at is not null and revoked_at is not null)),
  constraint household_invitations_acceptance_check
    check (
      (accepted_at is null and accepted_by is null)
      or (accepted_at is not null and accepted_by is not null)
    )
);

create index household_invitations_household_created_idx
  on public.household_invitations (household_id, created_at desc);

create index household_invitations_email_expiry_idx
  on public.household_invitations (email, expires_at desc)
  where accepted_at is null and revoked_at is null;

insert into public.profiles (user_id, display_name, active_household_id)
select
  bluepaws_user.id,
  left(
    coalesce(
      nullif(btrim(bluepaws_user.raw_user_meta_data ->> 'full_name'), ''),
      nullif(btrim(bluepaws_user.raw_user_meta_data ->> 'name'), ''),
      nullif(split_part(coalesce(bluepaws_user.email, ''), '@', 1), ''),
      'Bluepaws user'
    ),
    80
  ),
  first_membership.household_id
from auth.users as bluepaws_user
left join lateral (
  select member.household_id
  from public.household_members as member
  where member.user_id = bluepaws_user.id
  order by member.joined_at, member.household_id
  limit 1
) as first_membership on true
on conflict (user_id) do nothing;

create or replace function private.handle_new_bluepaws_user()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
  profile_name text;
begin
  profile_name := left(
    coalesce(
      nullif(btrim(new.raw_user_meta_data ->> 'full_name'), ''),
      nullif(btrim(new.raw_user_meta_data ->> 'name'), ''),
      nullif(split_part(coalesce(new.email, ''), '@', 1), ''),
      'Bluepaws user'
    ),
    80
  );

  insert into public.profiles (user_id, display_name)
  values (new.id, profile_name)
  on conflict (user_id) do nothing;

  return new;
end;
$$;

revoke execute on function private.handle_new_bluepaws_user()
  from public, anon, authenticated;

create or replace function private.set_profile_updated_at()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  new.display_name := btrim(new.display_name);
  new.updated_at := now();
  return new;
end;
$$;

create or replace function private.validate_profile_active_household()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  if new.active_household_id is not null and not exists (
    select 1
    from public.household_members as member
    where member.household_id = new.active_household_id
      and member.user_id = new.user_id
  ) then
    raise exception using
      errcode = '42501',
      message = 'Active Family must be one of the user''s memberships';
  end if;

  return new;
end;
$$;

revoke execute on function private.set_profile_updated_at()
  from public, anon, authenticated;
revoke execute on function private.validate_profile_active_household()
  from public, anon, authenticated;

create trigger profiles_set_updated_at
  before update on public.profiles
  for each row execute function private.set_profile_updated_at();

create trigger profiles_validate_active_household
  before insert or update of active_household_id, user_id on public.profiles
  for each row execute function private.validate_profile_active_household();

create or replace function private.current_user_is_household_owner(
  requested_household_id uuid
)
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select (select auth.uid()) is not null and exists (
    select 1
    from public.household_members as member
    where member.household_id = requested_household_id
      and member.user_id = (select auth.uid())
      and member.role = 'owner'
  );
$$;

create or replace function private.bluepaws_create_family(
  family_name text,
  profile_display_name text
)
returns uuid
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  created_household_id uuid;
  normalized_family_name text := btrim(family_name);
  normalized_display_name text := btrim(profile_display_name);
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if length(normalized_family_name) not between 1 and 80 then
    raise exception using errcode = '22023', message = 'Family name must be between 1 and 80 characters';
  end if;

  if length(normalized_display_name) not between 1 and 80 then
    raise exception using errcode = '22023', message = 'Display name must be between 1 and 80 characters';
  end if;

  perform pg_advisory_xact_lock(
    hashtextextended('bluepaws-family-onboarding:' || caller_id::text, 0)
  );

  select member.household_id
  into created_household_id
  from public.household_members as member
  where member.user_id = caller_id
  order by member.joined_at, member.household_id
  limit 1;

  if created_household_id is not null then
    update public.profiles
    set
      display_name = normalized_display_name,
      active_household_id = created_household_id
    where user_id = caller_id;

    return created_household_id;
  end if;

  insert into public.households (name, kind, created_by)
  values (normalized_family_name, 'customer', caller_id)
  returning id into created_household_id;

  insert into public.household_members (household_id, user_id, role)
  values (created_household_id, caller_id, 'owner');

  insert into public.profiles (user_id, display_name, active_household_id)
  values (caller_id, normalized_display_name, created_household_id)
  on conflict (user_id) do update
  set
    display_name = excluded.display_name,
    active_household_id = excluded.active_household_id;

  return created_household_id;
end;
$$;

create or replace function private.bluepaws_set_active_family(
  requested_household_id uuid
)
returns uuid
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if not exists (
    select 1
    from public.household_members as member
    where member.household_id = requested_household_id
      and member.user_id = caller_id
  ) then
    raise exception using errcode = '42501', message = 'Family membership required';
  end if;

  update public.profiles
  set active_household_id = requested_household_id
  where user_id = caller_id;

  if not found then
    raise exception using errcode = 'P0002', message = 'Bluepaws profile not found';
  end if;

  return requested_household_id;
end;
$$;

create or replace function private.bluepaws_create_family_invitation(
  requested_household_id uuid,
  invited_email text
)
returns table (
  invitation_id uuid,
  invitation_token text,
  invitation_expires_at timestamptz
)
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  normalized_email text := lower(btrim(invited_email));
  raw_token text := encode(extensions.gen_random_bytes(32), 'hex');
  expiry timestamptz := now() + interval '7 days';
  created_invitation_id uuid;
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if not private.current_user_is_household_owner(requested_household_id) then
    raise exception using errcode = '42501', message = 'Only the Family owner can invite members';
  end if;

  if length(normalized_email) not between 3 and 320 or position('@' in normalized_email) <= 1 then
    raise exception using errcode = '22023', message = 'A valid email address is required';
  end if;

  if (
    select count(*)
    from public.household_invitations as recent_invitation
    where recent_invitation.household_id = requested_household_id
      and recent_invitation.created_at > now() - interval '1 hour'
  ) >= 50 then
    raise exception using errcode = '54000', message = 'Family invitation rate limit reached';
  end if;

  update public.household_invitations
  set revoked_at = now()
  where household_id = requested_household_id
    and email = normalized_email
    and accepted_at is null
    and revoked_at is null;

  insert into public.household_invitations (
    household_id,
    email,
    role,
    token_hash,
    invited_by,
    expires_at
  )
  values (
    requested_household_id,
    normalized_email,
    'member',
    extensions.digest(convert_to(raw_token, 'UTF8'), 'sha256'),
    caller_id,
    expiry
  )
  returning id into created_invitation_id;

  return query select created_invitation_id, raw_token, expiry;
end;
$$;

create or replace function private.bluepaws_accept_family_invitation(
  invitation_token text,
  profile_display_name text default null
)
returns uuid
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  caller_email text;
  matched_invitation public.household_invitations%rowtype;
  normalized_display_name text := nullif(btrim(profile_display_name), '');
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if invitation_token is null or invitation_token !~ '^[0-9a-f]{64}$' then
    raise exception using errcode = '22023', message = 'Invitation is invalid or expired';
  end if;

  select lower(btrim(bluepaws_user.email))
  into caller_email
  from auth.users as bluepaws_user
  where bluepaws_user.id = caller_id
    and bluepaws_user.email_confirmed_at is not null;

  if caller_email is null then
    raise exception using errcode = '42501', message = 'A verified email address is required';
  end if;

  select invitation.*
  into matched_invitation
  from public.household_invitations as invitation
  where invitation.token_hash = extensions.digest(convert_to(invitation_token, 'UTF8'), 'sha256')
    and invitation.accepted_at is null
    and invitation.revoked_at is null
    and invitation.expires_at > now()
  for update;

  if not found or matched_invitation.email <> caller_email then
    raise exception using errcode = '42501', message = 'Invitation is invalid or expired';
  end if;

  if normalized_display_name is not null and length(normalized_display_name) > 80 then
    raise exception using errcode = '22023', message = 'Display name must be no more than 80 characters';
  end if;

  insert into public.household_members (household_id, user_id, role)
  values (matched_invitation.household_id, caller_id, 'member')
  on conflict (household_id, user_id) do nothing;

  update public.household_invitations
  set accepted_at = now(), accepted_by = caller_id
  where id = matched_invitation.id;

  insert into public.profiles (user_id, display_name, active_household_id)
  values (
    caller_id,
    coalesce(normalized_display_name, split_part(caller_email, '@', 1)),
    matched_invitation.household_id
  )
  on conflict (user_id) do update
  set
    display_name = coalesce(normalized_display_name, public.profiles.display_name),
    active_household_id = excluded.active_household_id;

  return matched_invitation.household_id;
end;
$$;

create or replace function private.bluepaws_revoke_family_invitation(
  requested_invitation_id uuid
)
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
declare
  invitation_household_id uuid;
begin
  if (select auth.uid()) is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  select invitation.household_id
  into invitation_household_id
  from public.household_invitations as invitation
  where invitation.id = requested_invitation_id
    and invitation.accepted_at is null
    and invitation.revoked_at is null;

  if invitation_household_id is null
    or not private.current_user_is_household_owner(invitation_household_id) then
    raise exception using errcode = '42501', message = 'Invitation not found';
  end if;

  update public.household_invitations
  set revoked_at = now()
  where id = requested_invitation_id;

  return true;
end;
$$;

create or replace function public.bluepaws_create_family(
  family_name text,
  profile_display_name text
)
returns uuid
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_create_family(family_name, profile_display_name);
$$;

create or replace function public.bluepaws_set_active_family(
  requested_household_id uuid
)
returns uuid
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_set_active_family(requested_household_id);
$$;

create or replace function public.bluepaws_create_family_invitation(
  requested_household_id uuid,
  invited_email text
)
returns table (
  invitation_id uuid,
  invitation_token text,
  invitation_expires_at timestamptz
)
language sql
security invoker
set search_path = ''
as $$
  select *
  from private.bluepaws_create_family_invitation(requested_household_id, invited_email);
$$;

create or replace function public.bluepaws_accept_family_invitation(
  invitation_token text,
  profile_display_name text default null
)
returns uuid
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_accept_family_invitation(invitation_token, profile_display_name);
$$;

create or replace function public.bluepaws_revoke_family_invitation(
  requested_invitation_id uuid
)
returns boolean
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_revoke_family_invitation(requested_invitation_id);
$$;

revoke all on function private.current_user_is_household_owner(uuid)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_create_family(text, text)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_set_active_family(uuid)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_create_family_invitation(uuid, text)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_accept_family_invitation(text, text)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_revoke_family_invitation(uuid)
  from public, anon, authenticated, service_role;

grant usage on schema private to authenticated;
grant execute on function private.current_user_is_household_owner(uuid) to authenticated;
grant execute on function private.bluepaws_create_family(text, text) to authenticated;
grant execute on function private.bluepaws_set_active_family(uuid) to authenticated;
grant execute on function private.bluepaws_create_family_invitation(uuid, text) to authenticated;
grant execute on function private.bluepaws_accept_family_invitation(text, text) to authenticated;
grant execute on function private.bluepaws_revoke_family_invitation(uuid) to authenticated;

revoke all on function public.bluepaws_create_family(text, text)
  from public, anon, authenticated;
revoke all on function public.bluepaws_set_active_family(uuid)
  from public, anon, authenticated;
revoke all on function public.bluepaws_create_family_invitation(uuid, text)
  from public, anon, authenticated;
revoke all on function public.bluepaws_accept_family_invitation(text, text)
  from public, anon, authenticated;
revoke all on function public.bluepaws_revoke_family_invitation(uuid)
  from public, anon, authenticated;

grant execute on function public.bluepaws_create_family(text, text) to authenticated;
grant execute on function public.bluepaws_set_active_family(uuid) to authenticated;
grant execute on function public.bluepaws_create_family_invitation(uuid, text) to authenticated;
grant execute on function public.bluepaws_accept_family_invitation(text, text) to authenticated;
grant execute on function public.bluepaws_revoke_family_invitation(uuid) to authenticated;

alter table public.profiles enable row level security;
alter table public.household_invitations enable row level security;

create policy "Users read their own profile"
  on public.profiles for select to authenticated
  using (user_id = (select auth.uid()));

create policy "Users update their own profile"
  on public.profiles for update to authenticated
  using (user_id = (select auth.uid()))
  with check (user_id = (select auth.uid()));

drop policy if exists "Members read own memberships" on public.household_members;
create policy "Members and owners read Family memberships"
  on public.household_members for select to authenticated
  using (
    user_id = (select auth.uid())
    or private.current_user_is_household_owner(household_id)
  );

create policy "Owners read Family invitations"
  on public.household_invitations for select to authenticated
  using (private.current_user_is_household_owner(household_id));

revoke all on table public.profiles from anon, authenticated;
revoke all on table public.household_invitations from anon, authenticated;

grant select on table public.profiles to authenticated;
grant update (display_name, active_household_id) on table public.profiles to authenticated;
grant select on table public.household_invitations to authenticated;

comment on table public.profiles is
  'Customer-facing profile and explicitly selected active Family.';
comment on table public.household_invitations is
  'Hashed, email-bound, one-time invitations for permanent Family members.';
