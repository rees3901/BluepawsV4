export type LedFindAction = "flash" | "repeat" | "stop";
export type LedFindPayload = { action: "flash" | "stop" } | { action: "repeat"; duration_s: number; interval_s: 60 };
export function ledFindPayload(action: LedFindAction, minutes = 10): LedFindPayload {
  if (action === "flash" || action === "stop") return { action };
  if (action !== "repeat" || !Number.isInteger(minutes) || minutes < 1 || minutes > 60) {
    throw new Error("Choose an LED duration between 1 and 60 minutes");
  }
  return { action, duration_s: minutes * 60, interval_s: 60 };
}

// Initial fitted batch; replace with firmware capability discovery when proper collars support it.
export function supportsLedFind(deviceId: number) { return deviceId >= 3001 && deviceId <= 3004 && Number.isInteger(deviceId); }
