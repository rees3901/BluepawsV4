-- Household-scoped pet marker customisation and private avatar storage.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.devices
  add constraint devices_device_household_unique unique (device_id, household_id);

create table public.device_appearances (
  device_id integer not null,
  household_id uuid not null,
  avatar_kind text not null default 'emoji',
  emoji_value text not null default '🐾',
  marker_colour text not null default '#1d9bf0',
  avatar_storage_path text,
  updated_by uuid references auth.users(id) on delete set null,
  updated_at timestamptz not null default now(),
  primary key (device_id),
  constraint device_appearances_device_household_fk
    foreign key (device_id, household_id)
    references public.devices (device_id, household_id)
    on delete cascade,
  constraint device_appearances_device_id_range check (device_id between 1 and 65535),
  constraint device_appearances_avatar_kind_check check (avatar_kind in ('emoji', 'photo')),
  constraint device_appearances_emoji_length check (char_length(emoji_value) between 1 and 16),
  constraint device_appearances_marker_colour_check check (marker_colour ~ '^#[0-9a-fA-F]{6}$'),
  constraint device_appearances_photo_path_check check (
    (avatar_kind = 'emoji' and avatar_storage_path is null)
    or
    (
      avatar_kind = 'photo'
      and avatar_storage_path like household_id::text || '/' || device_id::text || '/%'
    )
  )
);

create index device_appearances_household_device_idx
  on public.device_appearances (household_id, device_id);

create index device_appearances_updated_by_idx
  on public.device_appearances (updated_by)
  where updated_by is not null;

create or replace function private.set_device_appearance_audit()
returns trigger
language plpgsql
security invoker
set search_path = ''
as $$
begin
  new.updated_by := auth.uid();
  new.updated_at := now();
  return new;
end;
$$;

revoke execute on function private.set_device_appearance_audit()
  from public, anon, authenticated;

create trigger device_appearances_set_audit
  before insert or update on public.device_appearances
  for each row execute function private.set_device_appearance_audit();

alter table public.device_appearances enable row level security;

create policy "Household members read device appearances"
  on public.device_appearances for select to authenticated
  using (
    exists (
      select 1
      from public.household_members as member
      where member.household_id = device_appearances.household_id
        and member.user_id = (select auth.uid())
    )
  );

create policy "Household members create device appearances"
  on public.device_appearances for insert to authenticated
  with check (
    exists (
      select 1
      from public.household_members as member
      where member.household_id = device_appearances.household_id
        and member.user_id = (select auth.uid())
    )
  );

create policy "Household members update device appearances"
  on public.device_appearances for update to authenticated
  using (
    exists (
      select 1
      from public.household_members as member
      where member.household_id = device_appearances.household_id
        and member.user_id = (select auth.uid())
    )
  )
  with check (
    exists (
      select 1
      from public.household_members as member
      where member.household_id = device_appearances.household_id
        and member.user_id = (select auth.uid())
    )
  );

revoke all on table public.device_appearances from anon, authenticated;
grant select, insert, update on table public.device_appearances to authenticated;

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values (
  'pet-avatars',
  'pet-avatars',
  false,
  1048576,
  array['image/webp', 'image/jpeg']
)
on conflict (id) do update
set
  public = excluded.public,
  file_size_limit = excluded.file_size_limit,
  allowed_mime_types = excluded.allowed_mime_types;

create policy "Household members read pet avatars"
  on storage.objects for select to authenticated
  using (
    bucket_id = 'pet-avatars'
    and exists (
      select 1
      from public.household_members as member
      join public.devices as device
        on device.household_id = member.household_id
      where member.user_id = (select auth.uid())
        and member.household_id::text = (storage.foldername(name))[1]
        and device.device_id::text = (storage.foldername(name))[2]
    )
  );

create policy "Household members upload pet avatars"
  on storage.objects for insert to authenticated
  with check (
    bucket_id = 'pet-avatars'
    and exists (
      select 1
      from public.household_members as member
      join public.devices as device
        on device.household_id = member.household_id
      where member.user_id = (select auth.uid())
        and member.household_id::text = (storage.foldername(name))[1]
        and device.device_id::text = (storage.foldername(name))[2]
    )
  );

create policy "Household members delete pet avatars"
  on storage.objects for delete to authenticated
  using (
    bucket_id = 'pet-avatars'
    and exists (
      select 1
      from public.household_members as member
      join public.devices as device
        on device.household_id = member.household_id
      where member.user_id = (select auth.uid())
        and member.household_id::text = (storage.foldername(name))[1]
        and device.device_id::text = (storage.foldername(name))[2]
    )
  );

comment on table public.device_appearances is
  'Household-visible pet marker appearance. Photo objects remain private in Storage.';
