import type { PowerProfile } from "../types/telemetry.ts";

// Mirrors shared/lib/BluepawsProtocol/bp_config.h. These are full GPS
// telemetry opportunities, NOT a count of all received messages.
export const COLLAR_GPS_REPORTING: Record<PowerProfile, { wakeSeconds: number; homeRefreshEvery: number }> = {
  Normal: { wakeSeconds: 600, homeRefreshEvery: 10 },
  PowerSave: { wakeSeconds: 1800, homeRefreshEvery: 10 },
  Active: { wakeSeconds: 60, homeRefreshEvery: 10 },
  "Emergency Lost": { wakeSeconds: 30, homeRefreshEvery: 1 },
  Debug: { wakeSeconds: 30, homeRefreshEvery: 1 },
};

export const GPS_REPORT_GRACE_SECONDS = 60;
export const GPS_FULL_REPORT_REASONS = [0, 3, 4] as const; // Telemetry, Interrupt, Boot.

export function isFullGpsReport(reason: number | null | undefined) {
  return GPS_FULL_REPORT_REASONS.some(value => value === reason);
}

export function gpsReportingSchedule(profile: PowerProfile = "Normal", homeConfirmed = false) {
  const policy = COLLAR_GPS_REPORTING[profile] ?? COLLAR_GPS_REPORTING.Normal;
  return {
    contactWindowSeconds: policy.wakeSeconds + GPS_REPORT_GRACE_SECONDS,
    fullTelemetrySeconds: policy.wakeSeconds * (homeConfirmed ? policy.homeRefreshEvery : 1),
  };
}
