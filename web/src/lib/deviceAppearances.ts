import { createClient } from "@/lib/supabase/client";
import type { DeviceAvatar } from "@/types/telemetry";

export const PET_AVATAR_BUCKET = "pet-avatars";

interface DeviceAppearanceRow {
  device_id: number;
  avatar_kind: "emoji" | "photo";
  emoji_value: string;
  marker_colour: string;
  avatar_storage_path: string | null;
}

export interface SaveDeviceAppearance {
  deviceId: number;
  householdId: string;
  kind: "emoji" | "photo";
  emoji: string;
  color: string;
  previousStoragePath?: string;
  preparedPhoto?: Blob;
}

export async function loadDeviceAppearances(householdId: string) {
  const supabase = createClient();
  const { data, error } = await supabase
    .from("device_appearances")
    .select("device_id, avatar_kind, emoji_value, marker_colour, avatar_storage_path")
    .eq("household_id", householdId);

  if (error) throw error;

  const avatars: Record<number, DeviceAvatar> = {};
  await Promise.all((data as DeviceAppearanceRow[]).map(async (row) => {
    let photoUrl: string | undefined;
    if (row.avatar_kind === "photo" && row.avatar_storage_path) {
      const { data: photo, error: photoError } = await supabase.storage
        .from(PET_AVATAR_BUCKET)
        .download(row.avatar_storage_path);
      if (!photoError && photo) photoUrl = URL.createObjectURL(photo);
    }

    avatars[row.device_id] = {
      kind: photoUrl ? "photo" : "emoji",
      emoji: row.emoji_value,
      color: row.marker_colour,
      photoUrl,
      storagePath: row.avatar_storage_path ?? undefined,
    };
  }));
  return avatars;
}

export async function saveDeviceAppearance(input: SaveDeviceAppearance) {
  const supabase = createClient();
  let storagePath = input.kind === "photo" ? input.previousStoragePath : undefined;
  let uploadedPath: string | undefined;

  if (input.kind === "photo" && input.preparedPhoto) {
    uploadedPath = `${input.householdId}/${input.deviceId}/${crypto.randomUUID()}.webp`;
    const { error: uploadError } = await supabase.storage
      .from(PET_AVATAR_BUCKET)
      .upload(uploadedPath, input.preparedPhoto, { contentType: "image/webp", upsert: false });
    if (uploadError) throw uploadError;
    storagePath = uploadedPath;
  }

  if (input.kind === "photo" && !storagePath) {
    throw new Error("Choose a photo before saving");
  }

  const { error } = await supabase.from("device_appearances").upsert({
    device_id: input.deviceId,
    household_id: input.householdId,
    avatar_kind: input.kind,
    emoji_value: input.emoji,
    marker_colour: input.color,
    avatar_storage_path: storagePath ?? null,
  }, { onConflict: "device_id" });

  if (error) {
    if (uploadedPath) await supabase.storage.from(PET_AVATAR_BUCKET).remove([uploadedPath]);
    throw error;
  }

  if (input.previousStoragePath && input.previousStoragePath !== storagePath) {
    await supabase.storage.from(PET_AVATAR_BUCKET).remove([input.previousStoragePath]);
  }
}

export function revokeAvatarUrls(avatars: Record<number, DeviceAvatar>) {
  Object.values(avatars).forEach((avatar) => {
    if (avatar.photoUrl?.startsWith("blob:")) URL.revokeObjectURL(avatar.photoUrl);
  });
}
