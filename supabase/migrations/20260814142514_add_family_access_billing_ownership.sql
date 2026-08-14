-- Separate paid-account authority from Family membership, and allow a Family
-- Owner to remove an accepted Member without affecting that person's identity
-- or memberships in any other Family.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.households
  add column access_version integer not null default 1,
  add constraint households_access_version_check
    check (access_version between 1 and 2147483647);

create table public.family_billing_accounts (
  household_id uuid primary key
    references public.households(id) on delete cascade,
  billing_owner_user_id uuid,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint family_billing_accounts_owner_membership_fkey
    foreign key (household_id, billing_owner_user_id)
    references public.household_members(household_id, user_id)
    on delete set null (billing_owner_user_id)
);

create index family_billing_accounts_owner_idx
  on public.family_billing_accounts (billing_owner_user_id, household_id)
  where billing_owner_user_id is not null;

create or replace function private.validate_family_billing_owner()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  if new.billing_owner_user_id is not null and not exists (
    select 1
    from public.household_members as member
    where member.household_id = new.household_id
      and member.user_id = new.billing_owner_user_id
      and member.role = 'owner'
  ) then
    raise exception using
      errcode = '23514',
      message = 'Billing owner must be an Owner of the same Family';
  end if;

  new.updated_at := now();
  return new;
end;
$$;

revoke execute on function private.validate_family_billing_owner()
  from public, anon, authenticated, service_role;

create trigger family_billing_accounts_validate_owner
  before insert or update of household_id, billing_owner_user_id
  on public.family_billing_accounts
  for each row execute function private.validate_family_billing_owner();

create or replace function private.broadcast_latest_position()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
  household_access_version integer;
begin
  select household.access_version
  into strict household_access_version
  from public.households as household
  where household.id = new.household_id;

  perform realtime.broadcast_changes(
    'household:' || new.household_id::text || ':v' || household_access_version::text,
    tg_op,
    tg_op,
    tg_table_name,
    tg_table_schema,
    new,
    old
  );
  return null;
end;
$$;

revoke execute on function private.broadcast_latest_position()
  from public, anon, authenticated, service_role;

create or replace function private.broadcast_family_access_change()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  perform realtime.broadcast_changes(
    'household:' || old.id::text || ':v' || old.access_version::text,
    'ACCESS_CHANGED',
    tg_op,
    tg_table_name,
    tg_table_schema,
    new,
    old
  );
  return null;
end;
$$;

revoke execute on function private.broadcast_family_access_change()
  from public, anon, authenticated, service_role;

create trigger households_broadcast_access_change
  after update of access_version on public.households
  for each row
  when (old.access_version is distinct from new.access_version)
  execute function private.broadcast_family_access_change();

drop policy if exists "Household members receive position broadcasts" on realtime.messages;
create policy "Household members receive position broadcasts"
  on realtime.messages for select to authenticated
  using (
    extension = 'broadcast'
    and exists (
      select 1
      from public.household_members as member
      join public.households as household on household.id = member.household_id
      where member.user_id = (select auth.uid())
        and (select realtime.topic()) =
          'household:' || household.id::text || ':v' || household.access_version::text
    )
  );

insert into public.family_billing_accounts (household_id, billing_owner_user_id)
select household.id, billing_owner.user_id
from public.households as household
join lateral (
  select member.user_id
  from public.household_members as member
  where member.household_id = household.id
    and member.role = 'owner'
  order by
    (member.user_id = household.created_by) desc,
    member.joined_at,
    member.user_id
  limit 1
) as billing_owner on true
where household.kind = 'customer'
on conflict (household_id) do nothing;

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

  insert into public.family_billing_accounts (household_id, billing_owner_user_id)
  values (created_household_id, caller_id);

  insert into public.profiles (user_id, display_name, active_household_id)
  values (caller_id, normalized_display_name, created_household_id)
  on conflict (user_id) do update
  set
    display_name = excluded.display_name,
    active_household_id = excluded.active_household_id;

  return created_household_id;
end;
$$;

create or replace function private.bluepaws_remove_family_member(
  requested_household_id uuid,
  requested_user_id uuid
)
returns boolean
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  target_role text;
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if requested_user_id = caller_id then
    raise exception using errcode = '42501', message = 'Owners cannot remove themselves';
  end if;

  perform pg_advisory_xact_lock(
    hashtextextended(
      'bluepaws-family-member:' || requested_household_id::text || ':' || requested_user_id::text,
      0
    )
  );

  if not private.current_user_is_household_owner(requested_household_id) then
    raise exception using errcode = '42501', message = 'Only the Family owner can remove members';
  end if;

  select member.role
  into target_role
  from public.household_members as member
  where member.household_id = requested_household_id
    and member.user_id = requested_user_id
  for update;

  if target_role is distinct from 'member' then
    raise exception using errcode = 'P0002', message = 'Family member not found';
  end if;

  delete from public.household_members as member
  where member.household_id = requested_household_id
    and member.user_id = requested_user_id
    and member.role = 'member';

  update public.households
  set access_version = access_version + 1
  where id = requested_household_id;

  update public.profiles as profile
  set active_household_id = (
    select remaining_member.household_id
    from public.household_members as remaining_member
    where remaining_member.user_id = requested_user_id
    order by
      case remaining_member.role when 'owner' then 0 else 1 end,
      remaining_member.joined_at,
      remaining_member.household_id
    limit 1
  )
  where profile.user_id = requested_user_id
    and profile.active_household_id = requested_household_id;

  return true;
end;
$$;

create or replace function public.bluepaws_remove_family_member(
  requested_household_id uuid,
  requested_user_id uuid
)
returns boolean
language sql
security invoker
set search_path = ''
as $$
  select private.bluepaws_remove_family_member(requested_household_id, requested_user_id);
$$;

revoke all on function private.bluepaws_remove_family_member(uuid, uuid)
  from public, anon, authenticated, service_role;
grant usage on schema private to authenticated;
grant execute on function private.bluepaws_remove_family_member(uuid, uuid)
  to authenticated;

revoke all on function public.bluepaws_remove_family_member(uuid, uuid)
  from public, anon, authenticated;
grant execute on function public.bluepaws_remove_family_member(uuid, uuid)
  to authenticated;

alter table public.family_billing_accounts enable row level security;

create policy "Billing owners read their own Family billing accounts"
  on public.family_billing_accounts for select to authenticated
  using (billing_owner_user_id = (select auth.uid()));

revoke all on table public.family_billing_accounts from anon, authenticated;
grant select on table public.family_billing_accounts to authenticated;

comment on table public.family_billing_accounts is
  'Family-level billing authority, kept separate from ordinary Family membership and future provider subscription records.';
comment on column public.family_billing_accounts.billing_owner_user_id is
  'The Family Owner allowed to view and manage this Family billing relationship; null requires administrative recovery.';
comment on function public.bluepaws_remove_family_member(uuid, uuid) is
  'Allows a Family Owner to remove an accepted Member without affecting that user''s identity or other Family memberships.';
