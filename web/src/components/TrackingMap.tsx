"use client";

import L from "leaflet";
import { useEffect, useRef } from "react";
import { emojiImageUrl } from "@/lib/emoji";
import { formatMapCoordinates, googleMapsUrl, mapLocationShareText } from "@/lib/mapLocation";
import { MAP_LAYER_DEFINITIONS, type MapLayerName } from "@/lib/mapLayers";
import { normalizeMarkerColor } from "@/lib/markerColor";
import { transportPresentation } from "@/lib/transportPath";
import { appendTrailPoint, VISIBLE_TRAIL_POINT_LIMIT, type TrailLatLng } from "@/lib/trailPoints";
import {
  type DeviceAction,
  type DeviceAvatar,
  type MapCommand,
  type TelemetryDevice,
  type TrailPoint,
} from "@/types/telemetry";

interface TrackingMapProps {
  devices: TelemetryDevice[];
  avatars: Record<number, DeviceAvatar>;
  sidebarOpen: boolean;
  followedId: number | null;
  trailIds: Set<number>;
  trailHistory: Record<number, TrailPoint[]>;
  command: MapCommand | null;
  onAction: (device: TelemetryDevice, action: DeviceAction) => void;
}

const JUMP_TO_ZOOM = 17;

export default function TrackingMap(props: TrackingMapProps) {
  const { devices, avatars, sidebarOpen, followedId, trailIds, trailHistory, command, onAction } = props;
  const mapRef = useRef<L.Map | null>(null);
  const markersRef = useRef(new Map<number, L.Marker>());
  const trailsRef = useRef(new Map<number, L.Polyline>());
  const trailPointsRef = useRef(new Map<number, TrailLatLng[]>());
  const devicesRef = useRef(devices);
  const avatarsRef = useRef(avatars);
  const trailIdsRef = useRef(trailIds);
  const actionRef = useRef(onAction);

  useEffect(() => {
    devicesRef.current = devices;
    avatarsRef.current = avatars;
    trailIdsRef.current = trailIds;
    actionRef.current = onAction;
  }, [avatars, devices, onAction, trailIds]);

  useEffect(() => {
    const markers = markersRef.current;
    const trails = trailsRef.current;
    const trailPoints = trailPointsRef.current;
    const map = L.map("map", { center: [51.505, -0.09], zoom: 13, zoomControl: false, tapHold: true });
    mapRef.current = map;

    const createTileLayer = (name: MapLayerName) => {
      const definition = MAP_LAYER_DEFINITIONS[name];
      return L.tileLayer(definition.url, {
        attribution: definition.attribution,
        maxNativeZoom: definition.maxNativeZoom,
        maxZoom: definition.maxZoom,
      });
    };
    const baseLayers = Object.fromEntries(
      (Object.keys(MAP_LAYER_DEFINITIONS) as MapLayerName[]).map((name) => [
        name,
        createTileLayer(name),
      ]),
    ) as Record<MapLayerName, L.TileLayer>;
    baseLayers.Street.addTo(map);
    L.control.layers(baseLayers, undefined, { position: "topright", collapsed: true }).addTo(map);
    L.control.zoom({ position: "bottomleft" }).addTo(map);
    L.control.scale({ position: "bottomright", imperial: true, metric: true }).addTo(map);

    const FitControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        button.type = "button";
        button.title = "Fit all markers into view";
        button.innerHTML = '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><circle cx="8" cy="8" r="5"/><path d="M8 1v3m0 8v3M1 8h3m8 0h3"/><circle cx="8" cy="8" r="1.5" fill="currentColor" stroke="none"/></svg>';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => fitMarkers(map, markersRef.current));
        return button;
      },
    });
    new FitControl().addTo(map);

    const measureLayers: L.Layer[] = [];
    const measurePoints: L.LatLng[] = [];
    const temporaryPins = new Map<number, L.Marker>();
    let nextTemporaryPinId = 1;
    let measureButton: HTMLButtonElement | null = null;
    let measuring = false;

    const clearMeasurement = () => {
      measureLayers.splice(0).forEach((layer) => map.removeLayer(layer));
      measurePoints.splice(0);
    };

    const setMeasuring = (active: boolean) => {
      measuring = active;
      measureButton?.classList.toggle("active", active);
      map.getContainer().style.cursor = active ? "crosshair" : "";
      if (!active) clearMeasurement();
    };

    const addMeasurementPoint = (point: L.LatLng) => {
      measurePoints.push(point);
      const dot = L.circleMarker(point, { radius: 4, color: "#1d9bf0", fillOpacity: 1 }).addTo(map);
      measureLayers.push(dot);
      if (measurePoints.length < 2) return;

      const line = L.polyline(measurePoints, { color: "#1d9bf0", weight: 2, dashArray: "5,5" }).addTo(map);
      measureLayers.push(line);
      const total = measurePoints.slice(1).reduce((sum, currentPoint, index) => sum + currentPoint.distanceTo(measurePoints[index]), 0);
      const label = L.marker(point, {
        interactive: false,
        icon: L.divIcon({ className: "measure-label", html: formatDistance(total), iconSize: undefined }),
      }).addTo(map);
      measureLayers.push(label);
    };

    const MeasureControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        measureButton = button;
        button.type = "button";
        button.title = "Measure distance (click points on map)";
        button.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="1" y="7" width="22" height="10" rx="1"/><path d="M5 7v5M9 7v3M13 7v5M17 7v3M21 7v5"/></svg>';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => setMeasuring(!measuring));
        return button;
      },
    });
    new MeasureControl().addTo(map);

    const CoordsControl = L.Control.extend({
      options: { position: "bottomright" },
      onAdd() {
        const coordinates = L.DomUtil.create("div", "leaflet-cursor-coords");
        coordinates.id = "cursorCoords";
        coordinates.textContent = "--";
        return coordinates;
      },
    });
    new CoordsControl().addTo(map);

    map.on("mousemove", (event) => {
      const element = document.getElementById("cursorCoords");
      if (element) element.innerHTML = `${event.latlng.lat.toFixed(6)}, ${event.latlng.lng.toFixed(6)}<br>${toDms(event.latlng.lat, "N", "S")} ${toDms(event.latlng.lng, "E", "W")}`;
    });
    map.on("click", (event) => {
      if (!measuring) return;
      addMeasurementPoint(event.latlng);
    });

    const showMapNotice = (point: L.LatLng, message: string) => {
      const notice = L.tooltip({ className: "map-action-notice", direction: "top", opacity: 1 })
        .setLatLng(point)
        .setContent(message)
        .addTo(map);
      window.setTimeout(() => {
        if (map.hasLayer(notice)) map.removeLayer(notice);
      }, 1800);
    };

    const copyLocation = async (point: L.LatLng) => {
      try {
        if (!navigator.clipboard) throw new Error("Clipboard API unavailable");
        await navigator.clipboard.writeText(mapLocationShareText(point.lat, point.lng));
        showMapNotice(point, "Location copied");
      } catch {
        showMapNotice(point, "Copy unavailable");
      }
    };

    const shareLocation = async (point: L.LatLng) => {
      if (!navigator.share) {
        await copyLocation(point);
        return;
      }
      try {
        await navigator.share({
          title: "Bluepaws map location",
          text: formatMapCoordinates(point.lat, point.lng, 6),
          url: googleMapsUrl(point.lat, point.lng),
        });
        showMapNotice(point, "Location shared");
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") return;
        await copyLocation(point);
      }
    };

    const addTemporaryPin = (point: L.LatLng) => {
      const pinId = nextTemporaryPinId++;
      const marker = L.marker(point, {
        alt: "Temporary meeting point",
        icon: temporaryPinIcon(),
        keyboard: true,
        title: "Temporary meeting point",
        zIndexOffset: 900,
      }).addTo(map);
      marker.bindPopup(temporaryPinPopupHtml(point, pinId));
      temporaryPins.set(pinId, marker);
      marker.openPopup();
    };

    map.on("contextmenu", (event) => {
      L.popup({ className: "map-context-popup", closeButton: true, maxWidth: 290 })
        .setLatLng(event.latlng)
        .setContent(contextMenuHtml(event.latlng))
        .openOn(map);
    });

    const mapContainer = map.getContainer();
    const handleMapAction = (event: MouseEvent) => {
      const eventTarget = event.target as HTMLElement;
      const deviceAction = eventTarget.closest<HTMLButtonElement>("[data-map-action]");
      if (deviceAction) {
        const device = devicesRef.current.find((item) => item.id === Number(deviceAction.dataset.deviceId));
        if (device) actionRef.current(device, deviceAction.dataset.mapAction as DeviceAction);
        return;
      }

      const locationAction = eventTarget.closest<HTMLElement>("[data-location-action]");
      if (!locationAction) return;
      event.preventDefault();

      const action = locationAction.dataset.locationAction;
      const pinId = Number(locationAction.dataset.pinId);
      if (action === "remove-pin" && Number.isInteger(pinId)) {
        const marker = temporaryPins.get(pinId);
        if (marker) map.removeLayer(marker);
        temporaryPins.delete(pinId);
        map.closePopup();
        return;
      }

      const latitude = Number(locationAction.dataset.latitude);
      const longitude = Number(locationAction.dataset.longitude);
      if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return;
      const point = L.latLng(latitude, longitude);

      if (action === "drop-pin") {
        addTemporaryPin(point);
      } else if (action === "copy") {
        map.closePopup();
        void copyLocation(point);
      } else if (action === "share") {
        map.closePopup();
        void shareLocation(point);
      } else if (action === "measure") {
        map.closePopup();
        clearMeasurement();
        setMeasuring(true);
        addMeasurementPoint(point);
        showMapNotice(point, "Choose the next measurement point");
      }
    };
    mapContainer.addEventListener("click", handleMapAction);

    return () => {
      mapContainer.removeEventListener("click", handleMapAction);
      map.remove();
      mapRef.current = null;
      markers.clear();
      trails.clear();
      trailPoints.clear();
      temporaryPins.clear();
    };
  }, []);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    window.setTimeout(() => map.invalidateSize(), 280);
  }, [sidebarOpen]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    const activeDeviceIds = new Set(devices.map((device) => device.id));
    markersRef.current.forEach((marker, deviceId) => {
      if (activeDeviceIds.has(deviceId)) return;
      map.removeLayer(marker);
      markersRef.current.delete(deviceId);
    });
    trailsRef.current.forEach((trail, deviceId) => {
      if (activeDeviceIds.has(deviceId)) return;
      if (map.hasLayer(trail)) map.removeLayer(trail);
      trailsRef.current.delete(deviceId);
      trailPointsRef.current.delete(deviceId);
    });

    devices.forEach((device) => {
      const avatar = avatars[device.id];
      const markerColor = normalizeMarkerColor(avatar.color);
      const latLng: TrailLatLng = [device.lat, device.lon];
      let marker = markersRef.current.get(device.id);
      const icon = L.divIcon({
        className: "bp-marker-icon",
        html: markerElement(avatar, markerColor, device.status),
        iconSize: [36, 48],
        iconAnchor: [18, 47],
        popupAnchor: [0, -43],
      });
      if (!marker) {
        marker = L.marker(latLng, { icon }).addTo(map);
        markersRef.current.set(device.id, marker);
      } else {
        marker.setLatLng(latLng).setIcon(icon);
      }
      marker.bindPopup(popupHtml(device, avatar));

      const points = appendTrailPoint(trailPointsRef.current.get(device.id) ?? [], latLng);
      trailPointsRef.current.set(device.id, points);
      let trail = trailsRef.current.get(device.id);
      if (!trail) {
        trail = L.polyline(points, { color: markerColor, weight: 2, opacity: 0.75, dashArray: "6,5" });
        trailsRef.current.set(device.id, trail);
      } else {
        trail.setLatLngs(points);
        trail.setStyle({ color: markerColor });
      }
      if (trailIdsRef.current.has(device.id) && !map.hasLayer(trail)) trail.addTo(map);
      if (!trailIdsRef.current.has(device.id) && map.hasLayer(trail)) map.removeLayer(trail);
    });
  }, [avatars, devices]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    trailsRef.current.forEach((trail, deviceId) => {
      if (trailIds.has(deviceId) && !map.hasLayer(trail)) trail.addTo(map);
      if (!trailIds.has(deviceId) && map.hasLayer(trail)) map.removeLayer(trail);
    });
  }, [trailIds]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map || followedId === null) return;
    const followed = devices.find((device) => device.id === followedId);
    if (followed) map.panTo([followed.lat, followed.lon]);
  }, [devices, followedId]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    Object.entries(trailHistory).forEach(([deviceIdValue, historicalPoints]) => {
      const deviceId = Number(deviceIdValue);
      const device = devicesRef.current.find((item) => item.id === deviceId);
      const avatar = avatarsRef.current[deviceId];
      if (!device || !avatar || historicalPoints.length === 0) return;

      const points: TrailLatLng[] = historicalPoints
        .slice(-VISIBLE_TRAIL_POINT_LIMIT)
        .map((point) => [point.lat, point.lon]);
      const lastPoint = historicalPoints.at(-1);
      if (!lastPoint || lastPoint.lat !== device.lat || lastPoint.lon !== device.lon) {
        points.push([device.lat, device.lon]);
      }
      const visiblePoints = points.slice(-VISIBLE_TRAIL_POINT_LIMIT);
      trailPointsRef.current.set(deviceId, visiblePoints);

      let trail = trailsRef.current.get(deviceId);
      if (!trail) {
        trail = L.polyline(visiblePoints, { color: avatar.color, weight: 2, opacity: 0.75, dashArray: "6,5" });
        trailsRef.current.set(deviceId, trail);
      } else {
        trail.setLatLngs(visiblePoints);
      }
      if (trailIdsRef.current.has(deviceId) && !map.hasLayer(trail)) trail.addTo(map);
    });
  }, [trailHistory]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map || !command) return;
    if (command.type === "fit") fitMarkers(map, markersRef.current);
    if (command.type === "jump" && command.deviceId !== undefined) {
      const marker = markersRef.current.get(command.deviceId);
      if (marker) {
        map.closePopup();
        map.setView(marker.getLatLng(), Math.max(map.getZoom(), JUMP_TO_ZOOM), { animate: true });
      }
    }
  }, [command]);

  return <div id="map" aria-label="Live animal tracking map" />;
}

