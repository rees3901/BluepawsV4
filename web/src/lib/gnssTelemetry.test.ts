import test from "node:test";
import assert from "node:assert/strict";
import type { SupabaseClient } from "@supabase/supabase-js";
import type { PositionRow } from "./telemetryRows.ts";
import { positionToTelemetryDevice, applyPresenceToTelemetryDevice } from "./telemetryRows.ts";
import { withGnssQuality } from "./gnssTelemetry.ts";

const position = { observation_id: 12, device_uid: 3001, flags: 1, recorded_at: "2026-10-04T12:00:00Z", received_at: "2026-10-04T12:00:01Z" } as PositionRow;
const observation = { id: 12, device_guid16: 3001, gnss_valid: true, acc_m: 8, sat_count: 9, fix_age_s: 2, recorded_at: position.recorded_at };
function client(data: unknown[], error: unknown = null) {
  return { from: (table: string) => {
    assert.equal(table, "observations");
    return { select: () => ({ eq: (key: string, value: string) => {
      assert.equal(key, "household_id"); assert.equal(value, "family");
      return { in: async (key: string, ids: number[]) => {
        assert.equal(key, "id"); assert.deepEqual(ids, [12]); return { data, error };
      } };
    } }) };
  } } as unknown as SupabaseClient;
}
test("loads quality from the exact displayed observation within the household", async () => {
  const [row] = await withGnssQuality(client([observation]), "family", [position]);
  assert.deepEqual(row.gnss, { accuracyM: 8, satellites: 9, fixAgeS: 2, recordedAt: position.recorded_at });
  const device = positionToTelemetryDevice(row);
  const awake = applyPresenceToTelemetryDevice(device, { device_id: 3001, household_id: "family", last_seen_at: "2026-10-04T12:05:00Z", last_seen_status_code: 0, last_seen_power_profile_code: 1, last_seen_tx_reason: 7, last_seen_battery_mv: 3900 });
  assert.deepEqual(awake.gnss, device.gnss);
});
test("wrong observations, no-fix reports and failures cannot fabricate GNSS quality", async () => {
  for (const row of [{ ...observation, id: 13 }, { ...observation, device_guid16: 3002 }, { ...observation, gnss_valid: false }]) {
    assert.equal((await withGnssQuality(client([row]), "family", [position]))[0].gnss, undefined);
  }
  assert.deepEqual(await withGnssQuality(client([], new Error("unavailable")), "family", [position]), [position]);
});
