import type { HubPresence } from "./hubPresence";

// A missed minute report plus 30 seconds of grace, NOT proof that Wi-Fi is off.
export const HUB_CONTACT_GRACE_SECONDS = 90;
export const HUB_CONTROL_CONFIRM_MS = 30_000;
export type HubControlAttempt = { enabled: boolean; revision: number; startedAt: number };

export function hubControlFeedback(hub: HubPresence, attempt: HubControlAttempt | null, now: number) {
  if (!attempt) return null;
  if (hub.applied_revision >= attempt.revision && hub.ble_enabled === attempt.enabled) {
    return { state: "confirmed", text: `Bluetooth ${attempt.enabled ? "enabled" : "disabled"} — confirmed by hub.` };
  }
  if (hub.settings_revision > attempt.revision && hub.desired_ble_enabled !== attempt.enabled) {
    return { state: "failed", text: "A newer Bluetooth setting replaced this request." };
  }
  if (now - attempt.startedAt >= HUB_CONTROL_CONFIRM_MS) {
    return { state: "failed", text: "Hub has not confirmed the change. Check its connection; the saved setting will retry when it reconnects." };
  }
  return { state: "pending", text: `Updating Bluetooth ${attempt.enabled ? "on" : "off"}… waiting for hub confirmation.` };
}
