-- Hub photos use private storage and the existing Family-scoped preferences row.
alter table public.hub_presence
  add column avatar_kind text not null default 'emoji' check (avatar_kind in ('emoji','photo')),
  add column avatar_storage_path text,
  add constraint hub_avatar_path check (
    (avatar_kind='emoji' and avatar_storage_path is null) or
    (avatar_kind='photo' and avatar_storage_path is not null
      and avatar_storage_path like household_id::text || '/' || gateway_guid16::text || '/%'
      and avatar_storage_path ~ '^[0-9a-f-]{36}/[0-9]+/[0-9a-f-]{36}\.webp$'));
grant update(avatar_kind,avatar_storage_path) on public.hub_presence to authenticated;

-- A gateway transferred between Families must not retain a pointer to old private media.
create function private.bluepaws_reset_transferred_hub_photo() returns trigger
language plpgsql security invoker set search_path='' as $$
begin
  if new.household_id <> old.household_id then
    new.avatar_kind := 'emoji'; new.avatar_storage_path := null;
  end if;
  return new;
end $$;
revoke all on function private.bluepaws_reset_transferred_hub_photo() from public,anon,authenticated;
create trigger hub_reset_transferred_photo before update of household_id on public.hub_presence
for each row execute function private.bluepaws_reset_transferred_hub_photo();

insert into storage.buckets(id,name,public,file_size_limit,allowed_mime_types)
values('hub-avatars','hub-avatars',false,1048576,array['image/webp'])
on conflict(id) do update set public=false, file_size_limit=1048576, allowed_mime_types=array['image/webp'];

-- Separate bucket prevents collision with collars that happen to have the same ID.
create policy "Family reads hub photos" on storage.objects for select to authenticated
using(bucket_id='hub-avatars' and exists(
  select 1 from public.gateways g join public.household_members m on m.household_id=g.household_id
  where m.user_id=(select auth.uid()) and m.role in ('owner','member')
    and g.household_id::text=(storage.foldername(name))[1]
    and g.gateway_guid16::text=(storage.foldername(name))[2]));
create policy "Family uploads hub photos" on storage.objects for insert to authenticated
with check(bucket_id='hub-avatars' and name ~ '^[0-9a-f-]{36}/[0-9]+/[0-9a-f-]{36}\.webp$' and exists(
  select 1 from public.gateways g join public.household_members m on m.household_id=g.household_id
  where m.user_id=(select auth.uid()) and m.role in ('owner','member')
    and g.household_id::text=(storage.foldername(name))[1]
    and g.gateway_guid16::text=(storage.foldername(name))[2]));
create policy "Family deletes hub photos" on storage.objects for delete to authenticated
using(bucket_id='hub-avatars' and exists(
  select 1 from public.gateways g join public.household_members m on m.household_id=g.household_id
  where m.user_id=(select auth.uid()) and m.role in ('owner','member')
    and g.household_id::text=(storage.foldername(name))[1]
    and g.gateway_guid16::text=(storage.foldername(name))[2]));
-- Uploads always use a new UUID filename. No UPDATE/upsert object permission is needed.
