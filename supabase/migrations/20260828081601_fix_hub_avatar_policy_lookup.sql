-- The gateway provisioning registry is deliberately not browser-readable.
-- Authorize photos through hub_presence, whose SELECT RLS already limits
-- visibility to current Family owners/members. Do not grant registry access.
alter policy "Family reads hub photos" on storage.objects
using(bucket_id='hub-avatars' and exists(
  select 1 from public.hub_presence h
  where h.household_id::text=(storage.foldername(name))[1]
    and h.gateway_guid16::text=(storage.foldername(name))[2]));

alter policy "Family uploads hub photos" on storage.objects
with check(bucket_id='hub-avatars' and name ~ '^[0-9a-f-]{36}/[0-9]+/[0-9a-f-]{36}\.webp$' and exists(
  select 1 from public.hub_presence h
  where h.household_id::text=(storage.foldername(name))[1]
    and h.gateway_guid16::text=(storage.foldername(name))[2]));

alter policy "Family deletes hub photos" on storage.objects
using(bucket_id='hub-avatars' and exists(
  select 1 from public.hub_presence h
  where h.household_id::text=(storage.foldername(name))[1]
    and h.gateway_guid16::text=(storage.foldername(name))[2]));
