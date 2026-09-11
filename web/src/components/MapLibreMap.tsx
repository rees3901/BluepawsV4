"use client";

import * as maplibregl from "maplibre-gl";
import type { GeoJSONSource, LngLatBoundsLike, Map as MapLibre } from "maplibre-gl";
import { Protocol } from "pmtiles";
import { useEffect, useRef, type MutableRefObject } from "react";
import { emojiImageUrl } from "@/lib/emoji";
import { isCollarOffline } from "@/lib/devicePresence";
import { mapLibreStyle } from "@/lib/mapLibreStyle";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import { normalizeMarkerColor } from "@/lib/markerColor";
import { VISIBLE_TRAIL_POINT_LIMIT } from "@/lib/trailPoints";
import { locatedDevices, type MapRendererProps } from "@/components/mapRenderer";
import type { DeviceAction, DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

const JUMP_TO_ZOOM = 17;
const TRAILS_SOURCE = "bluepaws-trails";
const TRAILS_LAYER = "bluepaws-trails";
let protocolRegistered = false;

export default function MapLibreMap(props: MapRendererProps) {
  const { devices, avatars, presenceNow, sidebarOpen, followedId, trailIds, trailHistory, command, onNotice } = props;
  const containerRef = useRef<HTMLDivElement | null>(null);
  const mapRef = useRef<MapLibre | null>(null);
  const markersRef = useRef(new Map<number, maplibregl.Marker>());
  const propsRef = useRef(props);

  useEffect(() => { propsRef.current = props; }, [props]);

  useEffect(() => {
    if (!containerRef.current) return;
    if (!protocolRegistered) {
      const protocol = new Protocol();
      maplibregl.addProtocol("pmtiles", protocol.tile);
      protocolRegistered = true;
    }
    const map = new maplibregl.Map({
      container: containerRef.current,
      style: mapLibreStyle(process.env.NEXT_PUBLIC_BLUEPAWS_PMTILES_URL),
      center: [EMPTY_MAP_CENTER[1], EMPTY_MAP_CENTER[0]],
      zoom: EMPTY_MAP_ZOOM,
      attributionControl: {},
    });
    map.addControl(new maplibregl.NavigationControl({ showCompass: false }), "bottom-right");
    map.on("error", event => onNotice?.(`Vector map: ${event.error?.message ?? "source failed"}`));
    mapRef.current = map;
    const markers = markersRef.current;
    return () => {
      markers.forEach(marker => marker.remove());
      markers.clear();
      map.remove();
      mapRef.current = null;
    };
  }, [onNotice]);

  useEffect(() => {
    const timer = window.setTimeout(() => mapRef.current?.resize(), 340);
    return () => window.clearTimeout(timer);
  }, [sidebarOpen]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    const sync = () => {
      const visible = locatedDevices(devices);
      const activeIds = new Set(visible.map(device => device.id));
      markersRef.current.forEach((marker, id) => {
        if (!activeIds.has(id)) { marker.remove(); markersRef.current.delete(id); }
      });
      for (const device of visible) {
        const avatar = avatars[device.id];
        if (!avatar) continue;
        let marker = markersRef.current.get(device.id);
        if (!marker) {
          const element = markerElement(avatar, normalizeMarkerColor(avatar.color), device.status, isCollarOffline(device, presenceNow));
          marker = new maplibregl.Marker({ element, anchor: "bottom" }).setLngLat([device.lon, device.lat]).addTo(map);
          markersRef.current.set(device.id, marker);
          element.addEventListener("click", () => openPopup(map, marker!, device.id, propsRef));
        } else {
          marker.setLngLat([device.lon, device.lat]);
          const replacement = markerElement(avatar, normalizeMarkerColor(avatar.color), device.status, isCollarOffline(device, presenceNow));
          marker.getElement().className = replacement.className;
          marker.getElement().replaceChildren(...replacement.childNodes);
          marker.getElement().style.cssText = replacement.style.cssText;
        }
      }
      syncTrails(map, visible, avatars, trailIds, trailHistory);
    };
    if (map.loaded()) sync();
    else map.once("load", sync);
    return () => { map.off("load", sync); };
  }, [avatars, devices, presenceNow, trailHistory, trailIds]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map || followedId === null) return;
    const device = locatedDevices(devices).find(item => item.id === followedId);
    if (device) map.easeTo({ center: [device.lon, device.lat] });
  }, [devices, followedId]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map || !command) return;
    const visible = locatedDevices(devices);
    if (command.type === "fit") fitDevices(map, visible, sidebarOpen);
    if ((command.type === "jump" || command.type === "open") && command.deviceId !== undefined) {
      const device = visible.find(item => item.id === command.deviceId);
      const marker = markersRef.current.get(command.deviceId);
      if (device) map.easeTo({ center: [device.lon, device.lat], zoom: Math.max(map.getZoom(), JUMP_TO_ZOOM) });
      if (marker && command.type === "open") openPopup(map, marker, command.deviceId, propsRef);
    }
  }, [command, devices, sidebarOpen]);

  return <div ref={containerRef} id="map" className="maplibre-map" aria-label="Live animal tracking vector map" />;
}

