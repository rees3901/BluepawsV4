import test from "node:test";
import assert from "node:assert/strict";
import type { SupabaseClient } from "@supabase/supabase-js";
import type { PositionRow } from "./telemetryRows.ts";
import { positionToTelemetryDevice, applyPresenceToTelemetryDevice } from "./telemetryRows.ts";
import { withGnssQuality } from "./gnssTelemetry.ts";

const position = { observation_id: 12, device_uid: 3001, flags: 1, recorded_at: "2026-10-04T12:00:00Z", received_at: "2026-10-04T12:00:01Z" } as PositionRow;
const observation = { id: 12, device_guid16: 3001, gnss_valid: true, acc_m: 8, sat_count: 9, fix_age_s: 2, recorded_at: position.recorded_at };
function client(data: Record<string, unknown>[], error: unknown = null, failureError: unknown = null) {
  return { from: (table: string) => {
    assert.equal(table, "observations");
    let selected = "", limit = Infinity;
    const filters: ((row: Record<string, unknown>) => boolean)[] = [];
    const query = {
      select(columns: string) { selected = columns; return query; },
      eq(key: string, value: unknown) {
        if (key === "household_id") { assert.equal(value, "family"); return query; }
        filters.push(row => row[key] === value); return query;
      },
      in(key: string, values: unknown[]) {
        if (key === "tx_reason") assert.deepEqual(values, [0, 3, 4]);
        filters.push(row => values.includes(row[key])); return query;
      },
      gt(key: string, value: number | string) {
        filters.push(row => (row[key] as typeof value) > value); return query;
      },
      order(key: string, options: { ascending: boolean }) {
        assert.equal(key, "recorded_at"); assert.equal(options.ascending, false); return query;
      },
      limit(count: number) { assert.equal(count, 3); limit = count; return query; },
      abortSignal(signal: AbortSignal) { assert.ok(signal instanceof AbortSignal); return query; },
      then(resolve: (value: unknown) => unknown) {
        return Promise.resolve({ data: data.filter(row => filters.every(filter => filter(row))).slice(0, limit),
          error: selected === "id" ? failureError ?? error : error }).then(resolve);
      },
    };
    return query;
  } } as unknown as SupabaseClient;
}
test("loads quality from the exact displayed observation within the household", async () => {
  const [row] = await withGnssQuality(client([observation]), "family", [position]);
  assert.deepEqual(row.gnss, { accuracyM: 8, satellites: 9, fixAgeS: 2, recordedAt: position.recorded_at });
  const device = positionToTelemetryDevice(row);
  const awake = applyPresenceToTelemetryDevice(device, { device_id: 3001, household_id: "family", last_seen_at: "2026-10-04T12:05:00Z", last_seen_status_code: 0, last_seen_power_profile_code: 1, last_seen_tx_reason: 7, last_seen_battery_mv: 3900 });
  assert.deepEqual(awake.gnss, device.gnss);
});

test("only newer no-fix full reports count; check-ins, ACKs and replay do not", async () => {
  const at = "2026-10-04T12:01:00Z";
  const noFix = { device_guid16: 3001, gnss_valid: false, recorded_at: at };
  const lightweight = [1, 2, 5, 6, 7].map((tx_reason, index) => ({ ...noFix, id: 20 + index, tx_reason }));
  const ignored = [...lightweight, { ...noFix, id: 11, tx_reason: 0 },
    { ...noFix, id: 50, tx_reason: 0, recorded_at: "2026-10-03T12:00:00Z" },
    { ...noFix, id: 51, tx_reason: 0, device_guid16: 3002 }];
  assert.equal((await withGnssQuality(client([observation, ...ignored]), "family", [position]))[0].gpsFailedReports, 0);
  const failures = [0, 3, 4, 0].map((tx_reason, index) => ({ ...noFix, id: 30 + index, tx_reason }));
  for (let count = 1; count <= 4; count++) {
    const [row] = await withGnssQuality(client([observation, ...ignored, ...failures.slice(0, count)]), "family", [position]);
    assert.equal(row.gpsFailedReports, Math.min(count, 3));
    assert.equal(positionToTelemetryDevice(row).gpsFailedReports, Math.min(count, 3));
    assert.equal(row.gnss?.recordedAt, observation.recorded_at);
  }
});

test("failure-read outages do not fabricate a successful GPS cycle", async () => {
  const [row] = await withGnssQuality(client([observation], null, new Error("unavailable")), "family", [position]);
  assert.equal(row.gpsFailedReports, undefined);
  assert.equal(row.gnss?.recordedAt, observation.recorded_at);
});

test("a previously fast collar clock cannot hide subsequent full GPS failures", async () => {
  const ahead = { ...position, recorded_at: "2026-10-04T13:00:00Z" };
  const [row] = await withGnssQuality(client([{ ...observation, recorded_at: ahead.recorded_at },
    { id: 13, device_guid16: 3001, gnss_valid: false, tx_reason: 0, recorded_at: "2026-10-04T12:01:00Z" }]), "family", [ahead]);
  assert.equal(row.gpsFailedReports, 1);
});
test("wrong observations, no-fix reports and failures cannot fabricate GNSS quality", async () => {
  for (const row of [{ ...observation, id: 13 }, { ...observation, device_guid16: 3002 }, { ...observation, gnss_valid: false }]) {
    assert.equal((await withGnssQuality(client([row]), "family", [position]))[0].gnss, undefined);
  }
  assert.deepEqual(await withGnssQuality(client([], new Error("unavailable")), "family", [position]), [position]);
});
