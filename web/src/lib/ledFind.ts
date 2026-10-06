export type LedFindAction = "flash" | "repeat" | "stop";
export type LedFindPayload = { action: "flash" | "stop" } | { action: "repeat"; duration_s: number; interval_s: number };
export const LED_INTERVALS = [10, 30, 60, 120, 300, 600] as const;
// Use reported firmware profile, never an unacknowledged profile request.
export function ledIntervalsForProfile(profile?: string) {
  return LED_INTERVALS.filter(seconds => seconds >= 60 || profile === "Emergency Lost");
}
export const LED_DURATIONS = [10, 30, 60, 300, 600, 900, 1800, 3600, 7200, 14400] as const;
export function ledTimeLabel(seconds: number) {
  const value = seconds < 60 ? seconds : seconds < 3600 ? seconds / 60 : seconds / 3600;
  const unit = seconds < 60 ? "second" : seconds < 3600 ? "minute" : "hour";
  return `${value} ${unit}${value === 1 ? "" : "s"}`;
}
export function ledFindPayload(action: LedFindAction, seconds = 600, interval = 60): LedFindPayload {
  if (action === "flash" || action === "stop") return { action };
  if (action !== "repeat" || !Number.isInteger(seconds) || seconds < 10 || seconds > 14400 ||
      !Number.isInteger(interval) || interval < 10 || interval > 600) {
    throw new Error("Choose a duration from 10 seconds to 4 hours and an interval from 10 seconds to 10 minutes");
  }
  return { action, duration_s: seconds, interval_s: interval };
}

// Initial fitted batch; replace with firmware capability discovery when proper collars support it.
export function supportsLedFind(deviceId: number) { return deviceId >= 3001 && deviceId <= 3004 && Number.isInteger(deviceId); }
