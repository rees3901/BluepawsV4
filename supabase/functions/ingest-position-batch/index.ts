import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import {
  bytesToBase64,
  bytesToHex,
  type ParsedTlvRequest,
  parseTlvRequest,
  sha256Hex,
  TlvDecodeError,
} from "../ingest-position/tlv.ts";

const MAX_ITEMS = 10;
const MAX_BATCH_BYTES = 16_384;
const MAX_FUTURE_SKEW_MS = 5 * 60 * 1_000;
interface ReplayItem {
  local_id: number;
  wrapper: unknown;
}
interface IngestResult {
  accepted: boolean;
  duplicate: boolean;
  observation_id: number | null;
  position_id: number | null;
  received_at: string;
  error_code: string | null;
}

Deno.serve(async (request: Request) => {
  const requestId = crypto.randomUUID();
  if (request.method !== "POST") {
    return json({ error: "method_not_allowed", request_id: requestId }, 405);
  }
  if (
    !request.headers.get("content-type")?.toLowerCase().startsWith(
      "application/json",
    )
  ) {
    return json(
      { error: "content_type_must_be_json", request_id: requestId },
      415,
    );
  }
  const raw = await request.text();
  if (new TextEncoder().encode(raw).byteLength > MAX_BATCH_BYTES + 2_048) {
    return json({ error: "batch_too_large", request_id: requestId }, 413);
  }
  let body: unknown;
  try {
    body = JSON.parse(raw);
  } catch {
    return json({ error: "invalid_json", request_id: requestId }, 400);
  }
  if (
    !isObject(body) || !Array.isArray(body.items) || body.items.length < 1 ||
    body.items.length > MAX_ITEMS
  ) {
    return json({
      error: "items_must_contain_1_to_10_records",
      request_id: requestId,
    }, 400);
  }
  if (
    new TextEncoder().encode(JSON.stringify(body.items)).byteLength >
      MAX_BATCH_BYTES
  ) {
    return json(
      { error: "batch_items_exceed_16kb", request_id: requestId },
      413,
    );
  }
  const token = readBearer(request.headers.get("authorization"));
  if (!token) {
    return json({ error: "unauthorized", request_id: requestId }, 401);
  }

  const decoded: Array<{ input: ReplayItem; parsed: ParsedTlvRequest }> = [];
  for (const value of body.items) {
    if (
      !isObject(value) || !Number.isSafeInteger(value.local_id) ||
      Number(value.local_id) <= 0
    ) return json({ error: "invalid_local_id", request_id: requestId }, 400);
    try {
      const parsed = parseTlvRequest(value.wrapper);
      if (
        parsed.metadata.ingestPath !== "lora_hub" ||
        parsed.metadata.gatewayGuid16 === null ||
        parsed.metadata.gatewayRxTimeUnix === null
      ) {
        return json({
          error: "replay_requires_lora_gateway_wrapper",
          request_id: requestId,
        }, 400);
      }
      if (
        parsed.metadata.gatewayRxTimeUnix * 1_000 >
          Date.now() + MAX_FUTURE_SKEW_MS
      ) {
        return json({
          error: "gateway_time_too_far_in_future",
          request_id: requestId,
        }, 400);
      }
      decoded.push({ input: value as unknown as ReplayItem, parsed });
    } catch (error) {
      const code = error instanceof TlvDecodeError ? error.code : "invalid_tlv";
      return json({ error: "invalid_tlv", code, request_id: requestId }, 400);
    }
  }

  const gatewayId = decoded[0].parsed.metadata.gatewayGuid16!;
  if (
    decoded.some((item) => item.parsed.metadata.gatewayGuid16 !== gatewayId)
  ) {
    return json(
      { error: "mixed_gateways_not_allowed", request_id: requestId },
      400,
    );
  }
  const supabase = serverClient();
  const tokenHash = await sha256Text(token);
  const [credential, gateway] = await Promise.all([
    supabase.from("gateway_ingest_credentials").select("gateway_guid16").eq(
      "gateway_guid16",
      gatewayId,
    ).eq("token_hash", tokenHash).eq("enabled", true).maybeSingle(),
    supabase.from("gateways").select("gateway_guid16,household_id").eq(
      "gateway_guid16",
      gatewayId,
    ).eq("enabled", true).maybeSingle(),
  ]);
  if (credential.error || gateway.error) {
    return json({ error: "service_unavailable", request_id: requestId }, 503);
  }
  if (!credential.data || !gateway.data?.household_id) {
    return json({ error: "unauthorized", request_id: requestId }, 401);
  }
  const householdId = gateway.data.household_id;
  const deviceIds = [
    ...new Set(decoded.map((item) => item.parsed.packet.deviceGuid16)),
  ];
  const devices = await supabase.from("devices").select(
    "device_id,household_id",
  ).in("device_id", deviceIds).eq("enabled", true);
  if (devices.error) {
    return json({ error: "service_unavailable", request_id: requestId }, 503);
  }
  const allowed = new Set(
    (devices.data ?? []).filter((device) => device.household_id === householdId)
      .map((device) => device.device_id),
  );
  const results = [];
  for (const item of decoded) {
    if (!allowed.has(item.parsed.packet.deviceGuid16)) {
      results.push({
        local_id: item.input.local_id,
        status: "rejected",
        code: "unauthorized_device",
      });
    } else {
      results.push(await ingestOne(supabase, item.input.local_id, item.parsed));
    }
  }
  return json({
    gateway_guid16: gatewayId.toString(16).padStart(4, "0"),
    results,
    request_id: requestId,
  }, 200);
});

