import assert from "node:assert/strict";
import test from "node:test";
import { mapLibreStyle, mapLibreRasterStyle, ONLINE_VECTOR_STYLE } from "./mapLibreStyle.ts";
import { MAP_LAYER_DEFINITIONS, MAP_LAYER_PICKER_NAMES } from "./mapLayers.ts";

test("rotatable raster maps reuse the existing providers and native zoom limits", () => {
  for (const name of MAP_LAYER_PICKER_NAMES) {
    const style = mapLibreRasterStyle(name);
    const source = style.sources.basemap;
    assert.equal(source.type, "raster");
    if (source.type !== "raster") continue;
    assert.equal(source.tileSize, 256);
    assert.equal(source.maxzoom, MAP_LAYER_DEFINITIONS[name].maxNativeZoom);
    assert.equal(source.attribution, MAP_LAYER_DEFINITIONS[name].attribution);
    assert.ok(source.tiles?.every(url => url.startsWith("https://") && !url.includes("{s}")));
    assert.deepEqual(style.layers, [{ id: "basemap", type: "raster", source: "basemap" }]);
  }
  assert.deepEqual(mapLibreRasterStyle("Street").sources.basemap, {
    type: "raster", tiles: [MAP_LAYER_DEFINITIONS.Street.url], tileSize: 256, maxzoom: 19, attribution: MAP_LAYER_DEFINITIONS.Street.attribution,
  });
  const topographic = mapLibreRasterStyle("Topographic").sources.basemap;
  assert.ok("tiles" in topographic && topographic.tiles?.length === 3);
});

test("uses the hosted vector style until a PMTiles archive is configured", () => {
  assert.equal(mapLibreStyle(), ONLINE_VECTOR_STYLE);
});

test("builds a Protomaps style around the configured PMTiles archive", () => {
  const style = mapLibreStyle("https://cdn.example.test/bluepaws.pmtiles");
  assert.notEqual(typeof style, "string");
  if (typeof style === "string") return;
  assert.equal(style.sources.protomaps.type, "vector");
  assert.equal("url" in style.sources.protomaps ? style.sources.protomaps.url : null, "pmtiles://https://cdn.example.test/bluepaws.pmtiles");
  assert.ok(style.layers.length > 20);
});
