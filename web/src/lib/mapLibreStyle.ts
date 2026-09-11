import { layers, namedFlavor } from "@protomaps/basemaps";
import type { StyleSpecification } from "maplibre-gl";

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
