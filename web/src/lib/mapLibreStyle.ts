import { layers, namedFlavor } from "@protomaps/basemaps";
import type { StyleSpecification } from "maplibre-gl";

export const ONLINE_VECTOR_SOURCE = "https://tiles.openfreemap.org/planet";

export function mapLibreStyle(pmtilesUrl?: string): string | StyleSpecification {
  if (!pmtilesUrl) return onlineVectorStyle();
  return {
    version: 8,
    glyphs: "https://protomaps.github.io/basemaps-assets/fonts/{fontstack}/{range}.pbf",
    sources: {
      protomaps: {
        type: "vector",
        url: `pmtiles://${pmtilesUrl}`,
        attribution: "© OpenStreetMap contributors",
      },
    },
    layers: layers("protomaps", namedFlavor("light"), { lang: "en" }),
  } as StyleSpecification;
}

function onlineVectorStyle(): StyleSpecification {
  return {
    version: 8,
    glyphs: "https://tiles.openfreemap.org/fonts/{fontstack}/{range}.pbf",
    sources: {
      openmaptiles: { type: "vector", url: ONLINE_VECTOR_SOURCE, attribution: "© OpenStreetMap contributors" },
    },
    layers: [
      { id: "background", type: "background", paint: { "background-color": "#eef1e8" } },
      { id: "landcover", type: "fill", source: "openmaptiles", "source-layer": "landcover", paint: { "fill-color": "#dce8cf", "fill-opacity": 0.7 } },
      { id: "landuse", type: "fill", source: "openmaptiles", "source-layer": "landuse", paint: { "fill-color": "#e7eadb", "fill-opacity": 0.65 } },
      { id: "water", type: "fill", source: "openmaptiles", "source-layer": "water", paint: { "fill-color": "#9fd5e5" } },
      { id: "buildings", type: "fill", source: "openmaptiles", "source-layer": "building", minzoom: 12, paint: { "fill-color": "#d5cec3", "fill-outline-color": "#c4baad" } },
      { id: "roads", type: "line", source: "openmaptiles", "source-layer": "transportation", paint: { "line-color": "#ffffff", "line-width": ["interpolate", ["linear"], ["zoom"], 5, 0.5, 12, 1.5, 17, 5] } },
      { id: "boundaries", type: "line", source: "openmaptiles", "source-layer": "boundary", paint: { "line-color": "#9aa89b", "line-width": 1, "line-dasharray": [3, 2] } },
      { id: "places", type: "symbol", source: "openmaptiles", "source-layer": "place", layout: { "text-field": ["coalesce", ["get", "name:en"], ["get", "name"]], "text-font": ["Noto Sans Regular"], "text-size": ["interpolate", ["linear"], ["zoom"], 5, 10, 14, 14] }, paint: { "text-color": "#52605a", "text-halo-color": "#f5f3ed", "text-halo-width": 1 } },
    ],
  } as StyleSpecification;
}
