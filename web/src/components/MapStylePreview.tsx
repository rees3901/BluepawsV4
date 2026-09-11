"use client";

import L from "leaflet";
import { useEffect, useRef } from "react";
import { MAP_LAYER_DEFINITIONS, previewMapZoom, type MapLayerPickerName } from "@/lib/mapLayers";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "@/lib/mapViewport";
import type { MapViewport } from "@/components/mapRenderer";

interface MapStylePreviewProps {
  layer: MapLayerPickerName;
  viewport: MapViewport;
}

export default function MapStylePreview({ layer, viewport }: MapStylePreviewProps) {
  const containerRef = useRef<HTMLSpanElement | null>(null);
  const mapRef = useRef<L.Map | null>(null);
  const layerRef = useRef<L.TileLayer | null>(null);

  useEffect(() => {
    if (!containerRef.current) return;
    const map = L.map(containerRef.current, {
      attributionControl: false,
      boxZoom: false,
      center: [...EMPTY_MAP_CENTER],
      doubleClickZoom: false,
      dragging: false,
      keyboard: false,
      scrollWheelZoom: false,
      touchZoom: false,
      zoom: EMPTY_MAP_ZOOM,
      zoomControl: false,
    });
    mapRef.current = map;
    return () => {
      map.remove();
      mapRef.current = null;
      layerRef.current = null;
    };
  }, []);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    layerRef.current?.removeFrom(map);
    const definition = MAP_LAYER_DEFINITIONS[layer];
    layerRef.current = L.tileLayer(definition.url, {
      attribution: definition.attribution,
      maxNativeZoom: definition.maxNativeZoom,
      maxZoom: definition.maxZoom,
    }).addTo(map);
  }, [layer]);

  useEffect(() => {
    const map = mapRef.current;
    if (!map) return;
    map.setView([viewport.latitude, viewport.longitude], previewMapZoom(viewport.zoom, layer), { animate: false });
    window.setTimeout(() => map.invalidateSize(), 0);
  }, [layer, viewport]);

  return <span ref={containerRef} className="map-style-preview-image map-style-preview-live" aria-hidden="true" />;
}
