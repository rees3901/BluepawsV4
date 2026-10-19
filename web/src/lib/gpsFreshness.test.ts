import test from "node:test";
import assert from "node:assert/strict";
import { gpsAgeLabel, gpsAsAt, gpsFreshness, formatGpsAge } from "./gpsFreshness.ts";
import { applyPresenceToTelemetryDevice } from "./telemetryRows.ts";
import type { TelemetryDevice } from "../types/telemetry.ts";
import { readFileSync } from "node:fs";
import { COLLAR_GPS_REPORTING, GPS_FULL_REPORT_REASONS } from "./gpsReporting.ts";

const start = Date.parse("2026-10-15T15:13:55Z");
const device = { id: 3001, hasGps: true, profile: "Normal", status: "Out", bleHome: false, lastReportReason: 0,
  positionRecordedAt: new Date(start).toISOString(), lastUpdate: start,
  gnss: { accuracyM: 6, satellites: 8, fixAgeS: 0, recordedAt: new Date(start).toISOString() } } as TelemetryDevice;

test("GUI GPS cadence and full-message reasons stay aligned with the firmware policy", () => {
  const config = readFileSync(new URL("../../../shared/lib/BluepawsProtocol/bp_config.h", import.meta.url), "utf8");
  const protocol = readFileSync(new URL("../../../shared/lib/BluepawsProtocol/bp_protocol.h", import.meta.url), "utf8");
  for (const [profile, firmware] of [["Normal","NORMAL"],["PowerSave","POWERSAVE"],["Active","ACTIVE"],["Debug","DEBUG"],["Emergency Lost","LOST"]] as const) {
    const values = config.match(new RegExp(`\\{ PROFILE_${firmware},([^}]+)\\}`))![1].split(",").map(value => value.trim());
    const sleep = profile === "Emergency Lost" ? Number(config.match(/#define LOST_MODE_CYCLE_INTERVAL_S\s+(\d+)/)![1]) : Number(values[1]);
    assert.equal(COLLAR_GPS_REPORTING[profile].wakeSeconds, sleep);
    assert.equal(COLLAR_GPS_REPORTING[profile].homeRefreshEvery, Number(values[7]));
  }
  assert.deepEqual(GPS_FULL_REPORT_REASONS, ["TELEMETRY","INTERRUPT","BOOT"].map(name => Number(protocol.match(new RegExp(`TX_${name}\\s*=\\s*(\\d+)`))![1])));
});

test("GPS age uses the original fix, never newer check-ins", () => {
  const now = start + 8049_000;
  const heard = applyPresenceToTelemetryDevice(device, { device_id: 3001, household_id: "family", last_seen_at: new Date(now).toISOString(),
    last_seen_status_code: 1, last_seen_power_profile_code: 0, last_seen_tx_reason: 0, last_seen_battery_mv: 3900 });
  assert.equal(heard.lastUpdate, now);
  assert.equal(gpsFreshness(heard, now).fixAt, gpsFreshness(device, now).fixAt);
  assert.equal(gpsFreshness(heard, now).ageSeconds, gpsFreshness(device, now).ageSeconds);
  assert.equal(gpsFreshness(heard, now).label, "GPS 2h 14 min old");
  assert.equal(gpsFreshness(heard, now).state, "stale");
});

test("display bands age without any new reports; fix age at transmission counts", () => {
  for (const [seconds, state] of [[0,"fresh"],[659,"fresh"],[660,"fresh"],[661,"aging"],[1980,"aging"],[1981,"stale"],[86400,"stale"]] as const) {
    assert.equal(gpsFreshness(device, start + seconds * 1000).state, state);
  }
  const delayed = { ...device, gnss: { ...device.gnss!, fixAgeS: 45 } };
  assert.equal(gpsFreshness(delayed, start + 30_000).ageSeconds, 75);
  assert.equal(gpsFreshness(delayed, start + 30_000).fixAt, start - 45_000);
  assert.equal(gpsFreshness(delayed, start + 30_000).state, "fresh");
  assert.equal(gpsFreshness(delayed, start + 30_000).lastKnown, true);
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
  assert.equal(gpsFreshness(device, start + 600_000, 0).state, "fresh");
  assert.equal(gpsFreshness(device, start + 600_000, 0).lastKnown, true);
  const refreshed = { ...device, positionRecordedAt: new Date(start + 600_000).toISOString(), gnss: { ...device.gnss!, recordedAt: new Date(start + 600_000).toISOString() } };
  assert.equal(gpsFreshness(refreshed, start + 600_000, 1).state, "fresh");
});

test("each reported profile allows one full GPS cycle; three missed cycles turn red", () => {
  for (const [profile, seconds] of [["Normal",600],["PowerSave",1800],["Active",60],["Emergency Lost",30],["Debug",30]] as const) {
    const collar = { ...device, profile };
    const window = seconds + 60;
    for (const [age, state, missed] of [[window,"fresh",0],[window+1,"aging",1],[3*window,"aging",2],[3*window+1,"stale",3]] as const) {
      const gps = gpsFreshness(collar, start + age * 1000);
      assert.equal(gps.state, state, `${profile} at ${age}s`);
      assert.equal(gps.missedFullTelemetry, missed);
      assert.equal(gps.ageSeconds, age);
    }
  }
});

test("only recent beacon evidence enables the less frequent Home GPS schedule", () => {
  const now = start + 2 * 3600_000;
  const home = { ...device, profile: "PowerSave" as const, status: "Home" as const,
    bleHome: true, homeBeaconSeenAt: now, lastUpdate: now, lastReportReason: 7 };
  const gps = gpsFreshness(home, now);
  assert.equal(gps.homeConfirmed, true);
  assert.equal(gps.fullTelemetrySeconds, 5 * 3600);
  assert.equal(gps.state, "aging");
  assert.equal(gps.lastKnown, true);
  assert.equal(gps.ageSeconds, 7200);
  assert.equal(gps.fixAt, start);
  assert.equal(gpsAgeLabel(gps), "GPS age: 2h 0 min old");
  assert.match(gps.notice, /not a current position/);
  assert.equal(gpsFreshness(home, now + 1860_000).homeConfirmed, true);
  assert.equal(gpsFreshness(home, now + 1861_000).homeConfirmed, false);
  for (const changed of [{ status: "Out" as const }, { bleHome: false }, { homeBeaconSeenAt: null },
    { homeBeaconSeenAt: now + 1000 }, { profile: "Emergency Lost" as const }]) {
    const away = gpsFreshness({ ...home, ...changed }, now);
    assert.equal(away.homeConfirmed, false);
    assert.equal(away.state, "stale");
  }
});

test("absolute GPS age caps do not fabricate missed full reports or invalidate recent Home confirmation", () => {
  for (const [seconds, state] of [[3599,"fresh"],[3600,"aging"],[14399,"aging"],[14400,"stale"],[20000,"stale"]] as const) {
    const now = start + seconds * 1000;
    const home = { ...device, profile: "PowerSave" as const, status: "Home" as const,
      bleHome: true, homeBeaconSeenAt: now, lastUpdate: now, lastReportReason: 7 };
    const gps = gpsFreshness(home, now);
    assert.equal(gps.state, state, `GPS age ${seconds}s`);
    assert.equal(gps.homeConfirmed, true);
    assert.equal(gps.fixAt, start);
    assert.equal(gps.ageSeconds, seconds);
    if (seconds <= 14400) assert.equal(gps.missedFullTelemetry, 0);
    assert.match(gps.notice, /GPS age alone turns amber at 1h and red at 4h/);
  }
});

test("caps never delay the shorter profile warnings; Home Normal also caps at one/four hours", () => {
  for (const [profile, amber, red] of [["Normal",3600,14400],["PowerSave",3600,14400],
    ["Active",661,1981],["Emergency Lost",91,271],["Debug",91,271]] as const) {
    for (const [seconds, state] of [[amber,"aging"],[red,"stale"]] as const) {
      const now = start + seconds * 1000;
      const home = { ...device, profile, status: "Home" as const, bleHome: true, homeBeaconSeenAt: now };
      assert.equal(gpsFreshness(home, now).state, state, `${profile} at ${seconds}s`);
    }
  }
});

test("check-ins and ACKs neither consume full-telemetry opportunities nor clear failed full reports", () => {
  const now = start + 5 * 60_000;
  for (const reason of [1, 7]) {
    const heard = { ...device, lastReportReason: reason, lastUpdate: now };
    const gps = gpsFreshness(heard, now, 0x40);
    assert.equal(gps.state, "fresh", "a check-in/ACK cannot assert a failed GPS attempt");
    assert.equal(gps.ageSeconds, 300);
    for (const [gpsFailedReports, state] of [[1,"aging"],[2,"aging"],[3,"stale"]] as const) {
      assert.equal(gpsFreshness({ ...heard, gpsFailedReports }, now).state, state);
    }
  }
  assert.equal(gpsFreshness({ ...device, gpsFailedReports: 3 }, start).state, "stale");
  const refreshed = { ...device, gpsFailedReports: 0, gnss: { ...device.gnss!, recordedAt: new Date(now).toISOString() }, positionRecordedAt: new Date(now).toISOString() };
  assert.equal(gpsFreshness(refreshed, now).state, "fresh");
});

test("nine Home wake check-ins do not count as nine failed GPS telemetry messages", () => {
  let home: TelemetryDevice = { ...device, profile: "Active" };
  for (let wake = 1; wake <= 9; wake++) {
    const now = start + wake * 60_000;
    home = applyPresenceToTelemetryDevice(home, { device_id: 3001, household_id: "family", last_seen_at: new Date(now).toISOString(),
      last_seen_status_code: 0, last_seen_power_profile_code: 2, last_seen_tx_reason: 7, last_seen_battery_mv: 3900 });
    const gps = gpsFreshness(home, now);
    assert.equal(gps.state, "fresh");
    assert.equal(gps.missedFullTelemetry, 0);
    assert.equal(gps.ageSeconds, wake * 60);
    assert.equal(gps.fixAt, start);
  }
  const now = start + 661_000;
  assert.equal(gpsFreshness({ ...home, homeBeaconSeenAt: now }, now).state, "aging");
  assert.equal(gpsFreshness({ ...home, homeBeaconSeenAt: start + 1981_000 }, start + 1981_000).state, "stale");
});

test("age formatting and old-fix timestamps include a date", () => {
  assert.equal(formatGpsAge(8049), "2h 14 min");
  assert.equal(formatGpsAge(90061), "1d 1h");
  const now = start + 2 * 86400_000;
  assert.match(gpsAsAt(gpsFreshness(device, now), now), /2026/);
});

test("compact age copy preserves freshness and distinguishes unknown from no fix", () => {
  assert.equal(gpsAgeLabel(gpsFreshness(device, start + 4980_000)), "GPS age: 1h 23 min old");
  assert.equal(gpsAgeLabel(gpsFreshness(device, start + 10_000)), "GPS age: 10s old");
  assert.equal(gpsAgeLabel(gpsFreshness({ ...device, gnss: null }, start)), "GPS age: —");
  assert.equal(gpsAgeLabel(gpsFreshness({ ...device, hasGps: false }, start)), "GPS age: no fix");
});
