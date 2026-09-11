"use client";

import * as maplibregl from "maplibre-gl";
import type { GeoJSONSource, LngLatBoundsLike, Map as MapLibre } from "maplibre-gl";
import { Protocol } from "pmtiles";
import { useEffect, useRef, useState, type MutableRefObject } from "react";
import { emojiImageUrl } from "@/lib/emoji";
import { isCollarOffline } from "@/lib/devicePresence";
import { mapLibreStyle } from "@/lib/mapLibreStyle";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import { normalizeMarkerColor } from "@/lib/markerColor";
import { mapPopupHtml } from "@/lib/mapPopup";
import { VISIBLE_TRAIL_POINT_LIMIT } from "@/lib/trailPoints";
import { locatedDevices, type ConfiguredMapRendererProps } from "@/components/mapRenderer";
import type { DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

const JUMP_TO_ZOOM = 17;
const TRAILS_SOURCE = "bluepaws-trails";
const TRAILS_LAYER = "bluepaws-trails";
const MEASURE_SOURCE = "bluepaws-measurement";
const MEASURE_LINE_LAYER = "bluepaws-measurement-line";
const MEASURE_POINT_LAYER = "bluepaws-measurement-points";
let protocolRegistered = false;

maplibregl.setWorkerUrl("/maplibre/maplibre-gl-worker.mjs");

export default function MapLibreMap(props: ConfiguredMapRendererProps) {
  const { devices, avatars, presenceNow, sidebarOpen, followedId, trailIds, trailHistory, vectorSource, command, onNotice } = props;
  const containerRef = useRef<HTMLDivElement | null>(null);
  const mapRef = useRef<MapLibre | null>(null);
  const markersRef = useRef(new Map<number, maplibregl.Marker>());
  const measurementPointsRef = useRef<[number, number][]>([]);
  const measurementPopupRef = useRef<maplibregl.Popup | null>(null);
  const propsRef = useRef(props);
  const [measuring, setMeasuring] = useState(false);

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
      style: mapLibreStyle(vectorSource === "pmtiles" ? process.env.NEXT_PUBLIC_BLUEPAWS_PMTILES_URL : undefined),
      center: [EMPTY_MAP_CENTER[1], EMPTY_MAP_CENTER[0]],
      zoom: EMPTY_MAP_ZOOM,
      attributionControl: {},
    });
    map.addControl(new maplibregl.ScaleControl({ unit: "imperial" }), "bottom-right");
    map.on("error", event => onNotice?.(`Vector map: ${event.error?.message ?? "source failed"}`));
    const stopFollowingForGesture = (event: maplibregl.MapLibreEvent<MouseEvent | TouchEvent | WheelEvent | undefined>) => {
      if (event.originalEvent && propsRef.current.followedId !== null) propsRef.current.onUserNavigation?.();
    };
    map.on("dragstart", stopFollowingForGesture);
    map.on("zoomstart", stopFollowingForGesture);
    mapRef.current = map;
    const markers = markersRef.current;
    return () => {
      markers.forEach(marker => marker.remove());
      markers.clear();
      measurementPopupRef.current?.remove();
      map.remove();
      mapRef.current = null;
    };
  }, [onNotice, vectorSource]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    map.getCanvas().style.cursor = measuring ? "crosshair" : "";
    if (!measuring) clearMeasurement(map, measurementPointsRef, measurementPopupRef);
    const addPoint = (event: maplibregl.MapMouseEvent) => {
      if (!measuring) return;
      measurementPointsRef.current.push([event.lngLat.lng, event.lngLat.lat]);
      renderMeasurement(map, measurementPointsRef.current);
      if (measurementPointsRef.current.length < 2) return;
      const total = measurementDistanceMetres(measurementPointsRef.current);
      measurementPopupRef.current?.remove();
      measurementPopupRef.current = new maplibregl.Popup({ closeButton: false, closeOnClick: false, offset: 12, className: "measure-popup" })
        .setLngLat(event.lngLat)
        .setText(formatDistance(total))
        .addTo(map);
    };
    map.on("click", addPoint);
    return () => { map.off("click", addPoint); };
  }, [measuring, vectorSource]);

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
          const element = markerElement(device.name, avatar, normalizeMarkerColor(avatar.color), device.status, isCollarOffline(device, presenceNow));
          marker = new maplibregl.Marker({ element, anchor: "bottom" }).setLngLat([device.lon, device.lat]).addTo(map);
          markersRef.current.set(device.id, marker);
          const openMarker = () => openPopup(map, marker!, device.id, propsRef);
          element.addEventListener("click", event => { event.stopPropagation(); openMarker(); });
          element.addEventListener("keydown", event => {
            if (event.key !== "Enter" && event.key !== " ") return;
            event.preventDefault();
            event.stopPropagation();
            openMarker();
          });
        } else {
          marker.setLngLat([device.lon, device.lat]);
          updateMarkerElement(marker.getElement(), device.name, avatar, normalizeMarkerColor(avatar.color), device.status, isCollarOffline(device, presenceNow));
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
    const currentProps = propsRef.current;
    const visible = locatedDevices(currentProps.devices);
    if (command.type === "fit") fitDevices(map, visible, currentProps.sidebarOpen);
    if ((command.type === "jump" || command.type === "open") && command.deviceId !== undefined) {
      const device = visible.find(item => item.id === command.deviceId);
      const marker = markersRef.current.get(command.deviceId);
      if (device) map.easeTo({ center: [device.lon, device.lat], zoom: Math.max(map.getZoom(), JUMP_TO_ZOOM) });
      if (marker && command.type === "open") openPopup(map, marker, command.deviceId, propsRef);
    }
  }, [command]);

  const stopFollowing = () => {
    if (propsRef.current.followedId !== null) propsRef.current.onUserNavigation?.();
  };
  const centerHome = () => {
    const map = mapRef.current;
    const homeHub = propsRef.current.devices.find(device => device.entity === "hub" && device.hasGps);
    if (!map || !homeHub) {
      propsRef.current.onNotice?.("Home Hub location is not available yet");
      return;
    }
    stopFollowing();
    map.easeTo({ center: [homeHub.lon, homeHub.lat], zoom: Math.max(map.getZoom(), JUMP_TO_ZOOM) });
  };
  const fitAll = () => {
    const map = mapRef.current;
    if (!map) return;
    stopFollowing();
    fitDevices(map, locatedDevices(propsRef.current.devices), propsRef.current.sidebarOpen);
  };
  const zoomBy = (delta: number) => {
    const map = mapRef.current;
    if (!map) return;
    stopFollowing();
    map.easeTo({ zoom: map.getZoom() + delta });
  };

  return <>
    <div ref={containerRef} id="map" className="maplibre-map" aria-label="Live animal tracking vector map" />
    <div className="maplibre-tool-stack" aria-label="Vector map tools">
      <button type="button" className="leaflet-map-btn" title="Center on Home Hub" aria-label="Center map on Home Hub" data-tour="map-home" onClick={centerHome}><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5"><circle cx="8" cy="8" r="5"/><path d="M8 1v3m0 8v3M1 8h3m8 0h3"/><circle cx="8" cy="8" r="1.5" fill="currentColor" stroke="none"/></svg></button>
      <button type="button" className="leaflet-map-btn" title="Fit all markers into view" aria-label="Fit all markers into view" data-tour="map-fit" onClick={fitAll}><span className="fit-markers-icon maplibre-fit-markers-icon" aria-hidden="true" /></button>
      {props.onAllTrailsToggle ? <button type="button" className={`leaflet-map-btn global-trails-btn${props.allTrailsVisible ? " active" : ""}`} title={props.allTrailsVisible ? "Hide all breadcrumb trails" : "Show all breadcrumb trails"} aria-label={props.allTrailsVisible ? "Hide all breadcrumb trails" : "Show all breadcrumb trails"} aria-pressed={props.allTrailsVisible} disabled={!props.trailsAvailable} data-tour="map-trails" onClick={props.onAllTrailsToggle}><span className="global-trails-icon" aria-hidden="true" /></button> : null}
      <button type="button" className={`leaflet-map-btn${measuring ? " active" : ""}`} title="Measure distance (click points on map)" aria-label="Measure distance on the map" aria-pressed={measuring} data-tour="map-measure" onClick={() => setMeasuring(active => !active)}><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8"><rect x="1" y="7" width="22" height="10" rx="1"/><path d="M5 7v5M9 7v3M13 7v5M17 7v3M21 7v5"/></svg></button>
    </div>
    <div className="maplibre-zoom-stack" aria-label="Map zoom controls">
      <button type="button" aria-label="Zoom in" onClick={() => zoomBy(1)}>+</button>
      <button type="button" aria-label="Zoom out" onClick={() => zoomBy(-1)}>−</button>
    </div>
  </>;
}

