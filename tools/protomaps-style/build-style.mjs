import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { layers, namedFlavor } from "@protomaps/basemaps";

function argument(name, fallback = undefined) {
  const prefix = `--${name}=`;
  const value = process.argv.slice(2).find((item) => item.startsWith(prefix));
  return value ? value.slice(prefix.length) : fallback;
}

const output = argument("output");
if (!output) {
  throw new Error("Usage: node build-style.mjs --output=FILE [--tile-url=URL] [--flavor=light]");
}

const sourceName = "protomaps";
const tileUrl = argument(
  "tile-url",
  "http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt",
);
const flavor = argument("flavor", "light");
const baseFlavor = flavor === "bluepaws-carto" || flavor === "bluepaws-road" ? "light" : flavor;

const majorRoadColor = [
  "match",
  ["get", "kind_detail"],
  "trunk",
  "#f9b29c",
  "primary",
  "#fcd6a4",
  "secondary",
  "#f7fabf",
  "tertiary",
  "#ffffff",
  "#ffffff",
];
const majorRoadCasing = [
  "match",
  ["get", "kind_detail"],
  "trunk",
  "#c98578",
  "primary",
  "#c8a477",
  "secondary",
  "#b9b77f",
  "tertiary",
  "#aaa8a4",
  "#aaa8a4",
];

function bluePawsCartoLayer(layer) {
  const result = {
    ...layer,
    paint: layer.paint ? { ...layer.paint } : undefined,
    layout: layer.layout ? { ...layer.layout } : undefined,
  };
  const paint = result.paint ?? {};

  const exactPaint = {
    background: { "background-color": "#c7d7df" },
    earth: { "fill-color": "#f2efe9" },
    landcover: {
      "fill-color": [
        "match",
        ["get", "kind"],
        "grassland", "#cde7bd",
        "barren", "#ead9b8",
        "urban_area", "#dedbd7",
        "farmland", "#dce8bd",
        "glacier", "#ffffff",
        "scrub", "#c7dda7",
        "#b9d7ad",
      ],
      "fill-opacity": paint["fill-opacity"],
    },
    landuse_park: {
      "fill-opacity": paint["fill-opacity"],
      "fill-color": [
        "case",
        ["in", ["get", "kind"], ["literal", ["forest", "wood"]]], "#add19e",
        ["in", ["get", "kind"], ["literal", ["park", "nature_reserve", "protected_area", "national_park"]]], "#b8dfaa",
        ["in", ["get", "kind"], ["literal", ["grass", "grassland", "golf_course"]]], "#c9e6b5",
        ["==", ["get", "kind"], "sand"], "#f3e4b6",
        ["in", ["get", "kind"], ["literal", ["military", "naval_base", "airfield"]]], "#e3c9c9",
        "#c7dda7",
      ],
    },
    landuse_urban_green: { "fill-color": "#b8dfaa", "fill-opacity": 0.85 },
    landuse_hospital: { "fill-color": "#f1dada" },
    landuse_industrial: { "fill-color": "#d9d0db" },
    landuse_school: { "fill-color": "#eee0c2" },
    landuse_beach: { "fill-color": "#f3e4b6" },
    landuse_zoo: { "fill-color": "#c7dda7" },
    landuse_aerodrome: { "fill-color": "#dddde3" },
    landuse_pedestrian: { "fill-color": "#e7e1dc" },
    landuse_pier: { "fill-color": "#f2efe9" },
    water: { "fill-color": "#aad3df" },
    water_stream: { "line-color": "#79b9d1", "line-width": paint["line-width"] },
    water_river: { "line-color": "#79b9d1", "line-width": paint["line-width"] },
    buildings: {
      "fill-color": "#d6c5b9",
      "fill-opacity": 0.9,
      "fill-outline-color": "#b8a79b",
    },
  };
  if (exactPaint[result.id]) {
    result.paint = exactPaint[result.id];
  }

  if (result.id.includes("_highway_casing")) {
    result.paint["line-color"] = "#c86175";
  } else if (result.id.includes("_major_casing")) {
    result.paint["line-color"] = majorRoadCasing;
  } else if (result.id.includes("_minor_casing") || result.id.includes("_link_casing")) {
    result.paint["line-color"] = "#aaa8a4";
  } else if (result.id.includes("_other_casing")) {
    result.paint["line-color"] = "#9c8d78";
  }

  if (result.id.match(/^roads_(tunnels_|bridges_)?highway$/)) {
    result.paint["line-color"] = "#e892a2";
  } else if (result.id.match(/^roads_(tunnels_|bridges_)?major$/)) {
    result.paint["line-color"] = majorRoadColor;
  } else if (result.id.match(/^roads_(tunnels_|bridges_)?minor$/)) {
    result.paint["line-color"] = "#ffffff";
  } else if (result.id.match(/^roads_(tunnels_|bridges_)?link$/)) {
    result.paint["line-color"] = [
      "case",
      ["==", ["get", "kind"], "highway"], "#e892a2",
      ["==", ["get", "kind"], "major_road"], majorRoadColor,
      "#ffffff",
    ];
  } else if (result.id === "roads_minor_service") {
    result.paint["line-color"] = "#f7f4ef";
  } else if (result.id.match(/^roads_(tunnels_|bridges_)?other$/)) {
    result.paint["line-color"] = [
      "case",
      ["==", ["get", "kind"], "path"], "#a77b5d",
      "#d6d1ca",
    ];
  }

  if (result.type === "symbol") {
    result.paint = {
      ...result.paint,
      "text-color": result.id === "pois" ? paint["text-color"] : "#30343b",
      "text-halo-color": "#f7f4ef",
      "text-halo-width": 1.5,
    };
    if (result.id === "roads_labels_minor") {
      result.layout["text-size"] = 13;
    } else if (result.id === "roads_labels_major") {
      result.layout["text-size"] = 14;
    }
  }

  if (result.id === "roads_rail") {
    result.paint["line-color"] = "#55585e";
    result.paint["line-opacity"] = 0.8;
  }
  if (result.id.startsWith("boundaries")) {
    result.paint["line-color"] = "#8f6ca8";
  }
  return result;
}

