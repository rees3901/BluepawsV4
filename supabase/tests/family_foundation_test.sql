begin;
select plan(19);

select has_table('public', 'profiles', 'profiles table exists');
select has_table('public', 'household_invitations', 'Family invitations table exists');
select has_function(
  'public',
  'bluepaws_create_family',
  array['text', 'text'],
  'authenticated Family creation RPC exists'
);
select has_function(
  'public',
  'bluepaws_accept_family_invitation',
  array['text', 'text'],
  'authenticated Family invitation acceptance RPC exists'
);

insert into auth.users (
  instance_id,
  id,
  aud,
  role,
  email,
  encrypted_password,
  email_confirmed_at,
  raw_app_meta_data,
  raw_user_meta_data,
  created_at,
  updated_at
)
values
  (
    '00000000-0000-0000-0000-000000000000',
    '10000000-0000-0000-0000-000000000001',
    'authenticated',
    'authenticated',
    'alice@example.com',
    '',
    now(),
    '{}',
    '{"full_name":"Alice Jones"}',
    now(),
    now()
  ),
  (
    '00000000-0000-0000-0000-000000000000',
    '10000000-0000-0000-0000-000000000002',
    'authenticated',
    'authenticated',
    'bob@example.com',
    '',
    now(),
    '{}',
    '{"full_name":"Bob Jones"}',
    now(),
    now()
  ),
  (
    '00000000-0000-0000-0000-000000000000',
    '10000000-0000-0000-0000-000000000003',
    'authenticated',
    'authenticated',
    'eve@example.com',
    '',
    now(),
    '{}',
    '{"full_name":"Eve Example"}',
    now(),
    now()
  );

create temporary table family_test_state (
  family_id uuid,
  invitation_id uuid,
  invitation_token text
) on commit drop;
grant select, insert, update on table family_test_state to authenticated;

set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000001', true);
select set_config('request.jwt.claim.role', 'authenticated', true);

insert into family_test_state (family_id)
values (public.bluepaws_create_family('The Jones Family', 'Alice Jones'));

reset role;

select is(
  (
    select member.role
    from public.household_members as member
    join family_test_state as state on state.family_id = member.household_id
    where member.user_id = '10000000-0000-0000-0000-000000000001'
  ),
  'owner',
  'Family creator becomes an Owner'
);

select is(
  (
    select profile.active_household_id
    from public.profiles as profile
    where profile.user_id = '10000000-0000-0000-0000-000000000001'
  ),
  (select family_id from family_test_state),
  'new Family becomes the owner profile active Family'
);

set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000001', true);

select is(
  public.bluepaws_create_family('Ignored duplicate Family', 'Alice Jones'),
  (select family_id from family_test_state),
  'a repeated onboarding request resolves to the existing Family'
);

reset role;

select is(
  (
    select count(*)
    from public.households
    where created_by = '10000000-0000-0000-0000-000000000001'
      and kind = 'customer'
  ),
  1::bigint,
  'repeated onboarding cannot create a duplicate Family'
);

select is(
  (
    select count(*)
    from public.household_members
    where user_id = '10000000-0000-0000-0000-000000000002'
  ),
  0::bigint,
  'new auth users are not silently assigned to a Family'
);

set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000002', true);

select is(
  (select count(*) from public.households),
  0::bigint,
  'an unrelated authenticated user cannot read another Family'
);

reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000001', true);

with created_invitation as (
  select *
  from public.bluepaws_create_family_invitation(
    (select family_id from family_test_state),
    'bob@example.com'
  )
)
update family_test_state as state
set
  invitation_id = created.invitation_id,
  invitation_token = created.invitation_token
from created_invitation as created;

select ok(
  (select length(invitation_token) = 64 from family_test_state),
  'owner receives a 256-bit one-time invitation token'
);

reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000002', true);

select lives_ok(
  format(
    'select public.bluepaws_accept_family_invitation(%L, %L)',
    (select invitation_token from family_test_state),
    'Bob Jones'
  ),
  'the invited verified email can accept the invitation'
);

reset role;

select is(
  (
    select member.role
    from public.household_members as member
    join family_test_state as state on state.family_id = member.household_id
    where member.user_id = '10000000-0000-0000-0000-000000000002'
  ),
  'member',
  'accepted invite creates a non-administrative member'
);

select is(
  (
    select profile.active_household_id
    from public.profiles as profile
    where profile.user_id = '10000000-0000-0000-0000-000000000002'
  ),
  (select family_id from family_test_state),
  'accepted Family becomes the member active Family'
);

select is(
  (
    select count(*)
    from public.household_invitations
    where id = (select invitation_id from family_test_state)
      and accepted_by = '10000000-0000-0000-0000-000000000002'
      and accepted_at is not null
  ),
  1::bigint,
  'invitation is consumed exactly once'
);

set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000002', true);

select throws_ok(
  format(
    'select public.bluepaws_create_family_invitation(%L::uuid, %L)',
    (select family_id from family_test_state),
    'eve@example.com'
  ),
  '42501',
  'Only the Family owner can invite members',
  'members cannot issue permanent Family invitations'
);

reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000001', true);

with created_invitation as (
  select *
  from public.bluepaws_create_family_invitation(
    (select family_id from family_test_state),
    'not-eve@example.com'
  )
)
update family_test_state as state
set
  invitation_id = created.invitation_id,
  invitation_token = created.invitation_token
from created_invitation as created;

reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000003', true);

select throws_ok(
  format(
    'select public.bluepaws_accept_family_invitation(%L, %L)',
    (select invitation_token from family_test_state),
    'Eve Example'
  ),
  '42501',
  'Invitation is invalid or expired',
  'a valid token cannot be claimed by a different email address'
);

reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub', '10000000-0000-0000-0000-000000000001', true);

select ok(
  public.bluepaws_revoke_family_invitation(
    (select invitation_id from family_test_state)
  ),
  'owner can revoke an unused invitation'
);

reset role;

select is(
  (
    select count(*)
    from public.household_invitations
    where id = (select invitation_id from family_test_state)
      and revoked_at is not null
  ),
  1::bigint,
  'revoked invitation is recorded as unusable'
);

select * from finish();
rollback;