function renderMeasurement(map: MapLibre, points: [number, number][]) {
  const features = [
    ...(points.length > 1 ? [{ type: "Feature" as const, properties: {}, geometry: { type: "LineString" as const, coordinates: points } }] : []),
    ...points.map(coordinates => ({ type: "Feature" as const, properties: {}, geometry: { type: "Point" as const, coordinates } })),
  ];
  const data = { type: "FeatureCollection" as const, features };
  const source = map.getSource(MEASURE_SOURCE) as GeoJSONSource | undefined;
  if (source) source.setData(data);
  else {
    map.addSource(MEASURE_SOURCE, { type: "geojson", data });
    map.addLayer({ id: MEASURE_LINE_LAYER, type: "line", source: MEASURE_SOURCE, filter: ["==", ["geometry-type"], "LineString"], paint: { "line-color": "#1d9bf0", "line-width": 2, "line-dasharray": [3, 2] } });
    map.addLayer({ id: MEASURE_POINT_LAYER, type: "circle", source: MEASURE_SOURCE, filter: ["==", ["geometry-type"], "Point"], paint: { "circle-radius": 4, "circle-color": "#1d9bf0", "circle-stroke-color": "#ffffff", "circle-stroke-width": 1 } });
  }
}

function clearMeasurement(map: MapLibre, pointsRef: MutableRefObject<[number, number][]>, popupRef: MutableRefObject<maplibregl.Popup | null>) {
  pointsRef.current = [];
  const source = map.getSource(MEASURE_SOURCE) as GeoJSONSource | undefined;
  source?.setData({ type: "FeatureCollection", features: [] });
  popupRef.current?.remove();
  popupRef.current = null;
}

