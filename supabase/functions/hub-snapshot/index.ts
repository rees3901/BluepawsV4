import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient } from "@supabase/supabase-js";

const DEFAULT_LIMIT = 50;
const MAX_LIMIT = 100;

Deno.serve(async (request: Request) => {
  const requestId = crypto.randomUUID();
  if (request.method !== "GET") {
    return json({ error: "method_not_allowed", request_id: requestId }, 405);
  }
  const token = request.headers.get("authorization")?.match(/^Bearer\s+(.+)$/i)
    ?.[1]?.trim();
  if (!token) {
    return json({ error: "unauthorized", request_id: requestId }, 401);
  }
  const url = new URL(request.url);
  const gatewayText = url.searchParams.get("gateway_guid16") ?? "";
  if (!/^[0-9a-fA-F]{4}$/.test(gatewayText)) {
    return json({ error: "invalid_gateway", request_id: requestId }, 400);
  }
  const gatewayId = Number.parseInt(gatewayText, 16);
  const limit = Math.min(
    MAX_LIMIT,
    Math.max(
      1,
      Number.parseInt(
        url.searchParams.get("limit") ?? String(DEFAULT_LIMIT),
        10,
      ) || DEFAULT_LIMIT,
    ),
  );
  const cursor = Number.parseInt(url.searchParams.get("cursor") ?? "", 10);

  const supabaseUrl = Deno.env.get("SUPABASE_URL"),
    serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!supabaseUrl || !serviceKey) {
    return json({ error: "service_unavailable", request_id: requestId }, 503);
  }
  const supabase = createClient(supabaseUrl, serviceKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });
  const tokenHash = await sha256Hex(new TextEncoder().encode(token));
  const [credential, gateway] = await Promise.all([
    supabase.from("gateway_ingest_credentials").select("gateway_guid16").eq(
      "gateway_guid16",
      gatewayId,
    ).eq("token_hash", tokenHash).eq("enabled", true).maybeSingle(),
    supabase.from("gateways").select("gateway_guid16,household_id,display_name")
      .eq("gateway_guid16", gatewayId).eq("enabled", true).maybeSingle(),
  ]);
  if (credential.error || gateway.error) {
    return unavailable(
      requestId,
      "gateway_lookup",
      credential.error ?? gateway.error,
    );
  }
  if (!credential.data || !gateway.data?.household_id) {
    return json({ error: "unauthorized", request_id: requestId }, 401);
  }
  const householdId = gateway.data.household_id;

  let historyQuery = supabase.from("observations")
    .select(
      "id,device_guid16,protocol_version,msg_seq_id,time_unix,recorded_at,effective_seen_at,status,power_profile,flags,tx_reason,gnss_valid,latitude,longitude,batt_mv,acc_m,fix_age_s,sat_count,payload_b64,observation_paths(ingest_path,link_type,gateway_guid16,gateway_rx_time_unix,link_rssi_dbm,link_snr_db,offline_replay,gateway_local_id)",
    )
    .eq("household_id", householdId).order("id", { ascending: false }).limit(
      limit + 1,
    );
  if (Number.isSafeInteger(cursor) && cursor > 0) {
    historyQuery = historyQuery.lt("id", cursor);
  }

  const [devices, appearances, latest, history] = await Promise.all([
    supabase.from("devices").select(
      "device_id,display_name,enabled,last_seen_at,last_seen_status_code,last_seen_power_profile_code,last_seen_tx_reason,last_seen_battery_mv",
    ).eq("household_id", householdId).eq("enabled", true).order("device_id"),
    supabase.from("device_appearances").select(
      "device_id,avatar_kind,emoji_value,marker_colour",
    ).eq("household_id", householdId),
    supabase.from("device_latest_positions").select("*").eq(
      "household_id",
      householdId,
    ),
    historyQuery,
  ]);
  const firstError = devices.error ?? appearances.error ?? latest.error ??
    history.error;
  if (firstError) {
    const stage = devices.error
      ? "devices"
      : appearances.error
      ? "appearances"
      : latest.error
      ? "latest"
      : "history";
    return unavailable(requestId, stage, firstError);
  }
  const rows = history.data ?? [];
  const hasMore = rows.length > limit;
  const page = rows.slice(0, limit);
  return json({
    gateway: {
      gateway_guid16: gatewayText.toLowerCase(),
      display_name: gateway.data.display_name,
    },
    devices: devices.data ?? [],
    appearances: appearances.data ?? [],
    latest: latest.data ?? [],
    history: page,
    next_cursor: hasMore ? page[page.length - 1]?.id ?? null : null,
    request_id: requestId,
  }, 200);
});

function unavailable(
  requestId: string,
  stage: string,
  error: { code: string } | null,
) {
  // Codes and stage names diagnose failures without exposing credentials or rows.
  const code = error?.code ?? "unknown";
  console.error("Hub snapshot query failed", { requestId, stage, code });
  return json({
    error: "service_unavailable",
    stage,
    code,
    request_id: requestId,
  }, 503);
}

async function sha256Hex(bytes: Uint8Array) {
  const digest = await crypto.subtle.digest(
    "SHA-256",
    Uint8Array.from(bytes).buffer,
  );
  return [...new Uint8Array(digest)].map((byte) =>
    byte.toString(16).padStart(2, "0")
  ).join("");
}
function json(value: unknown, status: number) {
  return new Response(JSON.stringify(value), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}