function popupHtml(device: TelemetryDevice, avatar: DeviceAvatar) {
  const name = escapeHtml(device.name);
  const transport = transportPresentation(device.ingestPath);
  const signalMeasurements = device.rssi === null || device.snr === null ? "Not reported" : `${device.rssi} dBm / ${device.snr} dB`;
  const signal = `${signalMeasurements} · ${transport.badge}`;
  const battery = device.batteryPercent === undefined || device.batteryPercent === null ? `${(device.batt / 1000).toFixed(2)} V` : `${device.batteryPercent}%`;
  const source = device.source ? `<span class="label">Source</span><span class="value">${escapeHtml(device.source)}</span>` : "";
  const coordinates = formatMapCoordinates(device.lat, device.lon);
  const mapsUrl = googleMapsUrl(device.lat, device.lon);
  return `<div class="popup-content"><div class="popup-header">${avatarHtml(avatar, "popup-avatar")}<strong>${name}</strong><span class="card-status status-${device.status.toLowerCase()}" style="margin-left:6px;font-size:10px">${device.status}</span></div><div class="popup-grid"><span class="label">Coordinates</span><span class="value"><a class="card-coords card-coords-link" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${coordinates}</a></span><span class="label">Signal</span><span class="value">${signal}</span><span class="label">Battery</span><span class="value">${battery}</span><span class="label">Profile</span><span class="value">${escapeHtml(device.profile)}</span>${source}</div><div class="card-actions popup-actions"><button class="btn-action btn-jump" data-map-action="jump" data-device-id="${device.id}">↗ Jump To</button><button class="btn-action btn-follow" data-map-action="follow" data-device-id="${device.id}">● Follow</button><button class="btn-action btn-trail" data-map-action="trail" data-device-id="${device.id}">⌁ Trail</button><button class="btn-action btn-find" data-map-action="find" data-device-id="${device.id}">♟ Find Alert</button><button class="btn-action btn-cmd" data-map-action="command" data-device-id="${device.id}">⌘ Cmd</button></div></div>`;
}

