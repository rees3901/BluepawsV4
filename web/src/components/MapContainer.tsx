"use client";

import dynamic from "next/dynamic";
import { useState } from "react";
import { MAP_LAYER_PICKER_NAMES, type MapLayerPickerName } from "@/lib/mapLayers";
import type { MapRendererName, MapRendererProps, VectorSourceName } from "@/components/mapRenderer";

const LeafletMap = dynamic(() => import("@/components/TrackingMap"), { ssr: false });
const MapLibreMap = dynamic(() => import("@/components/MapLibreMap"), { ssr: false });
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
  const [vectorSource, setVectorSource] = useState<VectorSourceName>(() =>
    typeof window !== "undefined" && window.localStorage.getItem(VECTOR_KEY) === "pmtiles" ? "pmtiles" : "online");
  const [pickerOpen, setPickerOpen] = useState(false);
  const pmtilesAvailable = Boolean(process.env.NEXT_PUBLIC_BLUEPAWS_PMTILES_URL);

  const chooseRenderer = (next: MapRendererName) => {
    setRenderer(next);
    window.localStorage.setItem(STORAGE_KEY, next);
  };

  const chooseRaster = (next: MapLayerPickerName) => {
    setRasterLayer(next);
    window.localStorage.setItem(RASTER_KEY, next);
  };

  const chooseVector = (next: VectorSourceName) => {
    setVectorSource(next);
    window.localStorage.setItem(VECTOR_KEY, next);
  };

  return (
    <div className="map-renderer-shell">
      {renderer === "leaflet"
        ? <LeafletMap {...props} rasterLayer={rasterLayer} vectorSource={vectorSource} />
        : <MapLibreMap {...props} rasterLayer={rasterLayer} vectorSource={vectorSource} />}
      <div className={`map-style-picker${pickerOpen ? " open" : ""}`} data-tour="map-layers">
        <button type="button" className="map-style-preview" aria-label="Choose raster or vector map style" aria-expanded={pickerOpen} onClick={() => setPickerOpen(open => !open)}>
          <span className={`map-style-preview-image ${renderer === "leaflet" ? `raster-${rasterLayer.toLowerCase()}` : "vector-online"}`} />
          <span>{renderer === "leaflet" ? RASTER_LABELS[rasterLayer] : vectorSource === "pmtiles" ? "PMTiles" : "Vector"}</span>
        </button>
        {pickerOpen && <div className="map-style-menu">
          <div className="map-style-tabs" role="tablist" aria-label="Map format">
            <button type="button" role="tab" aria-selected={renderer === "leaflet"} className={renderer === "leaflet" ? "active" : ""} onClick={() => chooseRenderer("leaflet")}>Raster</button>
            <button type="button" role="tab" aria-selected={renderer === "maplibre"} className={renderer === "maplibre" ? "active" : ""} onClick={() => chooseRenderer("maplibre")}>Vector</button>
          </div>
          <div className="map-style-options" role="tabpanel">
            {renderer === "leaflet" ? MAP_LAYER_PICKER_NAMES.map(name => <button type="button" key={name} className={rasterLayer === name ? "active" : ""} onClick={() => chooseRaster(name)}>{RASTER_LABELS[name]}</button>) : <>
              <button type="button" className={vectorSource === "online" ? "active" : ""} onClick={() => chooseVector("online")}>Online vector</button>
              <button type="button" disabled={!pmtilesAvailable} className={vectorSource === "pmtiles" ? "active" : ""} title={pmtilesAvailable ? "Use the configured PMTiles archive" : "Set NEXT_PUBLIC_BLUEPAWS_PMTILES_URL to enable"} onClick={() => chooseVector("pmtiles")}>PMTiles</button>
            </>}
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
