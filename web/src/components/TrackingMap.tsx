"use client";

import L from "leaflet";
import { useEffect, useRef } from "react";
import { batteryPresentation, signalQuality } from "@/components/Indicators";
import { collarFault } from "@/lib/collarFault";
import { emojiImageUrl } from "@/lib/emoji";
import { isCollarOffline } from "@/lib/devicePresence";
import { formatHomeDistance, formatMapCoordinates, googleMapsUrl, homeDistanceMetres } from "@/lib/mapLocation";
import { alternatePreviewMapLayer, MAP_LAYER_DEFINITIONS, MAP_LAYER_PICKER_NAMES, previewMapZoom, type MapLayerName, type MapLayerPickerName } from "@/lib/mapLayers";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
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
  presenceNow: number;
  sidebarOpen: boolean;
  followedId: number | null;
  trailIds: Set<number>;
  trailHistory: Record<number, TrailPoint[]>;
  allTrailsVisible?: boolean;
  trailsAvailable?: boolean;
  command: MapCommand | null;
  onAction: (device: TelemetryDevice, action: DeviceAction) => void;
  onAllTrailsToggle?: () => void;
  onNotice?: (message: string) => void;
  readOnly?: boolean;
}

const JUMP_TO_ZOOM = 17;
const MARKER_SLIDE_DURATION_MS = 750;
const MAX_ANIMATED_MARKER_DISTANCE_METRES = 2_000;