function contextMenuHtml(point: L.LatLng) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu"><div class="map-context-heading">Map location</div>${coordinateActionRow(point, locationData)}<div class="map-context-actions"><button type="button" data-location-action="drop-pin" ${locationData}>📍 Drop temporary pin</button><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button></div><p class="map-context-hint">Right-click or long-press another point for more options.</p></div>`;
}

function temporaryPinPopupHtml(point: L.LatLng, pinId: number) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu temporary-pin-card"><div class="map-context-heading">Temporary meeting point</div>${coordinateActionRow(point, locationData)}<div class="map-context-actions"><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button><button type="button" class="danger" data-location-action="remove-pin" data-pin-id="${pinId}">× Remove pin</button></div><p class="map-context-hint">This pin stays only for this browser session.</p></div>`;
}

function coordinateActionRow(point: L.LatLng, locationData: string) {
  const mapsUrl = googleMapsUrl(point.lat, point.lng);
  return `<div class="map-context-coordinate-row"><a class="map-context-coordinates" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${formatMapCoordinates(point.lat, point.lng, 6)}</a><a class="map-context-icon-action" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open in Google Maps" aria-label="Open this location in Google Maps in a new tab">${openInNewTabIcon()}</a><button type="button" class="map-context-icon-action" data-location-action="copy" ${locationData} title="Copy location" aria-label="Copy this location">${copyIcon()}</button><button type="button" class="map-context-icon-action" data-location-action="share" ${locationData} title="Share location" aria-label="Share this location">${shareIcon()}</button></div>`;
}

