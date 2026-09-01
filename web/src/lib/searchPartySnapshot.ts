import { defaultDeviceAvatar } from "./defaultDeviceAvatar.ts";
import { normalizeMarkerColor } from "./markerColor.ts";
import { isPositionRow, positionToTelemetryDevice, type PositionRow } from "./telemetryRows.ts";
import type { DeviceAvatar, TelemetryDevice, TrailPoint } from "../types/telemetry.ts";

const SEARCH_TOKEN_PATTERN = /^[0-9a-f]{64}$/i;

export interface SearchPartySnapshot {
  valid: boolean;
  reason: "invalid" | "expired_or_revoked" | null;
  householdId: string | null;
  familyName: string | null;
  expiresAt: string | null;
  devices: TelemetryDevice[];
  avatars: Record<number, DeviceAvatar>;
  trailHistory: Record<number, TrailPoint[]>;
  error: string | null;
}

interface SearchPartyDeviceRow extends PositionRow {
  display_name?: unknown;
  avatar_kind?: unknown;
  emoji_value?: unknown;
  marker_colour?: unknown;
}

interface SearchPartyHubRow {
  gateway_guid16?: unknown;
  display_name?: unknown;
  mode?: unknown;
  received_at?: unknown;
  latitude?: unknown;
  longitude?: unknown;
  fix_at?: unknown;
  avatar_kind?: unknown;
  home_emoji?: unknown;
  portable_emoji?: unknown;
  marker_colour?: unknown;
}

export function isSearchPartyToken(value: string) {
  return SEARCH_TOKEN_PATTERN.test(value);
}

export function parseSearchPartySnapshot(value: unknown, token?: string): SearchPartySnapshot {
  if (!value || typeof value !== "object") return invalidSnapshot("invalid");
  const snapshot = value as Record<string, unknown>;
  if (snapshot.valid !== true) return invalidSnapshot(snapshot.reason === "expired_or_revoked" ? "expired_or_revoked" : "invalid");

  const devices: TelemetryDevice[] = [];
  const avatars: Record<number, DeviceAvatar> = {};
  const avatarBaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL;
  const rawDevices = Array.isArray(snapshot.devices) ? snapshot.devices : [];
  rawDevices.forEach((rawDevice) => {
    if (!isPositionRow(rawDevice)) return;
    const row = rawDevice as SearchPartyDeviceRow;
    const device = positionToTelemetryDevice(row);
    if (typeof row.display_name === "string" && row.display_name.trim()) device.name = row.display_name.trim();
    devices.push(device);
    avatars[device.id] = appearanceAvatar(device.id, row, token, avatarBaseUrl);
  });

  const rawHubs = Array.isArray(snapshot.hubs) ? snapshot.hubs : [];
  rawHubs.forEach((rawHub) => {
    const parsed = parseHub(rawHub, token, avatarBaseUrl);
    if (!parsed) return;
    devices.push(parsed.device);
    avatars[parsed.device.id] = parsed.avatar;
  });

  return {
    valid: true,
    reason: null,
    householdId: typeof snapshot.householdId === "string" ? snapshot.householdId : null,
    familyName: typeof snapshot.familyName === "string" ? snapshot.familyName : "Bluepaws Family",
    expiresAt: typeof snapshot.expiresAt === "string" ? snapshot.expiresAt : null,
    devices,
    avatars,
    trailHistory: parseTrailHistory(snapshot.trails),
    error: null,
  };
}

export function searchPartyAvatarUrl(token: string | undefined, entity: "collar" | "hub", id: number, supabaseUrl?: string) {
  if (!token || !isSearchPartyToken(token) || !supabaseUrl || !Number.isInteger(id) || id < 1 || id > 65535) return undefined;
  const base = supabaseUrl.replace(/\/+$/, "");
  return `${base}/functions/v1/search-party-avatar?token=${encodeURIComponent(token.toLowerCase())}&entity=${entity}&id=${id}`;
}

function invalidSnapshot(reason: SearchPartySnapshot["reason"]): SearchPartySnapshot {
  return { valid: false, reason, householdId: null, familyName: null, expiresAt: null, devices: [], avatars: {}, trailHistory: {}, error: null };
}

