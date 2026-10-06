import assert from "node:assert/strict";
import test from "node:test";
import { appendTrailPoint, pruneTrailPoints, updateTrailPoints, TRAIL_MAX_AGE_MS, type TrailLatLng } from "./trailPoints.ts";
import type { TelemetryDevice, TrailPoint } from "../types/telemetry.ts";
import { positionToTelemetryDevice, applyPresenceToTelemetryDevice, type PositionRow } from "./telemetryRows.ts";

test("retains only the four newest positions after a long sequence", () => {
  let points: TrailLatLng[] = [];

  for (let index = 0; index < 100; index += 1) {
    points = appendTrailPoint(points, [index, -index]);
  }

  assert.deepEqual(points, [
    [96, -96],
    [97, -97],
    [98, -98],
    [99, -99],
  ]);
});

test("does not add the same reported coordinate twice", () => {
  const points: TrailLatLng[] = [[51.5, -0.1]];

  assert.strictEqual(appendTrailPoint(points, [51.5, -0.1]), points);
});

const now = Date.parse("2026-10-06T12:00:00Z");
const point = (age: number, lat = age): TrailPoint => ({ lat, lon: 0, recordedAt: new Date(now - age).toISOString() });
const device = { id: 3001, lat: 51, lon: -2, time: now / 1000,
  positionRecordedAt: new Date(now).toISOString() } as TelemetryDevice;

test("expires each point after four hours, retaining the exact boundary", () => {
  assert.deepEqual(pruneTrailPoints([point(TRAIL_MAX_AGE_MS + 1), point(TRAIL_MAX_AGE_MS), point(0)], now),
    [point(TRAIL_MAX_AGE_MS), point(0)]);
  assert.deepEqual(pruneTrailPoints([point(0)], now + TRAIL_MAX_AGE_MS + 1), []);
});
test("age filter and four-point cap reject malformed and future timestamps", () => {
  const points = Array.from({length: 8}, (_, i) => point(i * 1000));
  assert.deepEqual(pruneTrailPoints([...points, point(-1), {...point(0), recordedAt:'invalid'}], now), points.slice(0,4).reverse());
});
test("quiet devices lose their trail without being appended as a fresh point", () => {
  const original = updateTrailPoints([], [point(1000)], device, now);
  assert.equal(original.length, 2);
  assert.deepEqual(updateTrailPoints(original, [point(1000)], device, now + TRAIL_MAX_AGE_MS + 1), []);
  assert.equal(device.lat, 51, 'last-known marker coordinates are untouched');
});
test("loaded history merges with live points without duplicates or stale resurrection", () => {
  const initial = updateTrailPoints([], [], device, now);
  assert.deepEqual(updateTrailPoints(initial, initial, device, now), initial);
  const moved = {...device, lat: 52, positionRecordedAt: new Date(now+1000).toISOString()};
  assert.equal(updateTrailPoints(initial, initial, moved, now+1000).length, 2);
});
test("a fresh heartbeat does not renew the timestamp of a stale position", () => {
  const stale = new Date(now - TRAIL_MAX_AGE_MS - 1).toISOString();
  const row = {device_uid:3001, message_id:1, latitude:51, longitude:-2, recorded_at:stale,
    received_at:stale, flags:1, battery:50, battery_mv:3800} as PositionRow;
  const original = positionToTelemetryDevice(row);
  const heard = applyPresenceToTelemetryDevice(original, {device_id:3001, household_id:'family',
    last_seen_at:new Date(now).toISOString(), last_seen_status_code:null,
    last_seen_power_profile_code:null,last_seen_tx_reason:null,last_seen_battery_mv:null});
  assert.equal(heard.lastUpdate, now);
  assert.equal(heard.positionRecordedAt, stale);
  assert.deepEqual(updateTrailPoints([], [], heard, now), []);
});
test("a hub report without a GPS fix timestamp cannot create a breadcrumb", () => {
  assert.deepEqual(updateTrailPoints([], [], {...device,entity:'hub',positionRecordedAt:null}, now), []);
});
test("a future duplicate cannot replace a valid cached point", () => {
  const initial = updateTrailPoints([], [], device, now);
  const future = {...device, positionRecordedAt: new Date(now+60000).toISOString()};
  assert.deepEqual(updateTrailPoints(initial, [], future, now), initial);
});