function openInNewTabIcon() {
  return '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3h7v7"/><path d="M10 14 21 3"/><path d="M21 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5"/></svg>';
}

function copyIcon() {
  return '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="11" height="11" rx="2"/><path d="M15 9V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2h3"/></svg>';
}

function shareIcon() {
  return '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 10.5 6.8-4"/><path d="m8.6 13.5 6.8 4"/></svg>';
}

function locationDataAttributes(point: L.LatLng) {
  return `data-latitude="${point.lat.toFixed(6)}" data-longitude="${point.lng.toFixed(6)}"`;
}

function temporaryPinIcon() {
  return L.divIcon({
    className: "temporary-map-pin-icon",
    html: '<span class="temporary-map-pin-emoji" aria-hidden="true">📍</span>',
    iconSize: [36, 42],
    iconAnchor: [18, 40],
    popupAnchor: [0, -36],
  });
}

function avatarHtml(avatar: DeviceAvatar, className: string) {
  if (avatar.kind === "photo" && avatar.photoUrl) {
    return `<img class="${className} avatar-image" src="${escapeHtml(avatar.photoUrl)}" alt="">`;
  }
  return `<img class="${className} avatar-emoji-image" src="${emojiImageUrl(avatar.emoji)}" alt="${escapeHtml(avatar.emoji)}">`;
}

