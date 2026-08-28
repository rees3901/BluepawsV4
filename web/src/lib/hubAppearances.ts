import { createClient } from "@/lib/supabase/client";
import type { SaveDeviceAppearance } from "./deviceAppearances";
import { normalizeDeviceName } from "./deviceName";

export const HUB_AVATAR_BUCKET = "hub-avatars";

export async function saveHubAppearance(input: SaveDeviceAppearance, mode: "home" | "portable" | "off_grid") {
  const gatewayId = -input.deviceId; // Negative keys exist only in the dashboard.
  if (!Number.isInteger(gatewayId) || gatewayId <= 0 || gatewayId > 65535 || gatewayId % 16) throw new Error("Invalid hub identity");
  const name = normalizeDeviceName(input.name, true);
  const db = createClient();
  let path = input.kind === "photo" ? input.previousStoragePath : undefined;
  let uploaded: string | undefined;
  if (input.kind === "photo" && input.preparedPhoto) {
    uploaded = `${input.householdId}/${gatewayId}/${crypto.randomUUID()}.webp`;
    const { error } = await db.storage.from(HUB_AVATAR_BUCKET).upload(uploaded, input.preparedPhoto, {contentType:"image/webp", upsert:false});
    if (error) throw error;
    path = uploaded;
  }
  if (input.kind === "photo" && !path) throw new Error("Choose a photo before saving");
  try {
    const {data, error} = await db.from("hub_presence").update({
      display_name: name,
      avatar_kind: input.kind, avatar_storage_path: path ?? null, marker_colour: input.color,
      [mode === "home" ? "home_emoji" : "portable_emoji"]: input.emoji,
    }).eq("gateway_guid16", gatewayId).eq("household_id", input.householdId).select("gateway_guid16");
    if (error) throw error;
    if (!data?.length) throw new Error("Hub appearance could not be saved for this Family");
  } catch (error) {
    if (uploaded) await db.storage.from(HUB_AVATAR_BUCKET).remove([uploaded]);
    throw error;
  }
  if (input.previousStoragePath && input.previousStoragePath !== path) await db.storage.from(HUB_AVATAR_BUCKET).remove([input.previousStoragePath]);
}
