import type { HubPresence } from "./hubPresence";
import { HUB_REPORTING, hubContactGrace, type HubReportingProfile } from "./hubReporting.ts";

// Delay is informational first; a missed confirmation does not undo a durable request.
export const HUB_CONTROL_CONFIRM_MS = 30_000;
export type HubControlAttempt = { revision: number; startedAt: number } &
  ({ enabled: boolean; profile?: never } | { profile: HubReportingProfile; enabled?: never });

export function hubControlFeedback(hub: HubPresence, attempt: HubControlAttempt | null, now: number) {
  if (!attempt) return null;
  const label = attempt.profile ? `Reporting profile: ${HUB_REPORTING[attempt.profile].label}`
    : `Bluetooth ${attempt.enabled ? "enabled" : "disabled"}`;
  const matches = attempt.profile ? hub.reporting_profile === attempt.profile : hub.ble_enabled === attempt.enabled;
  const superseded = attempt.profile ? hub.desired_reporting_profile !== attempt.profile : hub.desired_ble_enabled !== attempt.enabled;
  if (hub.applied_revision >= attempt.revision && matches) {
    return { state: "confirmed", text: `${label} — confirmed by hub.` };
  }
  if (hub.settings_revision > attempt.revision && superseded) {
    return { state: "failed", text: "A newer hub setting replaced this request." };
  }
  const waitBudget = Math.max(90, hubContactGrace(hub.reporting_profile)) * 1000;
  if (now - attempt.startedAt >= waitBudget) {
    return { state: "failed", text: "Hub has not confirmed the change. Check its connection; the saved setting will retry when it reconnects." };
  }
  if (now - attempt.startedAt >= HUB_CONTROL_CONFIRM_MS) {
    return { state: "pending", text: `Still waiting for hub confirmation — ${label.toLowerCase()}. The request remains saved.` };
  }
  return { state: "pending", text: `${label}… waiting for hub confirmation.` };
}
