import assert from "node:assert/strict";
import test from "node:test";
import { formatMapCoordinates, googleMapsUrl } from "./mapLocation.ts";

test("formats map coordinates to the requested precision", () => {
  assert.equal(formatMapCoordinates(51.50408, -0.08791), "51.50408, -0.08791");
  assert.equal(formatMapCoordinates(51.50408, -0.08791, 6), "51.504080, -0.087910");
});

test("creates a Google Maps search link for an exact coordinate", () => {
  assert.equal(
    googleMapsUrl(51.50408, -0.08791),
    "https://www.google.com/maps/search/?api=1&query=51.504080%2C%20-0.087910",
  );
});

