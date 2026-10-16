import type { TelemetryDevice } from "../types/telemetry.ts";

export const GPS_AGING_AFTER_SECONDS = 60;
export const GPS_STALE_AFTER_SECONDS = 5 * 60;
export type GpsFreshness = ReturnType<typeof gpsFreshness>;

// Contact/presence is deliberately NOT an input. Only the observation backing
// the displayed coordinates can date that location. 65535 means unknown age.
export function gpsFreshness(device: Pick<TelemetryDevice, "hasGps" | "gnss" | "positionRecordedAt" | "entity" | "faultReport">, now: number, latestFlags?: number | null) {
  let fixAt: number | null = null;
  const fix = device.gnss;
  if (device.hasGps && fix && Number.isInteger(fix.fixAgeS) && fix.fixAgeS >= 0 && fix.fixAgeS < 65535) {
    const packetAt = Date.parse(fix.recordedAt);
    // positionRecordedAt is capped at receipt time for collars with fast clocks.
    const cappedAt = Date.parse(device.positionRecordedAt ?? "");
    const anchor = device.entity !== "hub" && Number.isFinite(cappedAt) ? Math.min(packetAt, cappedAt) : packetAt;
    const measuredAt = anchor - fix.fixAgeS * 1000;
    if (Number.isFinite(measuredAt) && measuredAt > 0 && measuredAt <= now) fixAt = measuredAt;
  } else if (device.entity === "hub" && device.hasGps) {
    const measuredAt = Date.parse(device.positionRecordedAt ?? "");
    if (Number.isFinite(measuredAt) && measuredAt > 0 && measuredAt <= now) fixAt = measuredAt;
  }
  const ageSeconds = fixAt === null ? null : Math.floor((now - fixAt) / 1000);
  const reportedStale = ((latestFlags ?? device.faultReport?.flags ?? 0) & 0x40) !== 0;
  const state = !device.hasGps ? "missing" : ageSeconds === null ? "unknown"
    : ageSeconds >= GPS_STALE_AFTER_SECONDS ? "stale"
    : ageSeconds >= GPS_AGING_AFTER_SECONDS || reportedStale ? "aging" : "fresh";
  const age = ageSeconds === null ? null : formatGpsAge(ageSeconds);
  const label = state === "missing" ? "No GPS fix" : state === "unknown" ? "GPS time unknown"
    : state === "fresh" ? `GPS ${age} ago` : `GPS ${age} old`;
  const warning = state !== "fresh";
  const notice = state === "missing" ? "No GPS location available."
    : state === "unknown" ? "Last known location — fix time unavailable; not a confirmed current position."
    : warning ? "Last known location — not a current position."
    : "Recent GPS fix — position may have changed since it was measured.";
  return { state, fixAt, ageSeconds, label, warning, notice, className: `gps-${state}` };
}

export function formatGpsAge(seconds: number) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`;
  return `${Math.floor(seconds / 86400)}d ${Math.floor(seconds % 86400 / 3600)}h`;
}

/** Compact visible copy; the full GPS label and notice remain in tooltips. */
export function gpsAgeLabel(gps: GpsFreshness) {
  if (gps.state === "missing") return "no fix";
  return gps.ageSeconds === null ? "age —" : `age ${formatGpsAge(gps.ageSeconds)}`;
}

export function gpsAsAt(gps: GpsFreshness, now: number) {
  if (gps.fixAt === null) return "Unknown";
  const date = new Date(gps.fixAt);
  const sameDay = date.toDateString() === new Date(now).toDateString();
  const timestamp = sameDay ? date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })
    : date.toLocaleString([], { year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  return `${timestamp} · ${formatGpsAge(gps.ageSeconds!)} ago`;
}
