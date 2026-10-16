import type { TelemetryDevice } from "../types/telemetry.ts";
import { GPS_REPORT_GRACE_SECONDS, gpsReportingSchedule, isFullGpsReport } from "./gpsReporting.ts";

export const GPS_AGING_AFTER_SECONDS = 60;
export const GPS_STALE_AFTER_SECONDS = 5 * 60;
// Absolute location-age backstops, not missed/failed telemetry counters.
export const GPS_AMBER_AGE_CAP_SECONDS = 60 * 60;
export const GPS_RED_AGE_CAP_SECONDS = 4 * 60 * 60;
export const HOME_BEACON_NOTICE = "The collar recently detected the Home Hub beacon. This confirms Home presence, not a fresh GPS position.";
export type GpsFreshness = ReturnType<typeof gpsFreshness>;

// Contact and beacon evidence affect the EXPECTED full-telemetry schedule,
// never the GPS fix timestamp. 65535 means unknown fix age.
export function gpsFreshness(device: Pick<TelemetryDevice, "hasGps" | "gnss" | "positionRecordedAt" | "entity" | "faultReport" | "profile" | "status" | "bleHome" | "homeBeaconSeenAt" | "lastReportReason" | "gpsFailedReports">, now: number, latestFlags?: number | null) {
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
  const flags = latestFlags ?? device.faultReport?.flags ?? 0;
  const reason = device.lastReportReason ?? device.faultReport?.txReason;
  const reportedStale = (flags & 0x40) !== 0 && (reason == null || isFullGpsReport(reason));
  const contactWindow = gpsReportingSchedule(device.profile).contactWindowSeconds;
  const beaconAge = (now - (device.homeBeaconSeenAt ?? NaN)) / 1000;
  const homeConfirmed = device.entity !== "hub" && device.profile !== "Emergency Lost"
    && device.status === "Home" && device.bleHome && Number.isFinite(beaconAge)
    && beaconAge >= 0 && beaconAge <= contactWindow;
  const { fullTelemetrySeconds } = gpsReportingSchedule(device.profile, homeConfirmed);
  // Allow a GNSS acquisition/delivery grace. Home wake-check-ins do not
  // consume a full-telemetry opportunity (normally only every tenth wake).
  const missedScheduled = ageSeconds === null ? 0
    : Math.max(0, Math.ceil(ageSeconds / (fullTelemetrySeconds + GPS_REPORT_GRACE_SECONDS)) - 1);
  const failedReports = Number.isInteger(device.gpsFailedReports) && device.gpsFailedReports! > 0
    ? Math.min(3, device.gpsFailedReports!) : 0;
  const missedFullTelemetry = Math.max(missedScheduled, failedReports, reportedStale ? 1 : 0);
  const state = !device.hasGps ? "missing" : ageSeconds === null ? "unknown"
    : device.entity === "hub" ? ageSeconds >= GPS_STALE_AFTER_SECONDS ? "stale"
      : ageSeconds >= GPS_AGING_AFTER_SECONDS || reportedStale ? "aging" : "fresh"
    : missedFullTelemetry >= 3 || ageSeconds >= GPS_RED_AGE_CAP_SECONDS ? "stale"
    : missedFullTelemetry >= 1 || ageSeconds >= GPS_AMBER_AGE_CAP_SECONDS ? "aging" : "fresh";
  // Green means on schedule, not a live location. Older coordinates remain
  // explicitly last-known, including while Home presence is confirmed.
  const lastKnown = ageSeconds === null || ageSeconds >= GPS_AGING_AFTER_SECONDS || reportedStale || failedReports > 0;
  const age = ageSeconds === null ? null : formatGpsAge(ageSeconds);
  const label = state === "missing" ? "No GPS fix" : state === "unknown" ? "GPS time unknown"
    : lastKnown ? `GPS ${age} old` : `GPS ${age} ago`;
  const warning = state !== "fresh";
  const locationNotice = state === "missing" ? "No GPS location available."
    : state === "unknown" ? "Last known location — fix time unavailable; not a confirmed current position."
    : lastKnown ? "Last known location — not a current position."
    : "Recent GPS fix — position may have changed since it was measured.";
  const scheduleNotice = device.entity === "hub" ? ""
    : ` Full GPS telemetry expected every ${formatGpsAge(fullTelemetrySeconds)}, plus up to ${GPS_REPORT_GRACE_SECONDS}s acquisition/delivery grace. Wake check-ins and ACKs are not GPS updates. GPS age alone turns amber at 1h and red at 4h; profile timing or failed full reports may warn sooner.`;
  const notice = (homeConfirmed ? "Home confirmed by a recent collar beacon report; GPS refreshes less often at home. " : "") + locationNotice + scheduleNotice;
  return { state, fixAt, ageSeconds, label, warning, lastKnown, homeConfirmed, missedFullTelemetry, fullTelemetrySeconds, notice, className: `gps-${state}` };
}

export function formatGpsAge(seconds: number) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`;
  return `${Math.floor(seconds / 86400)}d ${Math.floor(seconds % 86400 / 3600)}h`;
}

/** Compact visible copy; the full GPS label and notice remain in tooltips. */
export function gpsAgeLabel(gps: GpsFreshness) {
  const age = gps.state === "missing" ? "GPS age: no fix" : gps.ageSeconds === null ? "GPS age: —" : `GPS age: ${formatGpsAge(gps.ageSeconds)}`;
  return age;
}

export function gpsAsAt(gps: GpsFreshness, now: number) {
  if (gps.fixAt === null) return "Unknown";
  const date = new Date(gps.fixAt);
  const sameDay = date.toDateString() === new Date(now).toDateString();
  const timestamp = sameDay ? date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })
    : date.toLocaleString([], { year: "numeric", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  return `${timestamp} · ${formatGpsAge(gps.ageSeconds!)} ago`;
}