function bluePawsRoadLayer(layer) {
  const result = bluePawsCartoLayer(layer);
  const paint = result.paint ?? {};
  const exactPaint = {
    background: { "background-color": "#AFC1C8" },
    earth: { "fill-color": "#DED9CD" },
    landcover: {
      "fill-color": [
        "match", ["get", "kind"],
        "grassland", "#ABD18D",
        "barren", "#D8C49A",
        "urban_area", "#CFCAC0",
        "farmland", "#C6D69C",
        "glacier", "#F8F8F4",
        "scrub", "#A8C887",
        "#9FC384",
      ],
      "fill-opacity": paint["fill-opacity"],
    },
    landuse_park: {
      "fill-opacity": paint["fill-opacity"],
      "fill-color": [
        "case",
        ["in", ["get", "kind"], ["literal", ["forest", "wood"]]], "#7FB875",
        ["in", ["get", "kind"], ["literal", ["park", "nature_reserve", "protected_area", "national_park"]]], "#91C782",
        ["in", ["get", "kind"], ["literal", ["grass", "grassland", "golf_course"]]], "#A9D18B",
        ["==", ["get", "kind"], "sand"], "#E1C98F",
        "#9CC77F",
      ],
    },
    landuse_urban_green: { "fill-color": "#91C782", "fill-opacity": 0.9 },
    landuse_hospital: { "fill-color": "#E6BFC0" },
    landuse_industrial: { "fill-color": "#C7BCCB" },
    landuse_school: { "fill-color": "#E1C995" },
    landuse_beach: { "fill-color": "#E1C98F" },
    landuse_zoo: { "fill-color": "#9CC77F" },
    landuse_aerodrome: { "fill-color": "#C4C5CB" },
    landuse_pedestrian: { "fill-color": "#D4CEC4" },
    landuse_pier: { "fill-color": "#DED9CD" },
    water: { "fill-color": "#72B5D0" },
    water_stream: { "line-color": "#428EAF", "line-width": paint["line-width"] },
    water_river: { "line-color": "#428EAF", "line-width": paint["line-width"] },
    buildings: {
      "fill-color": "#B9A897",
      "fill-opacity": 1,
      "fill-outline-color": "#786A5D",
    },
  };
  if (exactPaint[result.id]) {
    result.paint = exactPaint[result.id];
  }

  if (result.type === "symbol") {
    result.paint = {
      ...result.paint,
      "text-color": "#17232B",
      "text-halo-color": "#F3EFE5",
      "text-halo-width": 2,
    };
  }
  if (result.id.includes("_minor_casing") || result.id.includes("_link_casing")) {
    result.paint["line-color"] = "#77736E";
  }
  if (result.id.match(/^roads_(tunnels_|bridges_)?minor$/)) {
    result.paint["line-color"] = "#F7F4EC";
  }
  if (result.id === "roads_minor_service") {
    result.paint["line-color"] = "#E8E2D7";
  }
  return result;
}

