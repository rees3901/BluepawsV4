import { gpsFreshness } from "./gpsFreshness";
import type { TelemetryDevice } from "../types/telemetry";

export type HomeAnchor = { lat: number; lon: number; confirmedAt: number; hubId: number };
export type MapPosition = HomeAnchor & { source: "home" | "home-stale" };
export const HOME_ANCHORS_KEY = "bluepaws-home-map-anchors-v1";

// Session-only recovery: refreshing the page must not resurrect pre-Home GPS.
// Never trust cached values without validating them and matching the hub below.
export function parseHomeAnchors(json: string | null, now: number): Record<number, HomeAnchor> {
  const anchors: Record<number, HomeAnchor> = {};
  try {
    const entries = JSON.parse(json ?? "{}");
    for (const [key, anchor] of Object.entries(entries).slice(-128)) {
      const value = anchor as HomeAnchor;
      if (!Number.isSafeInteger(Number(key)) || !value || !Number.isFinite(value.lat) || !Number.isFinite(value.lon)
        || Math.abs(value.lat) > 90 || Math.abs(value.lon) > 180 || !Number.isSafeInteger(value.hubId)
        || !Number.isFinite(value.confirmedAt) || value.confirmedAt <= 0 || value.confirmedAt > now) continue;
      anchors[Number(key)] = { lat: value.lat, lon: value.lon, hubId: value.hubId, confirmedAt: value.confirmedAt };
    }
  } catch { /* Missing or malformed storage cannot invent a position. */ }
  return anchors;
}

// Presentation only. Never mutate GPS coordinates, fix timestamps or trails.
export function resolveMapPosition(device: TelemetryDevice, now: number, previous?: HomeAnchor): MapPosition | undefined {
  if (device.entity === "hub") return;
  const gps = gpsFreshness(device, now);
  const hub = device.homeHub;
  const validHub = hub && typeof hub.lat === "number" && typeof hub.lon === "number"
    && Number.isFinite(hub.lat) && Number.isFinite(hub.lon)
    && Math.abs(hub.lat) <= 90 && Math.abs(hub.lon) <= 180
    && !!hub.fixAt && Number.isFinite(Date.parse(hub.fixAt));
  if (gps.homeConfirmed && validHub) {
    return { lat: hub.lat!, lon: hub.lon!, hubId: hub.id,
      confirmedAt: device.homeBeaconSeenAt!, source: "home" };
  }
  // A newer measured fix, not a check-in or retransmission, releases the hold.
  if (!previous || previous.hubId !== hub?.id) return;
  if (gps.fixAt !== null && gps.fixAt > previous.confirmedAt) return;
  return { ...previous, source: "home-stale" };
}

export function mapPosition(device: TelemetryDevice) {
  const anchor = device.mapPosition;
  if (!anchor) return device;
  // Small, stable visual separation, never written back as GPS or trail data.
  const angle = (device.id * 137.508 % 360) * Math.PI / 180;
  return { lat: anchor.lat + 6 * Math.cos(angle) / 111320,
    lon: anchor.lon + 6 * Math.sin(angle) / (111320 * Math.max(.01, Math.cos(anchor.lat * Math.PI / 180))) };
}

export function mapPositionNotice(device: TelemetryDevice) {
  return device.mapPosition?.source === "home"
    ? "Home beacon confirmed — shown beside the Home Hub, not a GPS fix."
    : device.mapPosition?.source === "home-stale"
      ? "Current position unknown — held beside the last home-confirmed location while awaiting a new GPS fix."
      : "";
}
