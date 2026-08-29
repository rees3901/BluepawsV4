import assert from "node:assert/strict";
import test from "node:test";
import { alternatePreviewMapLayer, MAP_LAYER_DEFINITIONS, MAP_LAYER_PICKER_NAMES, previewMapZoom, type MapLayerName } from "./mapLayers.ts";

test("picker hides unreliable layers without deleting their provider definitions", () => {
  assert.deepEqual(MAP_LAYER_PICKER_NAMES, ["Street", "Satellite", "Topographic"]);
  for (const hiddenName of ["Satellite HD", "Humanitarian", "Esri Topo"] as const) {
    assert.ok(MAP_LAYER_DEFINITIONS[hiddenName]);
    assert.equal((MAP_LAYER_PICKER_NAMES as readonly MapLayerName[]).includes(hiddenName), false);
  }
});

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

test("uses satellite as the alternate preview except when satellite is active", () => {
  assert.equal(alternatePreviewMapLayer("Street"), "Satellite");
  assert.equal(alternatePreviewMapLayer("Topographic"), "Satellite");
  assert.equal(alternatePreviewMapLayer("Humanitarian"), "Satellite");
  assert.equal(alternatePreviewMapLayer("Esri Topo"), "Satellite");
  assert.equal(alternatePreviewMapLayer("Satellite"), "Street");
  assert.equal(alternatePreviewMapLayer("Satellite HD"), "Street");
});

test("keeps the alternate preview one zoom level out and inside tile limits", () => {
  assert.equal(previewMapZoom(17, "Satellite"), 16);
  assert.equal(previewMapZoom(1, "Satellite"), 0);
  assert.equal(previewMapZoom(24, "Satellite HD"), MAP_LAYER_DEFINITIONS["Satellite HD"].maxZoom);
});