function markerElement(avatar: DeviceAvatar, color: string, status: TelemetryDevice["status"], offline: boolean) {
  const pin = document.createElement("div");
  pin.className = `marker-pin bp-marker maplibre-marker status-${status.toLowerCase()}${offline ? " marker-offline" : ""}`;
  pin.style.setProperty("--marker-color", color);
  const face = document.createElement("span");
  face.className = "card-avatar marker-pin-face";
  if (avatar.kind === "photo" && avatar.photoUrl) {
    const image = document.createElement("img"); image.src = avatar.photoUrl; image.alt = ""; face.appendChild(image);
  } else {
    const image = document.createElement("img"); image.src = emojiImageUrl(avatar.emoji); image.alt = ""; face.appendChild(image);
  }
  pin.appendChild(face);
  return pin;
}

function openPopup(map: MapLibre, marker: maplibregl.Marker, deviceId: number, propsRef: MutableRefObject<MapRendererProps>) {
  const props = propsRef.current;
  const device = props.devices.find(item => item.id === deviceId);
  if (!device) return;
  const content = document.createElement("div");
  content.className = "maplibre-device-popup";
  const title = document.createElement("strong"); title.textContent = device.name; content.appendChild(title);
  const detail = document.createElement("span"); detail.textContent = `${device.status} · ${device.lat.toFixed(6)}, ${device.lon.toFixed(6)}`; content.appendChild(detail);
  const actions = document.createElement("div"); actions.className = "maplibre-popup-actions";
  const available: DeviceAction[] = props.readOnly || device.entity === "hub" ? ["jump", "follow", "trail"] : ["jump", "follow", "trail", "find", "command"];
  for (const action of available) {
    const button = document.createElement("button"); button.type = "button"; button.textContent = action === "jump" ? "Jump To" : action; button.addEventListener("click", () => props.onAction(device, action)); actions.appendChild(button);
  }
  content.appendChild(actions);
  new maplibregl.Popup({ offset: 38, maxWidth: "380px" }).setDOMContent(content).setLngLat(marker.getLngLat()).addTo(map);
}

function syncTrails(map: MapLibre, devices: TelemetryDevice[], avatars: Record<number, DeviceAvatar>, trailIds: Set<number>, history: MapRendererProps["trailHistory"]) {
  const features = devices.filter(device => trailIds.has(device.id)).map(device => ({
    type: "Feature" as const,
    properties: { color: normalizeMarkerColor(avatars[device.id]?.color) },
    geometry: { type: "LineString" as const, coordinates: [...(history[device.id] ?? []).slice(-VISIBLE_TRAIL_POINT_LIMIT).map(point => [point.lon, point.lat]), [device.lon, device.lat]] },
  })).filter(feature => feature.geometry.coordinates.length > 1);
  const data = { type: "FeatureCollection" as const, features };
  const source = map.getSource(TRAILS_SOURCE) as GeoJSONSource | undefined;
  if (source) source.setData(data);
  else {
    map.addSource(TRAILS_SOURCE, { type: "geojson", data });
    map.addLayer({ id: TRAILS_LAYER, type: "line", source: TRAILS_SOURCE, paint: { "line-color": ["get", "color"], "line-width": 2, "line-opacity": 0.75, "line-dasharray": [3, 2] } });
  }
}

function fitDevices(map: MapLibre, devices: TelemetryDevice[], sidebarOpen: boolean) {
  if (devices.length === 0) return;
  const bounds = devices.reduce((value, device) => value.extend([device.lon, device.lat]), new maplibregl.LngLatBounds());
  map.fitBounds(bounds as LngLatBoundsLike, { padding: { top: 70, right: 70, bottom: 70, left: sidebarOpen ? 430 : 70 }, maxZoom: JUMP_TO_ZOOM });
}
