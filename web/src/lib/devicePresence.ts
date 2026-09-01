import type { TelemetryDevice } from "@/types/telemetry";

// POWER_SAVE collars may intentionally use their scheduled LTE heartbeat only
// every three hours. The extra hour avoids declaring a healthy, conserving
// collar offline because a single report was delayed.
export const COLLAR_OFFLINE_AFTER_SECONDS = 4 * 60 * 60;

export function isCollarOfflineAge(ageSeconds: number) {
  return ageSeconds >= COLLAR_OFFLINE_AFTER_SECONDS;
}

export function isCollarOffline(device: TelemetryDevice, nowMs: number) {
  if (device.entity === "hub") return false;
  return isCollarOfflineAge(Math.max(0, Math.floor((nowMs - device.lastUpdate) / 1000)));
}
