import { defaultDeviceAvatar } from "@/lib/defaultDeviceAvatar";
import { createClient } from "@/lib/supabase/server";
import { isPositionRow, positionToTelemetryDevice, type PositionRow } from "@/lib/telemetryRows";
import { normalizeMarkerColor } from "@/lib/markerColor";
import type { DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

const SEARCH_TOKEN_PATTERN = /^[0-9a-f]{64}$/i;

export interface SearchPartySnapshot {
  valid: boolean;
  reason: "invalid" | "expired_or_revoked" | null;
  householdId: string | null;
  familyName: string | null;
  expiresAt: string | null;
  devices: TelemetryDevice[];
  avatars: Record<number, DeviceAvatar>;
  error: string | null;
}

interface SearchPartyDeviceRow extends PositionRow {
  avatar_kind?: unknown;
  emoji_value?: unknown;
  marker_colour?: unknown;
}

export function isSearchPartyToken(value: string) {
  return SEARCH_TOKEN_PATTERN.test(value);
}

export async function loadSearchPartySnapshot(token: string): Promise<SearchPartySnapshot> {
  if (!isSearchPartyToken(token)) return invalidSnapshot("invalid");

  try {
    const supabase = await createClient();
    const { data, error } = await supabase.rpc("bluepaws_get_search_party_snapshot", {
      share_token: token.toLowerCase(),
    });

    if (error) throw error;
    return parseSearchPartySnapshot(data);
  } catch (error) {
    console.error("Unable to load search party snapshot", error);
    return {
      ...invalidSnapshot("invalid"),
      error: "The search-party map could not be loaded. Try the link again, or ask the Family Owner for a fresh link.",
    };
  }
}

export function parseSearchPartySnapshot(value: unknown): SearchPartySnapshot {
  if (!value || typeof value !== "object") return invalidSnapshot("invalid");

  const snapshot = value as Record<string, unknown>;
  if (snapshot.valid !== true) {
    const reason = snapshot.reason === "expired_or_revoked" ? "expired_or_revoked" : "invalid";
    return invalidSnapshot(reason);
  }

  const rawDevices = Array.isArray(snapshot.devices) ? snapshot.devices : [];
  const devices: TelemetryDevice[] = [];
  const avatars: Record<number, DeviceAvatar> = {};

  rawDevices.forEach((rawDevice) => {
    if (!isPositionRow(rawDevice)) return;
    const row = rawDevice as SearchPartyDeviceRow;
    const device = positionToTelemetryDevice(row);
    devices.push(device);
    avatars[device.id] = appearanceAvatar(device.id, row);
  });

  return {
    valid: true,
    reason: null,
    householdId: typeof snapshot.householdId === "string" ? snapshot.householdId : null,
    familyName: typeof snapshot.familyName === "string" ? snapshot.familyName : "Bluepaws Family",
    expiresAt: typeof snapshot.expiresAt === "string" ? snapshot.expiresAt : null,
    devices,
    avatars,
    error: null,
  };
}

function invalidSnapshot(reason: SearchPartySnapshot["reason"]): SearchPartySnapshot {
  return {
    valid: false,
    reason,
    householdId: null,
    familyName: null,
    expiresAt: null,
    devices: [],
    avatars: {},
    error: null,
  };
}

function appearanceAvatar(deviceId: number, row: SearchPartyDeviceRow): DeviceAvatar {
  const fallback = defaultDeviceAvatar(deviceId);
  const emoji = typeof row.emoji_value === "string" && row.emoji_value.trim()
    ? row.emoji_value
    : fallback.emoji;
  const color = typeof row.marker_colour === "string"
    ? normalizeMarkerColor(row.marker_colour)
    : fallback.color;

  return {
    kind: "emoji",
    emoji,
    color,
  };
}
