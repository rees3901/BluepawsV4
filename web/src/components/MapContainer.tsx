"use client";

import dynamic from "next/dynamic";
import { useState } from "react";
import type { MapRendererName, MapRendererProps } from "@/components/mapRenderer";

const LeafletMap = dynamic(() => import("@/components/TrackingMap"), { ssr: false });
const MapLibreMap = dynamic(() => import("@/components/MapLibreMap"), { ssr: false });
const STORAGE_KEY = "bluepaws-map-renderer";

export default function MapContainer(props: MapRendererProps) {
  const [renderer, setRenderer] = useState<MapRendererName>(() => {
    if (typeof window === "undefined") return "leaflet";
    const saved = window.localStorage.getItem(STORAGE_KEY);
    return saved === "maplibre" ? "maplibre" : "leaflet";
  });

  const chooseRenderer = (next: MapRendererName) => {
    setRenderer(next);
    window.localStorage.setItem(STORAGE_KEY, next);
  };

  return (
    <div className="map-renderer-shell">
      {renderer === "leaflet" ? <LeafletMap {...props} /> : <MapLibreMap {...props} />}
      <div className="map-renderer-toggle" role="group" aria-label="Map renderer">
        <button type="button" className={renderer === "leaflet" ? "active" : ""} aria-pressed={renderer === "leaflet"} onClick={() => chooseRenderer("leaflet")}>Classic</button>
        <button type="button" className={renderer === "maplibre" ? "active" : ""} aria-pressed={renderer === "maplibre"} onClick={() => chooseRenderer("maplibre")}>Vector</button>
      </div>
    </div>
  );
}
