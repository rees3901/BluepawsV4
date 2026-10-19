import { gpsFreshness, isRecentGps } from "./gpsFreshness";
import { mapPositionNotice } from "./mapPosition";
import type { TelemetryDevice } from "@/types/telemetry";

// Shared DOM adornment; never changes the pin size, transform or coordinate anchor.
export function updateGpsMarker(element: HTMLElement, device: TelemetryDevice, now: number) {
  element.querySelector(".marker-gps-warning")?.remove();
  const gps = gpsFreshness(device, now);
  const description = device.entity === "hub" ? device.name : `${device.name}. ${mapPositionNotice(device)} ${gps.label}. ${gps.notice}`;
  element.title = description;
  element.setAttribute("aria-label", `Open ${description}`);
  // Map clutter has its own age threshold; retain the richer card/popup rules.
  // Missing timestamps cannot establish that a last-known position is recent.
  const home = device.mapPosition?.source === "home";
  const held = device.mapPosition?.source === "home-stale";
  if (device.entity === "hub" || (!home && !held && (!gps.lastKnown || isRecentGps(gps)))) return;
  const badge = document.createElement("span");
  badge.className = `marker-gps-warning ${home ? "marker-home-confirmed" : held ? "gps-stale" : gps.className}`;
  badge.textContent = home ? "🏠" : "🛰";
  badge.setAttribute("aria-hidden", "true");
  element.append(badge);
}
