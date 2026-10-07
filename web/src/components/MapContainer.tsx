"use client";

import dynamic from "next/dynamic";
import { useEffect, useRef, useState } from "react";
import { MAP_LAYER_PICKER_NAMES, type MapLayerPickerName } from "@/lib/mapLayers";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import type { MapRendererName, MapRendererProps, MapViewport, VectorSourceName } from "@/components/mapRenderer";

const LeafletMap = dynamic(() => import("@/components/TrackingMap"), { ssr: false });
const MapLibreMap = dynamic(() => import("@/components/MapLibreMap"), { ssr: false });
const MapStylePreview = dynamic(() => import("@/components/MapStylePreview"), { ssr: false });
const STORAGE_KEY = "bluepaws-map-renderer";
const RASTER_KEY = "bluepaws-raster-layer";
const VECTOR_KEY = "bluepaws-vector-source";
const ROTATION_KEY = "bluepaws-raster-rotation";
const PICKER_IDLE_MS = 20_000;

export default function MapContainer(props: MapRendererProps) {
  const [renderer, setRenderer] = useState<MapRendererName>(() => {
    if (typeof window === "undefined") return "leaflet";
    const saved = window.localStorage.getItem(STORAGE_KEY);
    return saved === "maplibre" ? "maplibre" : "leaflet";
  });
  const [rasterLayer, setRasterLayer] = useState<MapLayerPickerName>(() => {
    const saved = typeof window === "undefined" ? null : window.localStorage.getItem(RASTER_KEY);
    return MAP_LAYER_PICKER_NAMES.includes(saved as MapLayerPickerName) ? saved as MapLayerPickerName : "Street";
  });
  const [vectorSource] = useState<VectorSourceName>(() =>
    typeof window !== "undefined" && window.localStorage.getItem(VECTOR_KEY) === "pmtiles" ? "pmtiles" : "online");
  const [pickerOpen, setPickerOpen] = useState(false);
  const [rotateRaster, setRotateRaster] = useState(() => typeof window !== "undefined" && window.localStorage.getItem(ROTATION_KEY) === "true");
  const pickerRef = useRef<HTMLDivElement>(null);
  const [viewport, setViewport] = useState<MapViewport>({ latitude: EMPTY_MAP_CENTER[0], longitude: EMPTY_MAP_CENTER[1], zoom: EMPTY_MAP_ZOOM });

  useEffect(() => {
    const picker = pickerRef.current;
    if (!pickerOpen || !picker) return;
    let timer: number | undefined;
    let hovered = window.matchMedia("(hover: hover)").matches && picker.matches(":hover");
    const clearTimer = () => window.clearTimeout(timer);
    const restartTimer = () => {
      clearTimer();
      if (!hovered) timer = window.setTimeout(() => setPickerOpen(false), PICKER_IDLE_MS);
    };
    const enter = (event: PointerEvent) => {
      if (event.pointerType === "touch") return;
      hovered = true;
      clearTimer();
    };
    const leave = () => { hovered = false; restartTimer(); };
    const keyboard = (event: KeyboardEvent) => {
      if (event.key === "Escape") setPickerOpen(false);
      else restartTimer();
    };
    picker.addEventListener("pointerenter", enter);
    picker.addEventListener("pointerleave", leave);
    picker.addEventListener("pointerdown", restartTimer);
    picker.addEventListener("focusin", restartTimer);
    picker.addEventListener("keydown", keyboard);
    restartTimer();
    return () => {
      clearTimer();
      picker.removeEventListener("pointerenter", enter);
      picker.removeEventListener("pointerleave", leave);
      picker.removeEventListener("pointerdown", restartTimer);
      picker.removeEventListener("focusin", restartTimer);
      picker.removeEventListener("keydown", keyboard);
    };
  }, [pickerOpen]);

  const chooseRenderer = (next: MapRendererName) => {
    setRenderer(next);
    window.localStorage.setItem(STORAGE_KEY, next);
  };

  const chooseRaster = (next: MapLayerPickerName) => {
    setRasterLayer(next);
    chooseRenderer("leaflet");
    window.localStorage.setItem(RASTER_KEY, next);
  };

  const previewLayer: MapLayerPickerName = renderer === "leaflet" && rasterLayer === "Satellite" ? "Street" : "Satellite";
  const selectedStyle = renderer === "maplibre" ? "Vector" : RASTER_LABELS[rasterLayer];

  return (
    <div className="map-renderer-shell">
      {renderer === "leaflet" && !rotateRaster
        ? <LeafletMap {...props} rasterLayer={rasterLayer} vectorSource={vectorSource} onViewportChange={setViewport} />
        : <MapLibreMap key={selectedStyle} {...props} initialViewport={viewport} mapStyle={renderer === "maplibre" ? "Vector" : rasterLayer} rasterLayer={rasterLayer} vectorSource={vectorSource} onViewportChange={setViewport} />}
      <div ref={pickerRef} className={`map-style-picker${pickerOpen ? " open" : ""}`} data-tour="map-layers">
        <button type="button" className="map-style-preview" aria-label="Choose map style" aria-expanded={pickerOpen} onClick={() => setPickerOpen(open => !open)}>
          <MapStylePreview layer={previewLayer} viewport={viewport} />
          <span>{RASTER_LABELS[previewLayer]} overview · {selectedStyle}</span>
        </button>
        {pickerOpen && <div className="map-style-menu">
          <div className="map-style-options" aria-label="Map styles">
            {MAP_LAYER_PICKER_NAMES.map(name => <button type="button" key={name} className={renderer === "leaflet" && rasterLayer === name ? "active" : ""} onClick={() => chooseRaster(name)}><span className="map-style-option-title">{RASTER_LABELS[name]}</span><span className="map-style-option-description">{RASTER_DESCRIPTIONS[name]}</span></button>)}
            <button type="button" className={renderer === "maplibre" ? "active" : ""} onClick={() => chooseRenderer("maplibre")}><span className="map-style-option-title">Vector</span><span className="map-style-option-description">Smooth map with rotate and tilt controls.</span></button>
          </div>
          {renderer === "leaflet" && <label className="map-rotation-option" title="Right-drag or use two fingers to rotate and tilt. Switch off to use the original map."><input type="checkbox" checked={rotateRaster} onChange={event => { setRotateRaster(event.target.checked); window.localStorage.setItem(ROTATION_KEY, String(event.target.checked)); }} />Allow rotation and tilt</label>}
        </div>}
      </div>
    </div>
  );
}

const RASTER_LABELS: Record<MapLayerPickerName, string> = {
  Street: "Street",
  Satellite: "Satellite",
  Topographic: "Topographic",
};

const RASTER_DESCRIPTIONS: Record<MapLayerPickerName, string> = {
  Street: "Clear roads, paths and place names.",
  Satellite: "Aerial imagery for matching landmarks.",
  Topographic: "Contours and terrain detail for reading relief.",
};
