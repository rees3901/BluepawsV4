import type { TelemetryDevice } from "@/types/telemetry";

// POWER_SAVE collars may intentionally use their scheduled LTE heartbeat only
// every three hours. The extra hour avoids declaring a healthy, conserving
// collar offline because a single report was delayed.
export const COLLAR_RECEIVE_WINDOW_SECONDS = 10;
export const COLLAR_STALE_AFTER_SECONDS = 10 * 60;
export const COLLAR_OFFLINE_AFTER_SECONDS = 4 * 60 * 60;

export type CollarCardFreshness = "active" | "sleeping" | "stale" | "offline";

export function collarCardFreshness(ageSeconds: number, receiveWindowOpen: boolean): CollarCardFreshness {
  const age = Math.max(0, ageSeconds);
  if (age >= COLLAR_OFFLINE_AFTER_SECONDS) return "offline";
  if (age >= COLLAR_STALE_AFTER_SECONDS) return "stale";
  return receiveWindowOpen ? "active" : "sleeping";
}

export function isCollarOfflineAge(ageSeconds: number) {
  return ageSeconds >= COLLAR_OFFLINE_AFTER_SECONDS;
}

export function isCollarOffline(device: TelemetryDevice, nowMs: number) {
  if (device.entity === "hub") return false;
  return isCollarOfflineAge(Math.max(0, Math.floor((nowMs - device.lastUpdate) / 1000)));
}
