import test from "node:test";
import assert from "node:assert/strict";
import { gpsAgeLabel, gpsAsAt, gpsFreshness, formatGpsAge } from "./gpsFreshness.ts";
import { applyPresenceToTelemetryDevice } from "./telemetryRows.ts";
import type { TelemetryDevice } from "../types/telemetry.ts";

const start = Date.parse("2026-10-15T15:13:55Z");
const device = { id: 3001, hasGps: true, positionRecordedAt: new Date(start).toISOString(), lastUpdate: start,
  gnss: { accuracyM: 6, satellites: 8, fixAgeS: 0, recordedAt: new Date(start).toISOString() } } as TelemetryDevice;

test("GPS age uses the original fix, never newer check-ins", () => {
  const now = start + 8049_000;
  const heard = applyPresenceToTelemetryDevice(device, { device_id: 3001, household_id: "family", last_seen_at: new Date(now).toISOString(),
    last_seen_status_code: 1, last_seen_power_profile_code: 0, last_seen_tx_reason: 0, last_seen_battery_mv: 3900 });
  assert.equal(heard.lastUpdate, now);
  assert.deepEqual(gpsFreshness(heard, now), gpsFreshness(device, now));
  assert.equal(gpsFreshness(heard, now).label, "GPS 2h 14m old");
  assert.equal(gpsFreshness(heard, now).state, "stale");
});

test("display bands age without any new reports; fix age at transmission counts", () => {
  for (const [seconds, state] of [[0,"fresh"],[59,"fresh"],[60,"aging"],[299,"aging"],[300,"stale"],[86400,"stale"]] as const) {
    assert.equal(gpsFreshness(device, start + seconds * 1000).state, state);
  }
  const delayed = { ...device, gnss: { ...device.gnss!, fixAgeS: 45 } };
  assert.equal(gpsFreshness(delayed, start + 30_000).ageSeconds, 75);
  assert.equal(gpsFreshness(delayed, start + 30_000).fixAt, start - 45_000);
  assert.equal(gpsFreshness(delayed, start + 30_000).state, "aging");
});

test("unknown fix data is never silently called fresh", () => {
  for (const gnss of [null, { ...device.gnss!, fixAgeS: 65535 }, { ...device.gnss!, fixAgeS: -1 },
    { ...device.gnss!, fixAgeS: NaN }, { ...device.gnss!, recordedAt: "invalid" }]) {
    const gps = gpsFreshness({ ...device, gnss }, start);
    assert.equal(gps.state, "unknown"); assert.equal(gps.fixAt, null); assert.equal(gps.warning, true);
  }
  assert.equal(gpsFreshness({ ...device, hasGps: false }, start).state, "missing");
  assert.equal(gpsAsAt(gpsFreshness({ ...device, gnss: null }, start), start), "Unknown");
});

test("future clocks cannot pin GPS age at zero, and replay is not new GPS", () => {
  const ahead = { ...device, gnss: { ...device.gnss!, recordedAt: new Date(start + 300_000).toISOString(), fixAgeS: 10 } };
  assert.equal(gpsFreshness(ahead, start + 60_000).ageSeconds, 70);
  assert.equal(gpsFreshness({ ...ahead, positionRecordedAt: undefined }, start).state, "unknown");
  const replay = { ...device, lastUpdate: start + 7200_000 };
  assert.equal(gpsFreshness(replay, start + 7200_000).ageSeconds, 7200);
});

test("an explicit stale flag warns without ERROR_PRESENT; a wake/ACK never refreshes GPS", () => {
  assert.equal(gpsFreshness(device, start, 0x40).state, "aging");
  assert.equal(gpsFreshness(device, start + 600_000, 0).state, "stale");
  const refreshed = { ...device, positionRecordedAt: new Date(start + 600_000).toISOString(), gnss: { ...device.gnss!, recordedAt: new Date(start + 600_000).toISOString() } };
  assert.equal(gpsFreshness(refreshed, start + 600_000, 1).state, "fresh");
});

test("age formatting and old-fix timestamps include a date", () => {
  assert.equal(formatGpsAge(8049), "2h 14m");
  assert.equal(formatGpsAge(90061), "1d 1h");
  const now = start + 2 * 86400_000;
  assert.match(gpsAsAt(gpsFreshness(device, now), now), /2026/);
});

test("compact age copy preserves freshness and distinguishes unknown from no fix", () => {
  assert.equal(gpsAgeLabel(gpsFreshness(device, start + 4980_000)), "age 1h 23m");
  assert.equal(gpsAgeLabel(gpsFreshness(device, start + 10_000)), "age 10s");
  assert.equal(gpsAgeLabel(gpsFreshness({ ...device, gnss: null }, start)), "age —");
  assert.equal(gpsAgeLabel(gpsFreshness({ ...device, hasGps: false }, start)), "no fix");
});
