import assert from "node:assert/strict";
import test from "node:test";
import { formatMapCoordinates, googleMapsUrl, homeDistanceMetres, formatHomeDistance } from "./mapLocation.ts";

test("formats map coordinates to the requested precision", () => {
  assert.equal(formatMapCoordinates(51.50408, -0.08791), "51.50408, -0.08791");
  assert.equal(formatMapCoordinates(51.50408, -0.08791, 6), "51.504080, -0.087910");
});

test("home distance uses Gloucester hub coordinates, including later hub movement", () => {
  const device = { lat: 51.905809, lon: -2.239935, hasGps: true,
    homeHub: { lat: 51.90592233, lon: -2.239473833, fixAt: "2026-08-28T20:30:59Z" } };
  const metres = homeDistanceMetres(device)!;
  assert(metres > 30 && metres < 40);
  assert.equal(formatHomeDistance(metres), "34 m");
  assert.equal(homeDistanceMetres({ ...device, homeHub: { ...device.homeHub, lat: device.lat, lon: device.lon } }), 0);
  assert.equal(formatHomeDistance(156000), "156.0 km");
});

test("missing, invalid or non-GPS origins never become a fabricated distance", () => {
  const device = { lat: 0, lon: 0, hasGps: true,
    homeHub: { lat: 0, lon: 0, fixAt: "2026-08-28T20:30:59Z" } };
  assert.equal(homeDistanceMetres(device), 0);
  for (const change of [{ hasGps: false }, { homeHub: null }, { lat: NaN }, { lon: 181 },
    { homeHub: { ...device.homeHub, lat: null } }, { homeHub: { ...device.homeHub, fixAt: "bad" } }]) {
    assert.equal(formatHomeDistance(homeDistanceMetres({ ...device, ...change })), "Unknown");
  }
  assert.equal(formatHomeDistance(NaN), "Unknown");
});

test("creates a Google Maps search link for an exact coordinate", () => {
  assert.equal(
    googleMapsUrl(51.50408, -0.08791),
    "https://www.google.com/maps/search/?api=1&query=51.504080%2C%20-0.087910",
  );
});

