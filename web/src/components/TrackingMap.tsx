"use client";

import L from "leaflet";
import { useEffect, useRef } from "react";
import { emojiImageUrl } from "@/lib/emoji";
import { normalizeMarkerColor } from "@/lib/markerColor";
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
    const map = L.map("map", { center: [51.505, -0.09], zoom: 13, zoomControl: false });
    mapRef.current = map;

    const street = L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", { attribution: "&copy; OpenStreetMap", maxZoom: 19 });
    const satellite = L.tileLayer("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", { attribution: "&copy; Esri World Imagery", maxZoom: 19 });
    const clarity = L.tileLayer("https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", { attribution: "&copy; Esri Clarity", maxZoom: 19 });
    const topo = L.tileLayer("https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png", { attribution: "&copy; OpenTopoMap", maxZoom: 17 });
    const humanitarian = L.tileLayer("https://{s}.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png", { attribution: "&copy; OpenStreetMap, Tiles: HOT", maxZoom: 19 });
    const esriTopo = L.tileLayer("https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}", { attribution: "&copy; Esri", maxZoom: 19 });
    street.addTo(map);
    L.control.layers({ Street: street, Satellite: satellite, "Satellite HD": clarity, Topographic: topo, Humanitarian: humanitarian, "Esri Topo": esriTopo }, undefined, { position: "topright", collapsed: true }).addTo(map);
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
    let measuring = false;
    const MeasureControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        button.type = "button";
        button.title = "Measure distance (click points on map)";
        button.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="1" y="7" width="22" height="10" rx="1"/><path d="M5 7v5M9 7v3M13 7v5M17 7v3M21 7v5"/></svg>';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => {
          measuring = !measuring;
          button.classList.toggle("active", measuring);
          map.getContainer().style.cursor = measuring ? "crosshair" : "";
          if (!measuring) {
            measureLayers.splice(0).forEach((layer) => map.removeLayer(layer));
            measurePoints.splice(0);
          }
        });
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
      measurePoints.push(event.latlng);
      const dot = L.circleMarker(event.latlng, { radius: 4, color: "#1d9bf0", fillOpacity: 1 }).addTo(map);
      measureLayers.push(dot);
      if (measurePoints.length > 1) {
        const line = L.polyline(measurePoints, { color: "#1d9bf0", weight: 2, dashArray: "5,5" }).addTo(map);
        measureLayers.push(line);
        const total = measurePoints.slice(1).reduce((sum, point, index) => sum + point.distanceTo(measurePoints[index]), 0);
        const label = L.marker(event.latlng, { interactive: false, icon: L.divIcon({ className: "measure-label", html: formatDistance(total), iconSize: undefined }) }).addTo(map);
        measureLayers.push(label);
      }
    });

    const mapContainer = map.getContainer();
    const handlePopupAction = (event: MouseEvent) => {
      const target = (event.target as HTMLElement).closest<HTMLButtonElement>("[data-map-action]");
      if (!target) return;
      const device = devicesRef.current.find((item) => item.id === Number(target.dataset.deviceId));
      if (device) actionRef.current(device, target.dataset.mapAction as DeviceAction);
    };
    mapContainer.addEventListener("click", handlePopupAction);

    return () => {
      mapContainer.removeEventListener("click", handlePopupAction);
      map.remove();
      mapRef.current = null;
      markers.clear();
      trails.clear();
      trailPoints.clear();
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
        html: `<div class="marker-pin bp-marker status-${device.status.toLowerCase()}" style="--marker-color:${markerColor}"><div class="marker-pin-face">${avatarHtml(avatar, "bp-marker-avatar")}</div></div>`,
        iconSize: [40, 52],
        iconAnchor: [20, 51],
        popupAnchor: [0, -47],
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
        map.setView(marker.getLatLng(), 17, { animate: true });
        marker.openPopup();
      }
    }
  }, [command]);

  return <div id="map" aria-label="Live animal tracking map" />;
}

function popupHtml(device: TelemetryDevice, avatar: DeviceAvatar) {
  const name = escapeHtml(device.name);
  const signal = device.rssi === null || device.snr === null ? "Not reported" : `${device.rssi} dBm / ${device.snr} dB`;
  const battery = device.batteryPercent === undefined || device.batteryPercent === null ? `${(device.batt / 1000).toFixed(2)} V` : `${device.batteryPercent}%`;
  const source = device.source ? `<span class="label">Source</span><span class="value">${escapeHtml(device.source)}</span>` : "";
  return `<div class="popup-content"><div class="popup-header">${avatarHtml(avatar, "popup-avatar")}<strong>${name}</strong><span class="card-status status-${device.status.toLowerCase()}" style="margin-left:6px;font-size:10px">${device.status}</span></div><div class="popup-grid"><span class="label">Signal</span><span class="value">${signal}</span><span class="label">Battery</span><span class="value">${battery}</span><span class="label">Profile</span><span class="value">${escapeHtml(device.profile)}</span>${source}</div><div class="card-actions popup-actions"><button class="btn-action btn-jump" data-map-action="jump" data-device-id="${device.id}">↗ Jump To</button><button class="btn-action btn-follow" data-map-action="follow" data-device-id="${device.id}">● Follow</button><button class="btn-action btn-trail" data-map-action="trail" data-device-id="${device.id}">⌁ Trail</button><button class="btn-action btn-find" data-map-action="find" data-device-id="${device.id}">♟ Find Alert</button><button class="btn-action btn-cmd" data-map-action="command" data-device-id="${device.id}">⌘ Cmd</button></div></div>`;
}

function avatarHtml(avatar: DeviceAvatar, className: string) {
  if (avatar.kind === "photo" && avatar.photoUrl) {
    return `<img class="${className} avatar-image" src="${escapeHtml(avatar.photoUrl)}" alt="">`;
  }
  return `<img class="${className} avatar-emoji-image" src="${emojiImageUrl(avatar.emoji)}" alt="${escapeHtml(avatar.emoji)}">`;
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
