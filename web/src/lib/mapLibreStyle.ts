import { layers, namedFlavor } from "@protomaps/basemaps";
import type { StyleSpecification } from "maplibre-gl";
import { MAP_LAYER_DEFINITIONS, type MapLayerPickerName } from "./mapLayers.ts";

export function mapLibreRasterStyle(layer: MapLayerPickerName): StyleSpecification {
  const definition = MAP_LAYER_DEFINITIONS[layer];
  const tiles = definition.url.includes("{s}")
    ? ["a", "b", "c"].map(subdomain => definition.url.replace("{s}", subdomain))
    : [definition.url];
  return {
    version: 8,
    sources: { basemap: { type: "raster", tiles, tileSize: 256, maxzoom: definition.maxNativeZoom, attribution: definition.attribution } },
    layers: [{ id: "basemap", type: "raster", source: "basemap" }],
  };
}

export const ONLINE_VECTOR_STYLE = "https://tiles.openfreemap.org/styles/liberty";

export function mapLibreStyle(pmtilesUrl?: string): string | StyleSpecification {
  if (!pmtilesUrl) return ONLINE_VECTOR_STYLE;
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
