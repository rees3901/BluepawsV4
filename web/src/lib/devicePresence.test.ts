import assert from "node:assert/strict";
import test from "node:test";
import {
  COLLAR_FADED_AFTER_SECONDS,
  COLLAR_GREYED_AFTER_SECONDS,
  COLLAR_OFFLINE_AFTER_SECONDS,
  COLLAR_STALE_AFTER_SECONDS,
  COLLAR_SUBDUED_AFTER_SECONDS,
  collarCardFreshness,
  collarFreshnessClass,
  collarSummary,
  isCollarOffline,
  isCollarOfflineAge,
  isDeviceInactive,
} from "./devicePresence.ts";
import type { TelemetryDevice } from "../types/telemetry.ts";

test("header counts collars only and uses the card offline boundary", () => {
  const now = 20_000_000;
  const device = (age: number, entity?: "hub") => ({lastUpdate: now - age * 1000, entity}) as TelemetryDevice;
  assert.deepEqual(collarSummary([], now), {total: 0, live: 0, offline: 0});
  assert.deepEqual(collarSummary([
    device(5), device(60), device(COLLAR_OFFLINE_AFTER_SECONDS - 1),
    device(COLLAR_OFFLINE_AFTER_SECONDS), device(90000), device(5, "hub"), device(90000, "hub"),
  ], now), {total: 5, live: 3, offline: 2});
});

test("graduates collar presentation through each visual freshness stage", () => {
  assert.equal(collarCardFreshness(0, true), "active");
  assert.equal(collarCardFreshness(0, false), "sleeping");
  assert.equal(collarCardFreshness(COLLAR_SUBDUED_AFTER_SECONDS - 1, false), "sleeping");
  assert.equal(collarCardFreshness(COLLAR_SUBDUED_AFTER_SECONDS, true), "subdued");
  assert.equal(collarCardFreshness(COLLAR_FADED_AFTER_SECONDS - 1, false), "subdued");
  assert.equal(collarCardFreshness(COLLAR_FADED_AFTER_SECONDS, false), "faded");
  assert.equal(collarCardFreshness(COLLAR_STALE_AFTER_SECONDS - 1, false), "faded");
  assert.equal(collarCardFreshness(COLLAR_STALE_AFTER_SECONDS, true), "stale");
  assert.equal(collarCardFreshness(COLLAR_GREYED_AFTER_SECONDS - 1, false), "stale");
  assert.equal(collarCardFreshness(COLLAR_GREYED_AFTER_SECONDS, false), "greyed");
  assert.equal(collarCardFreshness(COLLAR_OFFLINE_AFTER_SECONDS - 1, false), "greyed");
  assert.equal(collarCardFreshness(COLLAR_OFFLINE_AFTER_SECONDS, false), "offline");
});

test("maps freshness stages to shared card and marker classes", () => {
  assert.equal(collarFreshnessClass("active"), "");
  assert.equal(collarFreshnessClass("sleeping"), "collar-sleeping");
  assert.equal(collarFreshnessClass("subdued"), "freshness-subdued");
  assert.equal(collarFreshnessClass("faded"), "freshness-faded");
  assert.equal(collarFreshnessClass("stale"), "stale");
  assert.equal(collarFreshnessClass("greyed"), "freshness-greyed");
  assert.equal(collarFreshnessClass("offline"), "offline");
});

test("keeps a collar online throughout the four-hour grace period", () => {
  assert.equal(isCollarOfflineAge(COLLAR_OFFLINE_AFTER_SECONDS - 1), false);
});

test("marks a collar offline at four hours without a report", () => {
  assert.equal(isCollarOfflineAge(COLLAR_OFFLINE_AFTER_SECONDS), true);
});

test("never applies the collar rule to a Home Hub", () => {
  const hub = { entity: "hub", lastUpdate: 0 } as TelemetryDevice;
  assert.equal(isCollarOffline(hub, COLLAR_OFFLINE_AFTER_SECONDS * 2 * 1000), false);
});

test("does not treat a future timestamp as offline", () => {
  const collar = { lastUpdate: 10_000 } as TelemetryDevice;
  assert.equal(isCollarOffline(collar, 5_000), false);
});


test("inactive grouping and fit use collar and profile-specific hub boundaries", () => {
  const now = 20_000_000;
  const device = (age: number, entity?: "hub", hubReportingProfile?: "power_save" | "normal" | "active") =>
    ({lastUpdate: now - age * 1000, entity, hubReportingProfile}) as TelemetryDevice;
  assert.equal(isDeviceInactive(device(14399), now), false);
  assert.equal(isDeviceInactive(device(14400), now), true);
  for (const [profile, boundary] of [["power_save", 210], ["normal", 90], ["active", 60]] as const) {
    assert.equal(isDeviceInactive(device(boundary - 1, "hub", profile), now), false);
    assert.equal(isDeviceInactive(device(boundary, "hub", profile), now), true);
  }
});
