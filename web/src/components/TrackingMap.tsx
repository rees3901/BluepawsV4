"use client";

import L from "leaflet";
import { useEffect, useRef } from "react";
import { emojiImageUrl } from "@/lib/emoji";
import { COLLAR_RECEIVE_WINDOW_SECONDS, collarCardFreshness, collarFreshnessClass, type CollarCardFreshness } from "@/lib/devicePresence";
import { formatMapCoordinates } from "@/lib/mapLocation";
import { contextMenuHtml, copyTextToClipboard, temporaryPinPopupHtml } from "@/lib/mapLocationPopup";
import { MAP_LAYER_DEFINITIONS, type MapLayerName } from "@/lib/mapLayers";
import { mapPopupHtml } from "@/lib/mapPopup";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import { normalizeMarkerColor } from "@/lib/markerColor";
import { appendTrailPoint, VISIBLE_TRAIL_POINT_LIMIT, type TrailLatLng } from "@/lib/trailPoints";
import { locatedDevices, type ConfiguredMapRendererProps } from "@/components/mapRenderer";
import {
  type DeviceAction,
  type DeviceAvatar,
  type TelemetryDevice,
} from "@/types/telemetry";

const JUMP_TO_ZOOM = 17;
const MARKER_SLIDE_DURATION_MS = 750;
const MAX_ANIMATED_MARKER_DISTANCE_METRES = 2_000;

export default function LeafletMap(props: ConfiguredMapRendererProps) {
  const { devices, avatars, presenceNow, sidebarOpen, followedId, trailIds, trailHistory, rasterLayer, allTrailsVisible = false, trailsAvailable = false, command, onAction, onAllTrailsToggle, onNotice, readOnly = false } = props;
  const mapRef = useRef<L.Map | null>(null);
  const baseLayerRef = useRef<L.TileLayer | null>(null);
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
  const viewportChangeRef = useRef(props.onViewportChange);
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
    viewportChangeRef.current = props.onViewportChange;
    const trailButton = allTrailsButtonRef.current;
    if (trailButton) {
      const label = allTrailsVisible ? "Hide all breadcrumb trails" : "Show all breadcrumb trails";
      trailButton.classList.toggle("active", allTrailsVisible);
      trailButton.disabled = !trailsAvailable;
      trailButton.title = label;
      trailButton.setAttribute("aria-label", label);
      trailButton.setAttribute("aria-pressed", String(allTrailsVisible));
    }
  }, [allTrailsVisible, avatars, devices, onAction, onAllTrailsToggle, onNotice, props.onViewportChange, trailIds, trailsAvailable]);

  useEffect(() => {
    const markers = markersRef.current;
    const markerAnimations = markerAnimationsRef.current;
    const trails = trailsRef.current;
    const trailPoints = trailPointsRef.current;
    const map = L.map("map", { center: [...EMPTY_MAP_CENTER], zoom: EMPTY_MAP_ZOOM, zoomControl: false, tapHold: true });
    mapRef.current = map;
    const reportViewport = () => {
      const center = map.getCenter();
      viewportChangeRef.current?.({ latitude: center.lat, longitude: center.lng, zoom: map.getZoom() });
    };
    map.on("moveend zoomend", reportViewport);
    reportViewport();

    const createTileLayer = (name: MapLayerName) => {
      const definition = MAP_LAYER_DEFINITIONS[name];
      return L.tileLayer(definition.url, {
        attribution: definition.attribution,
        maxNativeZoom: definition.maxNativeZoom,
        maxZoom: definition.maxZoom,
      });
    };
    baseLayerRef.current = createTileLayer("Street").addTo(map);
    L.control.zoom({ position: "bottomleft" }).addTo(map);

    const HomeControl = L.Control.extend({
      options: { position: "topleft" },
      onAdd() {
        const button = L.DomUtil.create("button", "leaflet-map-btn") as HTMLButtonElement;
        button.type = "button";
        button.title = "Center on Home Hub";
        button.setAttribute("aria-label", "Center map on Home Hub");
        button.setAttribute("data-tour", "map-home");
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
        button.setAttribute("data-tour", "map-fit");
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
        button.setAttribute("data-tour", "map-trails");
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
        button.setAttribute("aria-label", "Measure distance on the map");
        button.setAttribute("data-tour", "map-measure");
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
      markerAnimations.forEach((frameId) => window.cancelAnimationFrame(frameId));
      markerAnimations.clear();
      map.remove();
      mapRef.current = null;
      allTrailsButtonRef.current = null;
      markers.clear();
      trails.clear();
      trailPoints.clear();
      temporaryPins.clear();
      baseLayerRef.current = null;
    };
  }, []);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    baseLayerRef.current?.removeFrom(map);
    const definition = MAP_LAYER_DEFINITIONS[rasterLayer];
    baseLayerRef.current = L.tileLayer(definition.url, {
      attribution: definition.attribution,
      maxNativeZoom: definition.maxNativeZoom,
      maxZoom: definition.maxZoom,
    }).addTo(map);
  }, [rasterLayer]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    const resizeTimer = window.setTimeout(() => {
      if (mapRef.current === map) map.invalidateSize();
    }, 340);
    return () => window.clearTimeout(resizeTimer);
  }, [sidebarOpen]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;

    // Collar coordinates already come from last-position history. Hub rows may
    // exist before their first fix, so their numeric adapter placeholders aren't locations.
    const visibleDevices = locatedDevices(devices);
    const activeDeviceIds = new Set(visibleDevices.map((device) => device.id));
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

    visibleDevices.forEach((device) => {
      const avatar = avatars[device.id];
      const markerColor = normalizeMarkerColor(avatar.color);
      const ageSeconds = Math.max(0, Math.floor((presenceNow - device.lastUpdate) / 1000));
      const freshness = device.entity === "hub" ? null : collarCardFreshness(ageSeconds, ageSeconds < COLLAR_RECEIVE_WINDOW_SECONDS);
      const latLng: TrailLatLng = [device.lat, device.lon];
      let marker = markersRef.current.get(device.id);
      const icon = L.divIcon({
        className: "bp-marker-icon",
        html: markerElement(avatar, markerColor, device.status, freshness),
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
      const popupContent = mapPopupHtml(device, avatar, presenceNow, readOnly, followedId === device.id, trailIds.has(device.id));
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
    if (command.type === "open" && command.deviceId !== undefined) {
      const marker = markersRef.current.get(command.deviceId);
      if (marker) {
        map.setView(marker.getLatLng(), Math.max(map.getZoom(), JUMP_TO_ZOOM), { animate: true });
        marker.openPopup();
      }
    }
  }, [command]);

  return <div id="map" aria-label="Live animal tracking map" />;
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

function markerElement(avatar: DeviceAvatar, markerColor: string, status: TelemetryDevice["status"], freshness: CollarCardFreshness | null) {
  const pin = document.createElement("div");
  const freshnessClass = collarFreshnessClass(freshness);
  pin.className = `marker-pin bp-marker status-${status.toLowerCase()}${freshnessClass ? ` ${freshnessClass}` : ""}`;
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
