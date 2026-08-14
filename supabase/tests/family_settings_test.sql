begin;
select plan(9);

select has_function(
  'public',
  'bluepaws_list_family_members',
  array['uuid'],
  'Family member listing RPC exists'
);
select has_function(
  'public',
  'bluepaws_preview_family_invitation',
  array['text'],
  'email-bound invitation preview RPC exists'
);

insert into auth.users (
  instance_id, id, aud, role, email, encrypted_password, email_confirmed_at,
  raw_app_meta_data, raw_user_meta_data, created_at, updated_at
)
values
  ('00000000-0000-0000-0000-000000000000', '20000000-0000-0000-0000-000000000001', 'authenticated', 'authenticated', 'owner-settings@example.com', '', now(), '{}', '{"full_name":"Settings Owner"}', now(), now()),
  ('00000000-0000-0000-0000-000000000000', '20000000-0000-0000-0000-000000000002', 'authenticated', 'authenticated', 'member-settings@example.com', '', now(), '{}', '{"full_name":"Settings Member"}', now(), now()),
  ('00000000-0000-0000-0000-000000000000', '20000000-0000-0000-0000-000000000003', 'authenticated', 'authenticated', 'outsider-settings@example.com', '', now(), '{}', '{"full_name":"Settings Outsider"}', now(), now());

insert into public.households (id, name, kind, created_by)
values ('20000000-0000-0000-0000-000000000010', 'Settings Test Family', 'customer', '20000000-0000-0000-0000-000000000001');

insert into public.household_members (household_id, user_id, role)
values
  ('20000000-0000-0000-0000-000000000010', '20000000-0000-0000-0000-000000000001', 'owner'),
  ('20000000-0000-0000-0000-000000000010', '20000000-0000-0000-0000-000000000002', 'member');

insert into public.household_invitations (
  id, household_id, email, role, token_hash, invited_by, expires_at
)
values (
  '20000000-0000-0000-0000-000000000020',
  '20000000-0000-0000-0000-000000000010',
  'member-settings@example.com',
  'member',
  extensions.digest(convert_to(repeat('a', 64), 'UTF8'), 'sha256'),
  '20000000-0000-0000-0000-000000000001',
  now() + interval '7 days'
);

set local role authenticated;
select set_config('request.jwt.claim.sub', '20000000-0000-0000-0000-000000000001', true);

select is(
  (select count(*) from public.bluepaws_list_family_members('20000000-0000-0000-0000-000000000010')),
  2::bigint,
  'an Owner can list everyone in their Family'
);
select is(
  (select email from public.bluepaws_list_family_members('20000000-0000-0000-0000-000000000010') where role = 'member'),
  'member-settings@example.com',
  'Family list includes the member email needed for account management'
);

select set_config('request.jwt.claim.sub', '20000000-0000-0000-0000-000000000002', true);
select is(
  (select count(*) from public.bluepaws_list_family_members('20000000-0000-0000-0000-000000000010')),
  2::bigint,
  'a Member can see the people in their own Family'
);
select is(
  (select family_name from public.bluepaws_preview_family_invitation(repeat('a', 64))),
  'Settings Test Family',
  'the invited verified email can preview its active invitation'
);

select set_config('request.jwt.claim.sub', '20000000-0000-0000-0000-000000000003', true);
select throws_ok(
  $$select * from public.bluepaws_list_family_members('20000000-0000-0000-0000-000000000010')$$,
  '42501',
  'Family membership required',
  'an unrelated account cannot list another Family'
);
select is(
  (select count(*) from public.bluepaws_preview_family_invitation(repeat('a', 64))),
  0::bigint,
  'an unrelated email cannot preview a valid invitation token'
);
select is(
  (select count(*) from public.bluepaws_preview_family_invitation('not-a-token')),
  0::bigint,
  'an invalid token reveals no invitation data'
);

reset role;
select * from finish();
rollback;