export default function TrackingMap(props: TrackingMapProps) {
  const { devices, avatars, presenceNow, sidebarOpen, followedId, trailIds, trailHistory, allTrailsVisible = false, trailsAvailable = false, command, onAction, onAllTrailsToggle, onNotice, readOnly = false } = props;
  const mapRef = useRef<L.Map | null>(null);
  const markersRef = useRef(new Map<number, L.Marker>());
  const markerAnimationsRef = useRef(new Map<number, number>());
  const trailsRef = useRef(new Map<number, L.Polyline>());
  const trailPointsRef = useRef(new Map<number, TrailLatLng[]>());
  const devicesRef = useRef(devices);
  const avatarsRef = useRef(avatars);
  const trailIdsRef = useRef(trailIds);
  const actionRef = useRef(onAction);
  const allTrailsToggleRef = useRef(onAllTrailsToggle);
  const allTrailsVisibleRef = useRef(allTrailsVisible);
  const trailsAvailableRef = useRef(trailsAvailable);
  const noticeRef = useRef(onNotice);
  const allTrailsButtonRef = useRef<HTMLButtonElement | null>(null);

  useEffect(() => {
    devicesRef.current = devices;
    avatarsRef.current = avatars;
    trailIdsRef.current = trailIds;
    actionRef.current = onAction;
    allTrailsToggleRef.current = onAllTrailsToggle;
    allTrailsVisibleRef.current = allTrailsVisible;
    trailsAvailableRef.current = trailsAvailable;
    noticeRef.current = onNotice;
    const trailButton = allTrailsButtonRef.current;
    if (trailButton) {
      const label = allTrailsVisible ? "Hide all breadcrumb trails" : "Show all breadcrumb trails";
      trailButton.classList.toggle("active", allTrailsVisible);
      trailButton.disabled = !trailsAvailable;
      trailButton.title = label;
      trailButton.setAttribute("aria-label", label);
      trailButton.setAttribute("aria-pressed", String(allTrailsVisible));
    }
  }, [allTrailsVisible, avatars, devices, onAction, onAllTrailsToggle, onNotice, trailIds, trailsAvailable]);

  useEffect(() => {
    const markers = markersRef.current;
    const markerAnimations = markerAnimationsRef.current;
    const trails = trailsRef.current;
    const trailPoints = trailPointsRef.current;
    const map = L.map("map", { center: [...EMPTY_MAP_CENTER], zoom: EMPTY_MAP_ZOOM, zoomControl: false, tapHold: true });
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
      MAP_LAYER_PICKER_NAMES.map((name) => [
        name,
        createTileLayer(name),
      ]),
    ) as Record<MapLayerPickerName, L.TileLayer>;
    baseLayers.Street.addTo(map);
    let currentLayerName: MapLayerName = "Street";
    let layerPreviewMap: L.Map | null = null;
    let layerPreviewLayer: L.TileLayer | null = null;
    let layerPreviewUpdateTimer: number | null = null;
    const layerControl = L.control.layers(baseLayers, undefined, { position: "topright", collapsed: true }).addTo(map);
    const layerControlElement = layerControl.getContainer();
    let layerControlOpen = false;
    const setLayerControlOpen = (open: boolean) => {
      layerControlOpen = open;
      layerControlElement?.classList.toggle("bp-layer-open", open);
      if (open) layerControl.expand();
      else layerControl.collapse();
    };
    const updateLayerPreview = () => {
      if (!layerPreviewMap) return;
      const previewLayerName = alternatePreviewMapLayer(currentLayerName);
      const previewDefinition = MAP_LAYER_DEFINITIONS[previewLayerName];
      const center = map.getCenter();
      const zoom = previewMapZoom(map.getZoom(), previewLayerName);
      if (layerPreviewLayer) layerPreviewLayer.removeFrom(layerPreviewMap);
      layerPreviewLayer = createTileLayer(previewLayerName).addTo(layerPreviewMap);
      layerPreviewMap.setMaxZoom(previewDefinition.maxZoom);
      layerPreviewMap.setView(center, zoom, { animate: false });
      window.setTimeout(() => layerPreviewMap?.invalidateSize(), 0);
    };
    const scheduleLayerPreviewUpdate = () => {
      if (layerPreviewUpdateTimer !== null) window.clearTimeout(layerPreviewUpdateTimer);
      layerPreviewUpdateTimer = window.setTimeout(() => {
        layerPreviewUpdateTimer = null;
        updateLayerPreview();
      }, 350);
    };
    if (layerControlElement) {
      layerControlElement.classList.add("bp-click-layer-control");
      const toggle = layerControlElement.querySelector<HTMLElement>(".leaflet-control-layers-toggle");
      const previewButton = L.DomUtil.create("button", "bp-layer-preview-toggle", layerControlElement) as HTMLButtonElement;
      previewButton.type = "button";
      previewButton.title = "Preview alternate map style and choose map layer";
      previewButton.setAttribute("aria-label", "Preview alternate map style and choose map layer");
      const previewMapElement = L.DomUtil.create("span", "bp-layer-preview-map", previewButton);
      L.DomEvent.disableClickPropagation(layerControlElement);
      L.DomEvent.disableScrollPropagation(layerControlElement);
      L.DomEvent.on(previewButton, "click", (event: Event) => {
        L.DomEvent.stop(event);
        setLayerControlOpen(!layerControlOpen);
      });
      L.DomEvent.on(toggle ?? layerControlElement, "click", (event: Event) => {
        L.DomEvent.stop(event);
        setLayerControlOpen(!layerControlOpen);
      });
      layerPreviewMap = L.map(previewMapElement, {
        attributionControl: false,
        boxZoom: false,
        center: map.getCenter(),
        doubleClickZoom: false,
        dragging: false,
        keyboard: false,
        scrollWheelZoom: false,
        zoom: previewMapZoom(map.getZoom(), alternatePreviewMapLayer(currentLayerName)),
        zoomControl: false,
      });
      updateLayerPreview();
    }
    map.on("baselayerchange", (event: L.LeafletEvent & { name?: string }) => {
      if (isMapLayerName(event.name)) currentLayerName = event.name;
      setLayerControlOpen(false);
      scheduleLayerPreviewUpdate();
    });
    map.on("moveend zoomend", scheduleLayerPreviewUpdate);
    L.control.zoom({ position: "bottomleft" }).addTo(map);

    const HomeControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        button.type = "button";
        button.title = "Center on Home Hub";
        button.setAttribute("aria-label", "Center map on Home Hub");
        button.innerHTML = '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><circle cx="8" cy="8" r="5"/><path d="M8 1v3m0 8v3M1 8h3m8 0h3"/><circle cx="8" cy="8" r="1.5" fill="currentColor" stroke="none"/></svg>';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => {
          const homeHub = devicesRef.current.find((device) => device.entity === "hub" && device.hasGps);
          if (!homeHub) {
            noticeRef.current?.("Home Hub location is not available yet");
            return;
          }
          map.closePopup();
          map.setView([homeHub.lat, homeHub.lon], Math.max(map.getZoom(), JUMP_TO_ZOOM), { animate: true });
        });
        return button;
      },
    });
    new HomeControl().addTo(map);

    const FitControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        button.type = "button";
        button.title = "Fit all markers into view";
        button.setAttribute("aria-label", "Fit all markers into view");
        button.innerHTML = '<img class="fit-markers-icon" src="/icons/location-fit-markers.png" alt="" aria-hidden="true">';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => fitMarkers(map, markersRef.current));
        return button;
      },
    });
    new FitControl().addTo(map);

    const TrailsControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn global-trails-btn") as HTMLButtonElement;
        const label = allTrailsVisibleRef.current ? "Hide all breadcrumb trails" : "Show all breadcrumb trails";
        allTrailsButtonRef.current = button;
        button.type = "button";
        button.title = label;
        button.disabled = !trailsAvailableRef.current;
        button.classList.toggle("active", allTrailsVisibleRef.current);
        button.setAttribute("aria-label", label);
        button.setAttribute("aria-pressed", String(allTrailsVisibleRef.current));
        button.innerHTML = '<span class="global-trails-icon" aria-hidden="true"></span>';
        L.DomEvent.disableClickPropagation(button);
        L.DomEvent.on(button, "click", () => allTrailsToggleRef.current?.());
        return button;
      },
    });
    if (allTrailsToggleRef.current) new TrailsControl().addTo(map);

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
        coordinates.tabIndex = 0;
        coordinates.setAttribute("role", "status");
        coordinates.setAttribute("aria-label", "Map cursor coordinates. Hover or focus to reveal.");
        const tab = L.DomUtil.create("span", "leaflet-cursor-coords-tab", coordinates);
        tab.textContent = "⌖";
        tab.setAttribute("aria-hidden", "true");
        const value = L.DomUtil.create("span", "leaflet-cursor-coords-value", coordinates);
        value.id = "cursorCoords";
        value.textContent = "Move over map";
        L.DomEvent.disableClickPropagation(coordinates);
        L.DomEvent.disableScrollPropagation(coordinates);
        return coordinates;
      },
    });
    new CoordsControl().addTo(map);
    L.control.scale({ position: "bottomright", imperial: true, metric: true }).addTo(map);

    map.on("mousemove", (event) => {
      const element = document.getElementById("cursorCoords");
      if (element) {
        element.innerHTML = `${event.latlng.lat.toFixed(6)}, ${event.latlng.lng.toFixed(6)}<br>${toDms(event.latlng.lat, "N", "S")} ${toDms(event.latlng.lng, "E", "W")}`;
        element.parentElement?.setAttribute("aria-label", `Map cursor coordinates: ${element.textContent ?? ""}`);
      }
    });
    map.on("click", (event) => {
      if (layerControlOpen) setLayerControlOpen(false);
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
      event.originalEvent.preventDefault();
      const coordinates = formatMapCoordinates(event.latlng.lat, event.latlng.lng, 6);
      void copyTextToClipboard(coordinates).then((copied) => {
        noticeRef.current?.(copied ? "Coordinates copied to clipboard" : "Unable to copy coordinates");
      });
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
        noticeRef.current?.("Temporary pin dropped");
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
      if (layerPreviewUpdateTimer !== null) window.clearTimeout(layerPreviewUpdateTimer);
      layerPreviewMap?.remove();
      markerAnimations.forEach((frameId) => window.cancelAnimationFrame(frameId));
      markerAnimations.clear();
      map.remove();
      mapRef.current = null;
      allTrailsButtonRef.current = null;
      markers.clear();
      trails.clear();
      trailPoints.clear();
      temporaryPins.clear();
    };
  }, []);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    window.setTimeout(() => map.invalidateSize(), 340);
  }, [sidebarOpen]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    // Collar coordinates already come from last-position history. Hub rows may
    // exist before their first fix, so their numeric adapter placeholders aren't locations.
    const locatedDevices = devices.filter(device => (device.entity !== "hub" || device.hasGps) && Number.isFinite(device.lat) && Number.isFinite(device.lon));
    const activeDeviceIds = new Set(locatedDevices.map((device) => device.id));
    markersRef.current.forEach((marker, deviceId) => {
      if (activeDeviceIds.has(deviceId)) return;
      cancelMarkerAnimation(markerAnimationsRef.current, deviceId);
      map.removeLayer(marker);
      markersRef.current.delete(deviceId);
    });
    trailsRef.current.forEach((trail, deviceId) => {
      if (activeDeviceIds.has(deviceId)) return;
      if (map.hasLayer(trail)) map.removeLayer(trail);
      trailsRef.current.delete(deviceId);
      trailPointsRef.current.delete(deviceId);
    });

    locatedDevices.forEach((device) => {
      const avatar = avatars[device.id];
      const markerColor = normalizeMarkerColor(avatar.color);
      const offline = isCollarOffline(device, presenceNow);
      const latLng: TrailLatLng = [device.lat, device.lon];
      let marker = markersRef.current.get(device.id);
      const icon = L.divIcon({
        className: "bp-marker-icon",
        html: markerElement(avatar, markerColor, device.status, offline),
        iconSize: [36, 48],
        iconAnchor: [18, 47],
        popupAnchor: [0, -43],
      });
      if (!marker) {
        marker = L.marker(latLng, { icon }).addTo(map);
        markersRef.current.set(device.id, marker);
      } else {
        marker.setIcon(icon);
        slideMarkerTo(marker, L.latLng(device.lat, device.lon), markerAnimationsRef.current, device.id);
      }
      const popupContent = popupHtml(device, avatar, presenceNow, readOnly, followedId === device.id, trailIds.has(device.id));
      if (marker.getPopup()) marker.setPopupContent(popupContent);
      else marker.bindPopup(popupContent, { className: "device-marker-popup", minWidth: 300, maxWidth: 380 });

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
  }, [avatars, devices, followedId, presenceNow, readOnly, trailIds]);

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
    if (followed && (followed.entity !== "hub" || followed.hasGps)) map.panTo([followed.lat, followed.lon]);
  }, [devices, followedId]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    Object.entries(trailHistory).forEach(([deviceIdValue, historicalPoints]) => {
      const deviceId = Number(deviceIdValue);
      const device = devicesRef.current.find((item) => item.id === deviceId);
      const avatar = avatarsRef.current[deviceId];
      if (!device || (device.entity === "hub" && !device.hasGps) || !avatar || historicalPoints.length === 0) return;

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

function popupHtml(device: TelemetryDevice, avatar: DeviceAvatar, presenceNow: number, readOnly = false, followed = false, trailVisible = false) {
  const name = escapeHtml(device.name);
  const isHub = device.entity === "hub";
  const coordinates = formatMapCoordinates(device.lat, device.lon);
  const mapsUrl = googleMapsUrl(device.lat, device.lon);
  const status = popupStatus(device);
  const profile = popupProfile(device);
  const ageSeconds = Math.max(0, Math.floor((presenceNow - device.lastUpdate) / 1000));
  const offline = isCollarOffline(device, presenceNow);
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
    : `<div class="card-name-row"><span class="card-name">${name}</span><span class="card-status ${status.css}">${status.emoji} ${status.label}</span><span class="card-profile ${profile.css}">${profile.label}</span></div>${faultHtml}<div class="card-indicators"><span class="card-indicator-group">${batteryIndicatorHtml(isHub ? null : device.batt, device.batteryPercent)}</span><span class="card-indicator-group">${signalIndicatorHtml(device, isHub)}</span>${isHub ? "" : `<span class="collar-awake" title="Receive window state is not retained in the map card">💤</span>`}</div><div class="card-indicators card-indicators-row3">${isHub ? "" : homeDistanceHtml(distance)}${lastSeenHtml(formatLastSeen(ageSeconds))}</div>`;
  const offlineNotice = offline ? `<p class="card-offline-notice"><strong>Offline.</strong> These are last-known details from ${formatAge(ageSeconds)} and may no longer be current.</p>` : "";
  return `<div class="popup-content device-card map-device-card${offline ? " offline" : ""} expanded"><div class="card-summary map-popup-summary">${popupAvatarHtml(avatar)}<div class="card-identity">${summary}</div></div><div class="card-detail map-popup-detail">${offlineNotice}<div class="card-grid"><span class="label">${offline ? "Last known coordinates" : "Coordinates"}</span><span class="value"><a class="card-coords card-coords-link" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${coordinates}</a></span>${details}<span class="label">Last report</span><span class="value">${formatAge(ageSeconds)}</span>${source}</div>${actions}</div></div>`;
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

function contextMenuHtml(point: L.LatLng) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu"><div class="map-context-heading">Map location</div>${coordinateActionRow(point)}<div class="map-context-actions"><button type="button" data-location-action="drop-pin" ${locationData}>📍 Drop temporary pin</button><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button></div><p class="map-context-hint">Coordinates were copied when this menu opened.</p></div>`;
}

function temporaryPinPopupHtml(point: L.LatLng, pinId: number) {
  const locationData = locationDataAttributes(point);
  return `<div class="map-context-menu temporary-pin-card"><div class="map-context-heading">Temporary meeting point</div>${coordinateActionRow(point)}<div class="map-context-actions"><button type="button" data-location-action="measure" ${locationData}>↔ Measure from here</button><button type="button" class="danger" data-location-action="remove-pin" data-pin-id="${pinId}">× Remove pin</button></div><p class="map-context-hint">This pin stays only for this browser session.</p></div>`;
}

function coordinateActionRow(point: L.LatLng) {
  const mapsUrl = googleMapsUrl(point.lat, point.lng);
  return `<div class="map-context-coordinate-row"><a class="map-context-coordinates" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open this location in Google Maps">${formatMapCoordinates(point.lat, point.lng, 6)}</a><a class="map-context-icon-action" href="${mapsUrl}" target="_blank" rel="noopener noreferrer" title="Open in Google Maps" aria-label="Open this location in Google Maps in a new tab">${openInNewTabIcon()}</a></div>`;
}

function openInNewTabIcon() {
  return '<svg aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3h7v7"/><path d="M10 14 21 3"/><path d="M21 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5"/></svg>';
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

function markerElement(avatar: DeviceAvatar, markerColor: string, status: TelemetryDevice["status"], offline: boolean) {
  const pin = document.createElement("div");
  pin.className = `marker-pin bp-marker status-${status.toLowerCase()}${offline ? " marker-offline" : ""}`;
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

function slideMarkerTo(marker: L.Marker, target: L.LatLng, animations: Map<number, number>, deviceId: number) {
  cancelMarkerAnimation(animations, deviceId);

  const start = marker.getLatLng();
  const distance = start.distanceTo(target);
  if (
    distance === 0 ||
    distance > MAX_ANIMATED_MARKER_DISTANCE_METRES ||
    window.matchMedia("(prefers-reduced-motion: reduce)").matches
  ) {
    marker.setLatLng(target);
    return;
  }

  const startTime = window.performance.now();
  const tick = (now: number) => {
    const progress = easeOutCubic(Math.min((now - startTime) / MARKER_SLIDE_DURATION_MS, 1));
    marker.setLatLng([
      start.lat + (target.lat - start.lat) * progress,
      start.lng + (target.lng - start.lng) * progress,
    ]);

    if (progress < 1) {
      animations.set(deviceId, window.requestAnimationFrame(tick));
      return;
    }

    marker.setLatLng(target);
    animations.delete(deviceId);
  };

  animations.set(deviceId, window.requestAnimationFrame(tick));
}

function cancelMarkerAnimation(animations: Map<number, number>, deviceId: number) {
  const frameId = animations.get(deviceId);
  if (frameId === undefined) return;
  window.cancelAnimationFrame(frameId);
  animations.delete(deviceId);
}

function easeOutCubic(value: number) {
  return 1 - (1 - value) ** 3;
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

function isMapLayerName(value: unknown): value is MapLayerName {
  return typeof value === "string" && value in MAP_LAYER_DEFINITIONS;
}

function escapeHtml(value: string) {
  return value.replace(/[&<>'"]/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;" })[character] ?? character);
}

async function copyTextToClipboard(value: string) {
  // Keep the synchronous path inside the user's click gesture. Some browsers
  // reject the async Clipboard API while still allowing the legacy copy action.
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