function measurementDistanceMetres(points: [number, number][]) {
  const earthRadius = 6_371_000;
  return points.slice(1).reduce((total, point, index) => {
    const previous = points[index];
    const lat1 = previous[1] * Math.PI / 180;
    const lat2 = point[1] * Math.PI / 180;
    const deltaLat = lat2 - lat1;
    const deltaLon = (point[0] - previous[0]) * Math.PI / 180;
    const a = Math.sin(deltaLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) * Math.sin(deltaLon / 2) ** 2;
    return total + earthRadius * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  }, 0);
}

function formatDistance(metres: number) {
  if (metres < 1_000) return `${Math.round(metres)} m`;
  return `${(metres / 1_000).toFixed(metres < 10_000 ? 2 : 1)} km`;
}

function markerElement(name: string, avatar: DeviceAvatar, color: string, status: TelemetryDevice["status"], offline: boolean) {
  const pin = document.createElement("div");
  pin.className = "marker-pin bp-marker maplibre-marker";
  pin.tabIndex = 0;
  pin.setAttribute("role", "button");
  updateMarkerElement(pin, name, avatar, color, status, offline);
  return pin;
}

function updateMarkerElement(element: HTMLElement, name: string, avatar: DeviceAvatar, color: string, status: TelemetryDevice["status"], offline: boolean) {
  element.classList.remove("status-home", "status-out", "status-lost", "status-error");
  element.classList.add(`status-${status.toLowerCase()}`);
  element.classList.toggle("marker-offline", offline);
  element.style.setProperty("--marker-color", color);
  element.setAttribute("aria-label", `Open ${name} map marker`);
  element.title = name;

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
  element.replaceChildren(face);
}

function openPopup(map: MapLibre, marker: maplibregl.Marker, deviceId: number, propsRef: MutableRefObject<ConfiguredMapRendererProps>) {
  const props = propsRef.current;
  const device = props.devices.find(item => item.id === deviceId);
  const avatar = props.avatars[deviceId];
  if (!device || !avatar) return;

  const template = document.createElement("template");
  template.innerHTML = mapPopupHtml(device, avatar, props.presenceNow, props.readOnly, props.followedId === deviceId, props.trailIds.has(deviceId));
  const content = template.content.firstElementChild as HTMLElement | null;
  if (!content) return;
  content.addEventListener("click", event => {
    const action = (event.target as HTMLElement).closest<HTMLButtonElement>("[data-map-action]");
    if (!action) return;
    const currentProps = propsRef.current;
    const currentDevice = currentProps.devices.find(item => item.id === deviceId);
    if (currentDevice) currentProps.onAction(currentDevice, action.dataset.mapAction as Parameters<ConfiguredMapRendererProps["onAction"]>[1]);
  });
  new maplibregl.Popup({ offset: 38, maxWidth: "380px", className: "device-marker-popup maplibre-device-marker-popup" })
    .setDOMContent(content)
    .setLngLat(marker.getLngLat())
    .addTo(map);
}

function syncTrails(map: MapLibre, devices: TelemetryDevice[], avatars: Record<number, DeviceAvatar>, trailIds: Set<number>, history: ConfiguredMapRendererProps["trailHistory"]) {
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
