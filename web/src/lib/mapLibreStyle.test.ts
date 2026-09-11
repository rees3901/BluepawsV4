import assert from "node:assert/strict";
import test from "node:test";
import { mapLibreStyle, ONLINE_VECTOR_SOURCE } from "./mapLibreStyle.ts";

test("uses the hosted vector style until a PMTiles archive is configured", () => {
  const style = mapLibreStyle();
  assert.notEqual(typeof style, "string");
  if (typeof style === "string") return;
  assert.equal("url" in style.sources.openmaptiles ? style.sources.openmaptiles.url : null, ONLINE_VECTOR_SOURCE);
  assert.ok(style.layers.some(layer => layer.id === "water"));
});

test("builds a Protomaps style around the configured PMTiles archive", () => {
  const style = mapLibreStyle("https://cdn.example.test/bluepaws.pmtiles");
  assert.notEqual(typeof style, "string");
  if (typeof style === "string") return;
  assert.equal(style.sources.protomaps.type, "vector");
  assert.equal("url" in style.sources.protomaps ? style.sources.protomaps.url : null, "pmtiles://https://cdn.example.test/bluepaws.pmtiles");
  assert.ok(style.layers.length > 20);
});
