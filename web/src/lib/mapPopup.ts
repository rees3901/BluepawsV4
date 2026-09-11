import { batteryPresentation, signalQuality } from "@/components/Indicators";
import { collarFault } from "@/lib/collarFault";
import { emojiImageUrl } from "@/lib/emoji";
import { COLLAR_RECEIVE_WINDOW_SECONDS, collarCardFreshness, collarFreshnessClass } from "@/lib/devicePresence";
import { formatHomeDistance, formatMapCoordinates, googleMapsUrl, homeDistanceMetres } from "@/lib/mapLocation";
import { normalizeMarkerColor } from "@/lib/markerColor";
import { transportPresentation } from "@/lib/transportPath";
import type { DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

export function mapPopupHtml(device: TelemetryDevice, avatar: DeviceAvatar, presenceNow: number, readOnly = false, followed = false, trailVisible = false) {
  const name = escapeHtml(device.name);
  const isHub = device.entity === "hub";
  const coordinates = formatMapCoordinates(device.lat, device.lon);
  const mapsUrl = googleMapsUrl(device.lat, device.lon);
  const status = popupStatus(device);
  const profile = popupProfile(device);
  const ageSeconds = Math.max(0, Math.floor((presenceNow - device.lastUpdate) / 1000));
  const estimatedAwake = !isHub && ageSeconds < COLLAR_RECEIVE_WINDOW_SECONDS;
  const freshness = isHub ? null : collarCardFreshness(ageSeconds, estimatedAwake);
  const offline = freshness === "offline";
  const freshnessClass = collarFreshnessClass(freshness);
  const source = device.source ? `<span class="label">${offline ? "Last reported source" : "Source"}</span><span class="value">${escapeHtml(device.source)}</span>` : "";
  const distance = formatHomeDistance(homeDistanceMetres(device));
  const fault = isHub || offline ? null : collarFault(device.faultReport, device.error !== "None");
  const faultHtml = fault ? `<div class="card-fault-row"><span class="error-badge" title="${escapeHtml(fault.title)}">${escapeHtml(fault.label)}</span></div>` : "";
  const actions = `<div class="card-actions popup-actions"><button class="btn-action btn-jump" data-map-action="jump" data-device-id="${device.id}">↗ Jump To</button><button class="btn-action btn-follow${followed ? " active" : ""}" data-map-action="follow" data-device-id="${device.id}">● ${followed ? "Following" : "Follow"}</button><button class="btn-action btn-trail${trailVisible ? " active" : ""}" data-map-action="trail" data-device-id="${device.id}">⌁ Trail</button>${readOnly || isHub ? "" : `<button class="btn-action btn-find" data-map-action="find" data-device-id="${device.id}">♟ Find Alert</button><button class="btn-action btn-cmd" data-map-action="command" data-device-id="${device.id}">⌘ Cmd</button>`}</div>`;
  const details = isHub
    ? `<span class="label">Hub ID</span><span class="value">${Math.abs(device.id)}</span>`
    : `<span class="label">Device ID</span><span class="value">${device.id}</span><span class="label">${offline ? "Last reported profile" : "Power Profile"}</span><span class="value">${escapeHtml(device.profile)}</span><span class="label">${offline ? "Last known distance" : "Dist From Hub"}</span><span class="value">${distance}</span>`;
  const summary = offline
    ? `<div class="card-name-row"><span class="card-name">${name}</span><span class="card-status status-offline">Offline</span></div><div class="card-offline-summary">No reports for ${formatLastSeen(ageSeconds)}</div>`
    : `<div class="card-name-row"><span class="card-name">${name}</span><span class="card-status ${status.css}">${status.emoji} ${status.label}</span><span class="card-profile ${profile.css}">${profile.label}</span></div>${faultHtml}<div class="card-indicators card-indicators-primary"><span class="card-indicator-group">${batteryIndicatorHtml(isHub ? null : device.batt, device.batteryPercent)}</span><span class="card-indicator-group">${signalIndicatorHtml(device, isHub)}</span>${isHub ? "" : `<span class="collar-awake ${estimatedAwake ? "awake" : "sleeping"}" title="${estimatedAwake ? "Fresh report — command receive window may still be open" : "Receive window ended — collar probably sleeping"}">${estimatedAwake ? "💡" : "💤"}</span>`}</div><div class="card-indicators card-indicators-row3">${isHub ? "" : homeDistanceHtml(distance)}${lastSeenHtml(formatLastSeen(ageSeconds))}</div>`;
  const offlineNotice = offline ? `<p class="card-offline-notice"><strong>Offline.</strong> These are last-known details from ${formatAge(ageSeconds)} and may no longer be current.</p>` : "";
  return `<div class="popup-content device-card map-device-card${freshnessClass ? ` ${freshnessClass}` : ""} expanded"><div class="card-summary map-popup-summary">${popupAvatarHtml(avatar)}<div class="card-identity">${summary}</div></div><div class="card-detail map-popup-detail">${offlineNotice}<div class="card-grid"><span class="label">${offline ? "Last known coordinates" : "Coordinates"}</span><span class="value"><a class="card-coords card-coords-link" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${coordinates}</a></span>${details}<span class="label">Last report</span><span class="value">${formatAge(ageSeconds)}</span>${source}</div>${actions}</div></div>`;
}

function popupStatus(device: TelemetryDevice) {
  if (device.entity === "hub") {
    return device.hubMode === "home"
      ? { emoji: "🏡", label: "Home", css: "status-home" }
      : { emoji: "📱", label: device.hubMode === "portable" ? "Portable" : "Off-Grid", css: "status-out" };
  }
  return {
    Home: { emoji: "🏠", label: "Home", css: "status-home" },
    Out: { emoji: "🐾", label: "Out", css: "status-out" },
    Lost: { emoji: "‼", label: "Lost", css: "status-lost" },
    Error: { emoji: "❓", label: "Error", css: "status-error" },
  }[device.status];
}

function popupProfile(device: TelemetryDevice) {
  const profileName = device.entity === "hub"
    ? device.hubReportingProfile === "power_save" ? "Power Save" : device.hubReportingProfile === "active" ? "Active" : "Normal"
    : device.profile;
  const label = device.entity !== "hub" && device.profile === "PowerSave" ? "💤 PowerSave" : device.entity !== "hub" && device.profile === "Debug" ? "🧪 Debug" : profileName;
  const normalized = profileName.toLowerCase().replace("save", "").replaceAll(" ", "-");
  return { label: escapeHtml(label), css: `profile-${normalized}` };
}

function popupAvatarHtml(avatar: DeviceAvatar) {
  const markerColor = normalizeMarkerColor(avatar.color);
  return `<div class="card-avatar popup-avatar" style="border-color:${markerColor}">${avatarHtml(avatar, "popup-avatar-image")}</div>`;
}

function avatarHtml(avatar: DeviceAvatar, className: string) {
  if (avatar.kind === "photo" && avatar.photoUrl) return `<img class="${className} avatar-image" src="${escapeHtml(avatar.photoUrl)}" alt="">`;
  return `<img class="${className} avatar-emoji-image" src="${emojiImageUrl(avatar.emoji)}" alt="${escapeHtml(avatar.emoji)}">`;
}

function batteryIndicatorHtml(millivolts: number | null, percent?: number | null) {
  const battery = batteryPresentation(millivolts, percent);
  const bars = [0, 1, 2, 3, 4].map((bar) => `<rect x="${4 + bar * 3.4}" y="4.5" width="2.6" height="9" rx="0.6" fill="${bar < battery.level ? battery.color : "#2f3e4e"}"/>`).join("");
  return `<span class="battery-indicator" title="${battery.measurement} — ${battery.label}"><svg class="indicator-icon icon-battery" viewBox="0 0 28 18" fill="none"><rect x="1" y="1" width="23" height="16" rx="3" stroke="#607d8b" stroke-width="2"/><rect x="24" y="5.5" width="3" height="7" rx="1.2" fill="#607d8b"/>${bars}</svg><span class="sig-label" style="color:${battery.color}">${battery.label}</span></span>`;
}

function signalIndicatorHtml(device: TelemetryDevice, isHub: boolean) {
  const transport = transportPresentation(device.ingestPath);
  const quality = isHub ? wifiQuality(device.rssi) : device.rssi === null || device.snr === null ? null : signalQuality(device.rssi, device.snr);
  const level = quality?.level ?? 0;
  const color = quality?.color ?? "#607d8b";
  const label = quality?.label ?? (isHub ? "No Wi-Fi" : "Not reported");
  const bars = [1, 2, 3, 4, 5].map((bar) => `<span class="sig-bar${bar <= level ? " filled" : ""}" style="height:${4 + bar * 3}px${bar <= level ? `;background:${color}` : ""}"></span>`).join("");
  const badge = isHub ? "Wi-Fi" : transport.badge;
  const badgeClass = isHub ? "transport-wifi" : transport.cssClass;
  const title = isHub ? `Wi-Fi ${device.rssi == null ? "not connected" : `${device.rssi} dBm`} — ${label}` : `${transport.label}; ${device.rssi === null || device.snr === null ? "radio signal was not included in this report" : `RSSI: ${device.rssi} dBm / SNR: ${device.snr} dB — ${label}`}`;
  const bluetooth = isHub ? bluetoothBeaconHtml(device.bleHome) : "";
  return `<span class="signal-indicator" title="${escapeHtml(title)}">${antennaIcon()}${bars}<span class="sig-label" style="color:${color}">${label}</span><span class="transport-badge ${badgeClass}">${badge}</span></span>${bluetooth}`;
}

function bluetoothBeaconHtml(advertising: boolean) {
  const label = advertising ? "Home beacon advertising" : "Home beacon not advertising";
  return `<span class="bluetooth-beacon${advertising ? " active" : ""}" title="${label}" role="img" aria-label="${label}"><svg aria-hidden="true" viewBox="0 0 16 20"><path d="M4 5l9 10-5 4V1l5 4L4 15" fill="none" stroke="currentColor" stroke-width="2"/></svg><span class="bluetooth-beacon-state ${advertising ? "on" : "off"}" aria-hidden="true">${advertising ? "✓" : "❌"}</span></span>`;
}

function wifiQuality(rssi: number | null) {
  const level = rssi === null ? 0 : rssi >= -50 ? 5 : rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
  return { level, label: ["No Wi-Fi", "Very poor", "Poor", "Average", "Good", "Excellent"][level], color: ["#607d8b", "#ef4444", "#f97316", "#f59e0b", "#84cc16", "#22c55e"][level] };
}

function antennaIcon() {
  return '<svg class="indicator-icon icon-antenna" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="24" x2="12" y2="10"/><line x1="12" y1="10" x2="3" y2="2"/><line x1="12" y1="10" x2="21" y2="2"/><line x1="3" y1="2" x2="21" y2="2"/></svg>';
}

function homeDistanceHtml(distance: string) {
  return `<span class="card-indicator-group card-dist-group" title="Distance from home"><svg class="indicator-icon icon-home" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12l9-9 9 9"/><path d="M5 10v10a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V10"/></svg><span class="card-dist-value">${distance}</span></span>`;
}

function lastSeenHtml(value: string) {
  return `<span class="card-indicator-group card-lastseen-group" title="Last seen"><svg class="indicator-icon icon-stopwatch" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="13" r="8"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="9" y1="1" x2="15" y2="1"/><line x1="12" y1="1" x2="12" y2="5"/></svg><span class="card-lastseen-value">${value}</span></span>`;
}

function formatLastSeen(seconds: number) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
}

function formatAge(seconds: number) {
  if (seconds < 5) return "just now";
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}

function escapeHtml(value: string) {
  return value.replace(/[&<>'"]/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" })[character] ?? character);
}
