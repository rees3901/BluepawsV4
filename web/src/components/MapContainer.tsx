"use client";

import dynamic from "next/dynamic";
import { useState } from "react";
import { MAP_LAYER_PICKER_NAMES, type MapLayerPickerName } from "@/lib/mapLayers";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import type { MapRendererName, MapRendererProps, MapViewport, VectorSourceName } from "@/components/mapRenderer";

const LeafletMap = dynamic(() => import("@/components/TrackingMap"), { ssr: false });
const MapLibreMap = dynamic(() => import("@/components/MapLibreMap"), { ssr: false });
const MapStylePreview = dynamic(() => import("@/components/MapStylePreview"), { ssr: false });
const STORAGE_KEY = "bluepaws-map-renderer";
const RASTER_KEY = "bluepaws-raster-layer";
const VECTOR_KEY = "bluepaws-vector-source";

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
  const [viewport, setViewport] = useState<MapViewport>({ latitude: EMPTY_MAP_CENTER[0], longitude: EMPTY_MAP_CENTER[1], zoom: EMPTY_MAP_ZOOM });

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
      {renderer === "leaflet"
        ? <LeafletMap {...props} rasterLayer={rasterLayer} vectorSource={vectorSource} onViewportChange={setViewport} />
        : <MapLibreMap {...props} rasterLayer={rasterLayer} vectorSource={vectorSource} onViewportChange={setViewport} />}
      <div className={`map-style-picker${pickerOpen ? " open" : ""}`} data-tour="map-layers">
        <button type="button" className="map-style-preview" aria-label="Choose map style" aria-expanded={pickerOpen} onClick={() => setPickerOpen(open => !open)}>
          <MapStylePreview layer={previewLayer} viewport={viewport} />
          <span>{RASTER_LABELS[previewLayer]} overview · {selectedStyle}</span>
        </button>
        {pickerOpen && <div className="map-style-menu">
          <div className="map-style-options" aria-label="Map styles">
            {MAP_LAYER_PICKER_NAMES.map(name => <button type="button" key={name} className={renderer === "leaflet" && rasterLayer === name ? "active" : ""} onClick={() => chooseRaster(name)}>{RASTER_LABELS[name]}</button>)}
            <button type="button" className={renderer === "maplibre" ? "active" : ""} onClick={() => chooseRenderer("maplibre")}>Vector</button>
          </div>
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
