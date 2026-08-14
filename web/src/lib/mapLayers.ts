export type MapLayerName =
  | "Street"
  | "Satellite"
  | "Satellite HD"
  | "Topographic"
  | "Humanitarian"
  | "Esri Topo";

export interface MapLayerDefinition {
  url: string;
  attribution: string;
  maxNativeZoom: number;
  maxZoom: number;
}

const STANDARD_DISPLAY_MAX_ZOOM = 22;
const TOPOGRAPHIC_DISPLAY_MAX_ZOOM = 20;

export const MAP_LAYER_DEFINITIONS: Record<MapLayerName, MapLayerDefinition> = {
  Street: {
    url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    attribution: "&copy; OpenStreetMap",
    maxNativeZoom: 19,
    maxZoom: STANDARD_DISPLAY_MAX_ZOOM,
  },
  Satellite: {
    url: "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    attribution: "&copy; Esri World Imagery",
    maxNativeZoom: 19,
    maxZoom: 19,
  },
  "Satellite HD": {
    url: "https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    attribution: "&copy; Esri Clarity",
    maxNativeZoom: 18,
    maxZoom: 18,
  },
  Topographic: {
    url: "https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png",
    attribution: "&copy; OpenTopoMap",
    maxNativeZoom: 17,
    maxZoom: TOPOGRAPHIC_DISPLAY_MAX_ZOOM,
  },
  Humanitarian: {
    url: "https://{s}.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png",
    attribution: "&copy; OpenStreetMap, Tiles: HOT",
    maxNativeZoom: 19,
    maxZoom: STANDARD_DISPLAY_MAX_ZOOM,
  },
  "Esri Topo": {
    url: "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}",
    attribution: "&copy; Esri",
    maxNativeZoom: 18,
    maxZoom: 18,
  },
};
