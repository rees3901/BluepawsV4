import type { SupabaseClient } from "@supabase/supabase-js";

// Lightweight gateway-only settings read. It must NOT update presence, insert
// a position, or consume a pending collar command.
export async function handleHubSettings(db: SupabaseClient, payload: unknown, token: string, requestId: string) {
  const reply = (body: object, status: number) => new Response(JSON.stringify({ ...body, request_id: requestId }),
    { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
  const p = payload as Record<string, unknown>;
  if (!p || p.format !== "hub_settings" || p.ingest_path !== "hub_self"
      || typeof p.gateway_guid16 !== "string" || !/^[0-9a-fA-F]{4}$/.test(p.gateway_guid16))
    return reply({ error: "invalid_hub_settings_request" }, 400);
  const id = parseInt(p.gateway_guid16, 16);
  if (!id || id % 16 !== 0) return reply({ error: "invalid_hub_settings_request" }, 400);
  const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token))),
    b => b.toString(16).padStart(2, "0")).join("");
  const { data: credential, error: authError } = await db.from("gateway_ingest_credentials")
    .select("gateway_guid16").eq("gateway_guid16", id).eq("token_hash", hash).eq("enabled", true).maybeSingle();
  if (authError) return reply({ error: "service_unavailable" }, 503);
  if (!credential) return reply({ error: "unauthorized" }, 401);
  // A valid old credential does not authorise a disabled or transferred gateway.
  const { data: gateway, error: gatewayError } = await db.from("gateways")
    .select("household_id").eq("gateway_guid16", id).eq("enabled", true).maybeSingle();
  if (gatewayError) return reply({ error: "service_unavailable" }, 503);
  if (!gateway?.household_id) return reply({ error: "unauthorized" }, 401);
  const { data: row, error } = await db.from("hub_presence")
    .select("settings_revision,desired_ble_enabled,desired_reporting_profile,display_name,home_emoji,portable_emoji,marker_colour")
    .eq("gateway_guid16", id).eq("household_id", gateway.household_id).maybeSingle();
  if (error) return reply({ error: "service_unavailable" }, 503);
  // First status report creates this row. No stale data from another Family.
  if (!row) return reply({ settings: null }, 200);
  return reply({ settings: {
    revision: row.settings_revision, ble_enabled: row.desired_ble_enabled,
    reporting_profile: row.desired_reporting_profile,
    display_name: row.display_name, home_emoji: row.home_emoji,
    portable_emoji: row.portable_emoji, marker_colour: row.marker_colour,
  } }, 200);
}

export function parseHubPresence(value: unknown) {
  const p = value as Record<string, unknown>;
  if (!p || typeof p !== "object" || p.format !== "hub_status" || p.ingest_path !== "hub_self"
      || typeof p.gateway_guid16 !== "string" || !/^[0-9a-fA-F]{4}$/.test(p.gateway_guid16)) throw new Error("invalid_hub_report");
  const id = parseInt(p.gateway_guid16, 16);
  if (!id || id % 16 !== 0 || typeof p.mode !== "string" || !["home", "portable", "off_grid"].includes(p.mode)) throw new Error("invalid_hub_report");
  const integer = (k: string, min: number, max: number) => {
    const n = p[k]; if (typeof n !== "number" || !Number.isSafeInteger(n) || n < min || n > max) throw new Error("invalid_" + k);
    return n;
  };
  const boolean = (k: string) => { if (typeof p[k] !== "boolean") throw new Error("invalid_" + k); return p[k] as boolean; };
  const lat = p.latitude ?? null, lon = p.longitude ?? null;
  const reporting = p.reporting_profile === undefined ? "normal" : p.reporting_profile;
  if (typeof reporting !== "string" || !["normal","power_save","active"].includes(reporting)) throw new Error("invalid_reporting_profile");
  if ((lat === null) !== (lon === null) || (lat !== null && (typeof lat !== "number" || !Number.isFinite(lat) || Math.abs(lat) > 90
      || typeof lon !== "number" || !Number.isFinite(lon) || Math.abs(lon) > 180))) throw new Error("invalid_position");
  return {
    p_gateway: id, p_mode: String(p.mode), p_lat: lat, p_lon: lon,
    p_fix_age_s: lat === null ? null : integer("fix_age_s", 0, 604800),
    p_uptime: integer("uptime_s", 0, 4294967295),
    p_rssi: p.wifi_rssi_dbm == null ? null : integer("wifi_rssi_dbm", -127, 0),
    p_ble: boolean("ble_enabled"), p_advertising: boolean("ble_advertising"),
    p_heap: integer("free_heap", 0, 2147483647),
    p_applied: integer("applied_revision", 0, Number.MAX_SAFE_INTEGER),
    p_reporting_profile: reporting,
    p_control_poll_s: p.control_poll_s == null ? null : integer("control_poll_s", 1, 60),
  };
}

export async function handleHubPresence(db: SupabaseClient, payload: unknown, token: string, requestId: string) {
  const reply = (body: object, status: number) => new Response(JSON.stringify({ ...body, request_id: requestId }),
    { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
  let args;
  try { args = parseHubPresence(payload); } catch (e) {
    return reply({ error: e instanceof Error ? e.message : "invalid_hub_report" }, 400);
  }
  const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token))),
    b => b.toString(16).padStart(2, "0")).join("");
  const { data: credential, error: authError } = await db.from("gateway_ingest_credentials")
    .select("gateway_guid16").eq("gateway_guid16", args.p_gateway).eq("token_hash", hash).eq("enabled", true).maybeSingle();
  if (authError) return reply({ error: "service_unavailable" }, 503);
  if (!credential) return reply({ error: "unauthorized" }, 401);
  const { data, error } = await db.rpc("bluepaws_record_hub_presence", args);
  if (error) return reply({ error: error.code === "42501" ? "unauthorized" : "service_unavailable" }, error.code === "42501" ? 401 : 503);
  const row = data?.[0];
  if (!row) return reply({ error: "service_unavailable" }, 503);
  // No collar commands are claimed by a hub's own heartbeat.
  return reply({ accepted: true, format: "hub_status", ingest_path: "hub_self",
    received_at: row.received_at, settings: {
      revision: row.settings_revision, ble_enabled: row.desired_ble_enabled,
      reporting_profile: row.desired_reporting_profile,
      display_name: row.display_name, home_emoji: row.home_emoji,
      portable_emoji: row.portable_emoji, marker_colour: row.marker_colour,
    } }, 200);
}
