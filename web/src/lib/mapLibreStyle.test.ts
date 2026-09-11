import assert from "node:assert/strict";
import test from "node:test";
import { mapLibreStyle, ONLINE_VECTOR_STYLE } from "./mapLibreStyle.ts";

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
