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
const qgisLayers = layers(sourceName, namedFlavor(flavor), { lang: "en" })
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
    layout["text-font"] = ["Noto Sans Regular"];
    delete layout["icon-image"];
    delete layout["icon-size"];
    delete layout["icon-offset"];
    return { ...layer, layout };
  });
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
