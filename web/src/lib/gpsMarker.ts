import { gpsFreshness } from "./gpsFreshness";
import type { TelemetryDevice } from "@/types/telemetry";

// Shared DOM adornment; never changes the pin size, transform or coordinate anchor.
export function updateGpsMarker(element: HTMLElement, device: TelemetryDevice, now: number) {
  element.querySelector(".marker-gps-warning")?.remove();
  const gps = gpsFreshness(device, now);
  const description = device.entity === "hub" ? device.name : `${device.name}. ${gps.label}. ${gps.notice}`;
  element.title = description;
  element.setAttribute("aria-label", `Open ${description}`);
  if (device.entity === "hub" || !gps.warning) return;
  const badge = document.createElement("span");
  badge.className = `marker-gps-warning ${gps.className}`;
  badge.textContent = "🛰";
  badge.setAttribute("aria-hidden", "true");
  element.append(badge);
}