let qgisLayers = layers(sourceName, namedFlavor(baseFlavor), { lang: "en" })
  .filter((layer) => layer.type !== "symbol" || layer.layout?.["text-field"])
  .map((layer) => {
    if (layer.type !== "symbol") {
      return layer;
    }
    const layout = { ...layer.layout };
    // QGIS 3.44 does not yet convert Protomaps' multi-script `format`
    // expression. The source always retains the ordinary OSM `name`, so use
    // that portable field for the offline raster pack and drop sprite-only
    // decoration which is unnecessary at 800 x 480.
    layout["text-field"] = ["get", "name"];
    // QGIS resolves Mapbox font stack entries as installed family names.
    // "Noto Sans Regular" renders as missing-glyph boxes on the Windows LTR
    // build. QGIS's converter expects the installed family and style together;
    // Arial Regular is bundled and produces Latin OSM labels reliably.
    layout["text-font"] = ["Arial Regular"];
    delete layout["icon-image"];
    delete layout["icon-size"];
    delete layout["icon-offset"];
    return { ...layer, layout };
  });

if (flavor === "bluepaws-carto") {
  qgisLayers = qgisLayers.map(bluePawsCartoLayer);
  const parkIndex = qgisLayers.findIndex((layer) => layer.id === "landuse_park");
  qgisLayers.splice(Math.max(parkIndex, 2), 0, {
    id: "landuse_built",
    type: "fill",
    source: sourceName,
    "source-layer": "landuse",
    filter: ["in", "kind", "residential", "commercial", "retail"],
    paint: {
      "fill-color": [
        "match",
        ["get", "kind"],
        "commercial", "#ead8d8",
        "retail", "#ead8d8",
        "#e3dfdc",
      ],
      "fill-opacity": 0.8,
    },
  });
} else if (flavor === "bluepaws-road") {
  qgisLayers = qgisLayers.map(bluePawsRoadLayer);
}
const style = {
  version: 8,
  name: `BluePaws OSM ${flavor}`,
  glyphs: "https://protomaps.github.io/basemaps-assets/fonts/{fontstack}/{range}.pbf",
  sprite: `https://protomaps.github.io/basemaps-assets/sprites/v4/${flavor}`,
  sources: {
    [sourceName]: {
      type: "vector",
      tiles: [tileUrl],
      minzoom: 0,
      maxzoom: 15,
      attribution: "© OpenStreetMap contributors",
    },
  },
  layers: qgisLayers,
};

const destination = resolve(output);
await mkdir(dirname(destination), { recursive: true });
await writeFile(destination, `${JSON.stringify(style, null, 2)}\n`, "utf8");
console.log(destination);