function appearanceAvatar(deviceId: number, row: SearchPartyDeviceRow, token?: string, supabaseUrl?: string): DeviceAvatar {
  const fallback = defaultDeviceAvatar(deviceId);
  const emoji = typeof row.emoji_value === "string" && row.emoji_value.trim() ? row.emoji_value : fallback.emoji;
  const color = typeof row.marker_colour === "string" ? normalizeMarkerColor(row.marker_colour) : fallback.color;
  return { kind: row.avatar_kind === "photo" ? "photo" : "emoji", emoji, color,
    photoUrl: row.avatar_kind === "photo" ? searchPartyAvatarUrl(token, "collar", deviceId, supabaseUrl) : undefined };
}

function parseHub(value: unknown, token?: string, supabaseUrl?: string): { device: TelemetryDevice; avatar: DeviceAvatar } | null {
  if (!value || typeof value !== "object") return null;
  const hub = value as SearchPartyHubRow;
  if (!Number.isInteger(hub.gateway_guid16) || (hub.gateway_guid16 as number) < 1 || (hub.gateway_guid16 as number) > 65535) return null;
  if (hub.mode !== "home" && hub.mode !== "portable" && hub.mode !== "off_grid") return null;
  if (typeof hub.received_at !== "string" || !Number.isFinite(Date.parse(hub.received_at))) return null;
  const latitude = typeof hub.latitude === "number" ? hub.latitude : null;
  const longitude = typeof hub.longitude === "number" ? hub.longitude : null;
  if ((latitude === null) !== (longitude === null)) return null;
  const gatewayId = hub.gateway_guid16 as number;
  const homeEmoji = typeof hub.home_emoji === "string" && hub.home_emoji.trim() ? hub.home_emoji : "🏡";
  const portableEmoji = typeof hub.portable_emoji === "string" && hub.portable_emoji.trim() ? hub.portable_emoji : "📱";
  const emoji = hub.mode === "home" ? homeEmoji : portableEmoji;
  const color = typeof hub.marker_colour === "string" ? normalizeMarkerColor(hub.marker_colour) : "#38bdf8";
  const photoUrl = hub.avatar_kind === "photo" ? searchPartyAvatarUrl(token, "hub", gatewayId, supabaseUrl) : undefined;
  const lastUpdate = Date.parse(hub.received_at);
  return {
    device: {
      id: -gatewayId, name: typeof hub.display_name === "string" && hub.display_name.trim() ? hub.display_name.trim() : "Home Hub",
      entity: "hub", hubMode: hub.mode, lat: latitude ?? 0, lon: longitude ?? 0, hasGps: latitude !== null,
      lastUpdate, seq: 0, time: Math.floor(lastUpdate / 1000), status: hub.mode === "home" ? "Home" : "Out",
      profile: "Normal", error: "None", batt: 0, rssi: null, snr: null, bleHome: false, ingestPath: null,
      source: typeof hub.fix_at === "string" && Number.isFinite(Date.parse(hub.fix_at)) ? `Hub GNSS · ${new Date(hub.fix_at).toLocaleString()}` : "No hub GPS fix yet",
    },
    avatar: { kind: hub.avatar_kind === "photo" ? "photo" : "emoji", emoji, color, photoUrl },
  };
}

function parseTrailHistory(value: unknown): Record<number, TrailPoint[]> {
  if (!value || typeof value !== "object" || Array.isArray(value)) return {};
  const history: Record<number, TrailPoint[]> = {};
  Object.entries(value as Record<string, unknown>).forEach(([deviceIdText, rawPoints]) => {
    const deviceId = Number(deviceIdText);
    if (!Number.isInteger(deviceId) || deviceId < 1 || deviceId > 65535 || !Array.isArray(rawPoints)) return;
    const points = rawPoints.flatMap((rawPoint): TrailPoint[] => {
      if (!rawPoint || typeof rawPoint !== "object") return [];
      const point = rawPoint as Record<string, unknown>;
      if (typeof point.lat !== "number" || typeof point.lon !== "number" || typeof point.recordedAt !== "string" || !Number.isFinite(Date.parse(point.recordedAt))) return [];
      if (point.lat < -90 || point.lat > 90 || point.lon < -180 || point.lon > 180) return [];
      return [{ lat: point.lat, lon: point.lon, recordedAt: point.recordedAt }];
    }).slice(-4);
    if (points.length > 0) history[deviceId] = points;
  });
  return history;
}