function markerElement(avatar: DeviceAvatar, markerColor: string, status: TelemetryDevice["status"]) {
  const pin = document.createElement("div");
  pin.className = `marker-pin bp-marker status-${status.toLowerCase()}`;
  pin.style.setProperty("--marker-color", markerColor);

  const face = document.createElement("div");
  face.className = "card-avatar marker-pin-face";
  if (avatar.kind === "photo" && avatar.photoUrl) {
    face.classList.add("has-photo");
    face.style.backgroundImage = `url(${JSON.stringify(avatar.photoUrl)})`;
  } else {
    const image = document.createElement("img");
    image.className = "avatar-emoji-image";
    image.src = emojiImageUrl(avatar.emoji);
    image.alt = avatar.emoji;
    image.draggable = false;
    face.append(image);
  }
  pin.append(face);
  return pin;
}

function fitMarkers(map: L.Map, markers: Map<number, L.Marker>) {
  const points = [...markers.values()].map((marker) => marker.getLatLng());
  if (points.length) map.fitBounds(L.latLngBounds(points), { padding: [50, 50], maxZoom: 16 });
}

function toDms(value: number, positive: string, negative: string) {
  const direction = value >= 0 ? positive : negative;
  const absolute = Math.abs(value);
  const degrees = Math.floor(absolute);
  const minutes = Math.floor((absolute - degrees) * 60);
  const seconds = ((absolute - degrees) * 60 - minutes) * 60;
  return `${degrees}°${String(minutes).padStart(2, "0")}'${seconds.toFixed(1).padStart(4, "0")}"${direction}`;
}

function formatDistance(metres: number) {
  return metres >= 1000 ? `${(metres / 1000).toFixed(2)} km` : `${Math.round(metres)} m`;
}

function escapeHtml(value: string) {
  return value.replace(/[&<>'"]/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" })[character] ?? character);
}
