import assert from "node:assert/strict";
import test from "node:test";
import { MAP_LAYER_DEFINITIONS, type MapLayerName } from "./mapLayers.ts";

test("hard-caps layers that become blank beyond their reliable UK coverage", () => {
  const expectedLimits: Partial<Record<MapLayerName, number>> = {
    Satellite: 19,
    "Satellite HD": 18,
    "Esri Topo": 18,
  };

  for (const [name, expectedLimit] of Object.entries(expectedLimits)) {
    const layer = MAP_LAYER_DEFINITIONS[name as MapLayerName];
    assert.equal(layer.maxNativeZoom, expectedLimit);
    assert.equal(layer.maxZoom, expectedLimit);
  }
});

test("retains deliberate tile magnification for layers that tolerate over-zoom", () => {
  for (const name of ["Street", "Topographic", "Humanitarian"] as const) {
    const layer = MAP_LAYER_DEFINITIONS[name];
    assert.ok(layer.maxZoom > layer.maxNativeZoom);
  }
});