async function ingestOne(
  supabase: SupabaseClient,
  localId: number,
  parsed: ParsedTlvRequest,
) {
  const packet = parsed.packet, metadata = parsed.metadata;
  const result = await supabase.rpc("ingest_tlv_observation_replay", {
    p_protocol_version: packet.protocolVersion,
    p_device_guid16: packet.deviceGuid16,
    p_msg_seq_id: packet.messageSequenceId,
    p_time_unix: packet.timeUnix,
    p_status: packet.status,
    p_power_profile: packet.powerProfile,
    p_flags: packet.flags,
    p_tx_reason: packet.txReason,
    p_gnss_valid: packet.gnssValid,
    p_latitude: packet.latitude,
    p_longitude: packet.longitude,
    p_batt_mv: packet.batteryMillivolts,
    p_acc_m: packet.accuracyMetres,
    p_fix_age_s: packet.fixAgeSeconds,
    p_sat_count: packet.satelliteCount,
    p_tlv_data: packet.tlvs,
    p_payload_hash: await sha256Hex(packet.rawBytes),
    p_payload_b64: bytesToBase64(packet.rawBytes),
    p_hmac_body_b64: bytesToBase64(packet.authenticatedBytes),
    p_hmac_tag_hex: bytesToHex(packet.authenticationTag),
    p_ingest_path: metadata.ingestPath,
    p_link_type: metadata.linkType,
    p_gateway_guid16: metadata.gatewayGuid16,
    p_gateway_rx_time_unix: metadata.gatewayRxTimeUnix,
    p_link_rssi_dbm: metadata.linkRssiDbm,
    p_link_snr_db: metadata.linkSnrDb,
    p_cell_rsrp_dbm: null,
    p_cell_rsrq_db: null,
    p_cell_sinr_db: null,
    p_effective_seen_at: new Date(metadata.gatewayRxTimeUnix! * 1_000)
      .toISOString(),
    p_gateway_local_id: localId,
  });
  if (result.error) {
    return { local_id: localId, status: "retryable", code: result.error.code };
  }
  const row = firstRow<IngestResult>(result.data);
  if (!row) {
    return { local_id: localId, status: "retryable", code: "missing_result" };
  }
  if (!row.accepted) {
    return { local_id: localId, status: "rejected", code: row.error_code };
  }
  return {
    local_id: localId,
    status: row.duplicate ? "duplicate" : "accepted",
    observation_id: row.observation_id,
    position_updated: row.position_id !== null,
  };
}

function serverClient() {
  const url = Deno.env.get("SUPABASE_URL"),
    key = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !key) throw new Error("missing_supabase_runtime_configuration");
  return createClient(url, key, {
    auth: { persistSession: false, autoRefreshToken: false },
  });
}
function readBearer(value: string | null) {
  return value?.match(/^Bearer\s+(.+)$/i)?.[1]?.trim() || null;
}
async function sha256Text(value: string) {
  return sha256Hex(new TextEncoder().encode(value));
}
function firstRow<T>(value: unknown): T | null {
  return Array.isArray(value) && value.length
    ? value[0] as T
    : value && typeof value === "object"
    ? value as T
    : null;
}
function isObject(value: unknown): value is Record<string, unknown> {
  return Boolean(value && typeof value === "object" && !Array.isArray(value));
}
function json(value: unknown, status: number) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });
}
