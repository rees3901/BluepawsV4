import assert from "node:assert/strict";
import test from "node:test";
import { applyPresenceToTelemetryDevice, positionToTelemetryDevice, type DevicePresenceRow, type PositionRow } from "./telemetryRows.ts";

test("maps TLV projection fields into the live dashboard model", () => {
  const row: PositionRow = {
    position_id: 1,
    device_uid: 1001,
    household_id: "household",
    message_id: 42,
    latitude: 51.5,
    longitude: -0.1,
    battery: null,
    battery_mv: 3700,
    status_code: 2,
    power_profile_code: 3,
    flags: 0x89,
    tx_reason: 5,
    ingest_path: "cellular_direct",
    link_type: "lte",
    link_rssi_dbm: -104,
    link_snr_db: 7,
    source: "tlv",
    recorded_at: "2026-08-13T08:00:00.000Z",
    received_at: "2026-08-13T08:00:01.000Z",
    schema_version: 1,
  };

  const device = positionToTelemetryDevice(row);
  assert.equal(device.status, "Lost");
  assert.equal(device.profile, "Emergency Lost");
  assert.equal(device.error, "Module");
  assert.equal(device.batt, 3700);
  assert.equal(device.batteryPercent, null);
  assert.equal(device.rssi, -104);
  assert.equal(device.snr, 7);
  assert.equal(device.bleHome, true);
  assert.equal(device.ingestPath, "cellular_direct");
});

test("maps every TLV status and power profile code into stable dashboard labels", () => {
  const baseRow: PositionRow = {
    position_id: 1,
    device_uid: 1001,
    household_id: "household",
    message_id: 42,
    latitude: 51.5,
    longitude: -0.1,
    battery: null,
    battery_mv: 3700,
    status_code: 1,
    power_profile_code: 1,
    flags: 0,
    tx_reason: 0,
    ingest_path: "cellular_direct",
    link_type: "lte",
    link_rssi_dbm: -104,
    link_snr_db: 7,
    source: "tlv",
    recorded_at: "2026-08-13T08:00:00.000Z",
    received_at: "2026-08-13T08:00:01.000Z",
    schema_version: 1,
  };
  const statuses = ["Home", "Out", "Lost", "Error"] as const;
  const profiles = ["PowerSave", "Normal", "Active", "Emergency Lost", "Debug"] as const;

  for (const [statusCode, statusLabel] of statuses.entries()) {
    const device = positionToTelemetryDevice({ ...baseRow, status_code: statusCode });
    assert.equal(device.status, statusLabel);
  }

  for (const [profileCode, profileLabel] of profiles.entries()) {
    const device = positionToTelemetryDevice({ ...baseRow, power_profile_code: profileCode });
    assert.equal(device.profile, profileLabel);
  }
});

test("overlays newer wake-check-in presence without moving the last known position", () => {
  const positionRow: PositionRow = {
    position_id: 1,
    device_uid: 1001,
    household_id: "household",
    message_id: 42,
    latitude: 51.5,
    longitude: -0.1,
    battery: 35,
    battery_mv: 3650,
    status_code: 1,
    power_profile_code: 1,
    flags: 0x01,
    tx_reason: 0,
    ingest_path: "cellular_direct",
    link_type: "lte",
    link_rssi_dbm: -104,
    link_snr_db: 7,
    source: "tlv",
    recorded_at: "2026-08-13T08:00:00.000Z",
    received_at: "2026-08-13T08:00:01.000Z",
    schema_version: 1,
  };
  const presenceRow: DevicePresenceRow = {
    device_id: 1001,
    household_id: "household",
    last_seen_at: "2026-08-13T09:00:00.000Z",
    last_seen_status_code: 0,
    last_seen_power_profile_code: 2,
    last_seen_tx_reason: 7,
    last_seen_battery_mv: 3900,
  };

  const device = applyPresenceToTelemetryDevice(positionToTelemetryDevice(positionRow), presenceRow);
  assert.equal(device.lat, 51.5);
  assert.equal(device.lon, -0.1);
  assert.equal(device.status, "Home");
  assert.equal(device.profile, "Active");
  assert.equal(device.batt, 3900);
  assert.equal(device.batteryPercent, null);
  assert.equal(device.bleHome, true);
  assert.equal(device.source, "tlv-wake-checkin");
  assert.equal(device.lastUpdate, Date.parse("2026-08-13T09:00:00.000Z"));
});
