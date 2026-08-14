-- Read models needed by the customer-facing Family settings and secure
-- invitation join screens. Authorization remains tied to household_members;
-- the friendly "Family" name is presentation terminology only.

set lock_timeout = '5s';
set statement_timeout = '120s';

create or replace function private.bluepaws_list_family_members(
  requested_household_id uuid
)
returns table (
  user_id uuid,
  display_name text,
  email text,
  role text,
  joined_at timestamptz
)
language plpgsql
stable
security definer
set search_path = ''
as $$
begin
  if (select auth.uid()) is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if not exists (
    select 1
    from public.household_members as caller_membership
    where caller_membership.household_id = requested_household_id
      and caller_membership.user_id = (select auth.uid())
  ) then
    raise exception using errcode = '42501', message = 'Family membership required';
  end if;

  return query
  select
    member.user_id,
    profile.display_name,
    lower(btrim(bluepaws_user.email)) as email,
    member.role,
    member.joined_at
  from public.household_members as member
  join auth.users as bluepaws_user on bluepaws_user.id = member.user_id
  left join public.profiles as profile on profile.user_id = member.user_id
  where member.household_id = requested_household_id
  order by
    case member.role when 'owner' then 0 else 1 end,
    lower(coalesce(profile.display_name, bluepaws_user.email, '')),
    member.joined_at;
end;
$$;

create or replace function private.bluepaws_preview_family_invitation(
  invitation_token text
)
returns table (
  household_id uuid,
  family_name text,
  invited_email text,
  expires_at timestamptz
)
language plpgsql
stable
security definer
set search_path = ''
as $$
declare
  caller_id uuid := (select auth.uid());
  caller_email text;
begin
  if caller_id is null then
    raise exception using errcode = '28000', message = 'Authentication required';
  end if;

  if invitation_token is null or invitation_token !~ '^[0-9a-f]{64}$' then
    return;
  end if;

  select lower(btrim(bluepaws_user.email))
  into caller_email
  from auth.users as bluepaws_user
  where bluepaws_user.id = caller_id
    and bluepaws_user.email_confirmed_at is not null;

  if caller_email is null then
    return;
  end if;

  return query
  select
    invitation.household_id,
    household.name,
    invitation.email,
    invitation.expires_at
  from public.household_invitations as invitation
  join public.households as household on household.id = invitation.household_id
  where invitation.token_hash = extensions.digest(convert_to(invitation_token, 'UTF8'), 'sha256')
    and invitation.email = caller_email
    and invitation.accepted_at is null
    and invitation.revoked_at is null
    and invitation.expires_at > now();
end;
$$;

create or replace function public.bluepaws_list_family_members(
  requested_household_id uuid
)
returns table (
  user_id uuid,
  display_name text,
  email text,
  role text,
  joined_at timestamptz
)
language sql
stable
security invoker
set search_path = ''
as $$
  select * from private.bluepaws_list_family_members(requested_household_id);
$$;

create or replace function public.bluepaws_preview_family_invitation(
  invitation_token text
)
returns table (
  household_id uuid,
  family_name text,
  invited_email text,
  expires_at timestamptz
)
language sql
stable
security invoker
set search_path = ''
as $$
  select * from private.bluepaws_preview_family_invitation(invitation_token);
$$;

revoke all on function private.bluepaws_list_family_members(uuid)
  from public, anon, authenticated, service_role;
revoke all on function private.bluepaws_preview_family_invitation(text)
  from public, anon, authenticated, service_role;

grant usage on schema private to authenticated;
grant execute on function private.bluepaws_list_family_members(uuid) to authenticated;
grant execute on function private.bluepaws_preview_family_invitation(text) to authenticated;

revoke all on function public.bluepaws_list_family_members(uuid)
  from public, anon, authenticated;
revoke all on function public.bluepaws_preview_family_invitation(text)
  from public, anon, authenticated;

grant execute on function public.bluepaws_list_family_members(uuid) to authenticated;
grant execute on function public.bluepaws_preview_family_invitation(text) to authenticated;

comment on function public.bluepaws_list_family_members(uuid) is
  'Lists people in a Family only when the caller belongs to that Family.';
comment on function public.bluepaws_preview_family_invitation(text) is
  'Previews an active invitation only for its authenticated, verified email recipient.';
