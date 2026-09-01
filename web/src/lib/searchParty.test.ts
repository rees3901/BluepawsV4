import test from "node:test";
import assert from "node:assert/strict";
import { parseSearchPartySnapshot, searchPartyAvatarUrl } from "./searchPartySnapshot.ts";

const token = "a".repeat(64);

test("builds a token-scoped private avatar URL", () => {
  assert.equal(
    searchPartyAvatarUrl(token, "hub", 16, "https://project.supabase.co/"),
    `https://project.supabase.co/functions/v1/search-party-avatar?token=${token}&entity=hub&id=16`,
  );
  assert.equal(searchPartyAvatarUrl("bad", "collar", 1001, "https://project.supabase.co"), undefined);
});

test("parses sanitized collars, trails and a read-only portable Home Hub", () => {
  const previous = process.env.NEXT_PUBLIC_SUPABASE_URL;
  process.env.NEXT_PUBLIC_SUPABASE_URL = "https://project.supabase.co";
  const snapshot = parseSearchPartySnapshot({
    valid: true,
    householdId: "family-1",
    familyName: "Bluepaws Family",
    expiresAt: "2026-09-01T18:00:00Z",
    devices: [{
      position_id: 1, device_uid: 1001, household_id: "family-1", message_id: 9,
      latitude: 51.86426, longitude: -2.2377, battery: 95, battery_mv: 4170,
      status_code: 1, power_profile_code: 0, flags: 1, tx_reason: 0,
      ingest_path: "lora_hub", link_type: "lora", link_rssi_dbm: -72, link_snr_db: 9,
      source: "edge-api", recorded_at: "2026-09-01T13:00:00Z", received_at: "2026-09-01T13:00:01Z",
      schema_version: 2, display_name: "Simba", avatar_kind: "photo", emoji_value: "🐈", marker_colour: "#ffaa00",
    }],
    trails: { "1001": [
      { lat: 51.86, lon: -2.24, recordedAt: "2026-09-01T12:50:00Z" },
      { lat: 51.86426, lon: -2.2377, recordedAt: "2026-09-01T13:00:00Z" },
    ] },
    hubs: [{
      gateway_guid16: 16, display_name: "Home Hub", mode: "portable",
      received_at: "2026-09-01T13:00:02Z", latitude: 51.865, longitude: -2.238,
      fix_at: "2026-09-01T13:00:00Z", avatar_kind: "emoji", home_emoji: "🏡",
      portable_emoji: "📱", marker_colour: "#38bdf8",
    }],
  }, token);
  process.env.NEXT_PUBLIC_SUPABASE_URL = previous;

  assert.equal(snapshot.valid, true);
  assert.equal(snapshot.devices.length, 2);
  assert.equal(snapshot.devices[0]?.name, "Simba");
  assert.match(snapshot.avatars[1001]?.photoUrl ?? "", /search-party-avatar/);
  assert.deepEqual(snapshot.trailHistory[1001]?.map((point) => point.lat), [51.86, 51.86426]);
  assert.equal(snapshot.devices[1]?.id, -16);
  assert.equal(snapshot.devices[1]?.hubMode, "portable");
  assert.equal(snapshot.devices[1]?.hasGps, true);
});
