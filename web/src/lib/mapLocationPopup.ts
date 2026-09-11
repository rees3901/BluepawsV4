import { formatMapCoordinates, googleMapsUrl } from "@/lib/mapLocation";

export interface MapLocationPoint {
  lat: number;
  lng: number;
}

export function contextMenuHtml(point: MapLocationPoint) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu"><div class="map-context-heading">Map location</div>${coordinateActionRow(point)}<div class="map-context-actions"><button type="button" data-location-action="drop-pin" ${locationData}>📍 Drop temporary pin</button><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button></div><p class="map-context-hint">Coordinates were copied when this menu opened.</p></div>`;
}

export function temporaryPinPopupHtml(point: MapLocationPoint, pinId: number) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu temporary-pin-card"><div class="map-context-heading">Temporary meeting point</div>${coordinateActionRow(point)}<div class="map-context-actions"><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button><button type="button" class="danger" data-location-action="remove-pin" data-pin-id="${pinId}">× Remove pin</button></div><p class="map-context-hint">This pin stays only for this browser session.</p></div>`;
}

export async function copyTextToClipboard(value: string) {
  // Keep the synchronous path inside the user's gesture. Some browsers reject
  // the async Clipboard API while still allowing the legacy copy action.
  const textarea = document.createElement("textarea");
  textarea.value = value;
  textarea.style.position = "fixed";
  textarea.style.opacity = "0";
  document.body.append(textarea);
  textarea.select();
  const copiedSynchronously = document.execCommand("copy");
  textarea.remove();
  if (copiedSynchronously) return true;

  try {
    await navigator.clipboard.writeText(value);
    return true;
  } catch {
    return false;
  }
}

function coordinateActionRow(point: MapLocationPoint) {
  const mapsUrl = googleMapsUrl(point.lat, point.lng);
  return `<div class="map-context-coordinate-row"><a class="map-context-coordinates" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${formatMapCoordinates(point.lat, point.lng, 6)}</a><a class="map-context-icon-action" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open in Google Maps" aria-label="Open this location in Google Maps in a new tab">${openInNewTabIcon()}</a></div>`;
}

function openInNewTabIcon() {
  return '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3h7v7"/><path d="M10 14 21 3"/><path d="M21 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5"/></svg>';
}

function locationDataAttributes(point: MapLocationPoint) {
  return `data-latitude="${point.lat.toFixed(6)}" data-longitude="${point.lng.toFixed(6)}"`;
}
