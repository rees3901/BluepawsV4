import assert from "node:assert/strict";
import test from "node:test";
import {
  COLLAR_OFFLINE_AFTER_SECONDS,
  COLLAR_STALE_AFTER_SECONDS,
  collarCardFreshness,
  isCollarOffline,
  isCollarOfflineAge,
} from "./devicePresence.ts";
import type { TelemetryDevice } from "../types/telemetry.ts";

test("graduates collar cards from active to sleeping, stale and offline", () => {
  assert.equal(collarCardFreshness(0, true), "active");
  assert.equal(collarCardFreshness(0, false), "sleeping");
  assert.equal(collarCardFreshness(COLLAR_STALE_AFTER_SECONDS - 1, false), "sleeping");
  assert.equal(collarCardFreshness(COLLAR_STALE_AFTER_SECONDS - 1, true), "active");
  assert.equal(collarCardFreshness(COLLAR_STALE_AFTER_SECONDS, true), "stale");
  assert.equal(collarCardFreshness(COLLAR_OFFLINE_AFTER_SECONDS - 1, false), "stale");
  assert.equal(collarCardFreshness(COLLAR_OFFLINE_AFTER_SECONDS, false), "offline");
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
