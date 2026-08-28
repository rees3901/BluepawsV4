import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import { handleHubPresence, handleHubSettings } from "./hub-presence.ts";
import { commandEnvelope, handleDeviceCommands } from "./device-commands.ts";
import {
  bytesToBase64,
  bytesToHex,
  isTlvRequest,
  parseTlvRequest,
  sha256Hex,
  TlvDecodeError,
  type ParsedTlvRequest,
} from "./tlv.ts";

const MAX_BODY_BYTES = 4_096;
const MAX_FUTURE_SKEW_MS = 5 * 60 * 1_000;
const LEGACY_SOURCE = "edge-api";
const TX_REASON_ACK = 1;
const TX_REASON_WAKE_CHECKIN = 7;
const FLAG_HOME_BEACON_SEEN = 0x08;

interface PositionPayload {
  schema_version: 1;
  device_id: number;
  message_id: number;
  latitude: number;
  longitude: number;
  battery: number | null;
  recorded_at: string;
}

interface StoredPosition {
  id: number;
  device_uid: number;
  message_id: number;
  latitude: number;
  longitude: number;
  battery: number | null;
  recorded_at: string;
  received_at: string;
  schema_version: number;
}

interface TlvIngestResult {
  accepted: boolean;
  duplicate: boolean;
  observation_id: number | null;
  position_id: number | null;
  received_at: string;
  error_code: string | null;
}

interface PendingDeviceCommand {
  id: string;
  command_sequence_id: number;
  command_type: string;
  command_payload: Record<string, unknown>;
  expires_at: string;
}

interface AckedDeviceCommand {
  id: string;
  command_sequence_id: number;
  command_type: string;
  status: string;
  acknowledged_at: string;
}

Deno.serve(async (request: Request) => {
  const requestId = crypto.randomUUID();

  if (request.method !== "POST") {
    return json({ error: "method_not_allowed", request_id: requestId }, 405, { Allow: "POST" });
  }

  if (!request.headers.get("content-type")?.toLowerCase().startsWith("application/json")) {
    return json({ error: "content_type_must_be_json", request_id: requestId }, 415);
  }

  const declaredLength = Number(request.headers.get("content-length"));
  if (Number.isFinite(declaredLength) && declaredLength > MAX_BODY_BYTES) {
    return json({ error: "payload_too_large", request_id: requestId }, 413);
  }

  const rawBody = await request.text();
  if (new TextEncoder().encode(rawBody).byteLength > MAX_BODY_BYTES) {
    return json({ error: "payload_too_large", request_id: requestId }, 413);
  }

  let unknownPayload: unknown;
  try {
    unknownPayload = JSON.parse(rawBody);
  } catch {
    return json({ error: "invalid_json", request_id: requestId }, 400);
  }

  const token = readBearerToken(request.headers.get("authorization"));
  if (!token) return unauthorized(requestId);

  try {
    const supabase = serverClient();
    if (unknownPayload && typeof unknownPayload === "object" && "format" in unknownPayload
        && unknownPayload.format === "device_commands") {
      return await handleDeviceCommands(supabase, unknownPayload, token, requestId);
    }
    if (unknownPayload && typeof unknownPayload === "object" && "format" in unknownPayload
        && unknownPayload.format === "hub_settings") {
      return await handleHubSettings(supabase, unknownPayload, token, requestId);
    }
    if (unknownPayload && typeof unknownPayload === "object" && "format" in unknownPayload
        && unknownPayload.format === "hub_status") {
      return await handleHubPresence(supabase, unknownPayload, token, requestId);
    }
    if (isTlvRequest(unknownPayload)) {
      return await handleTlv(supabase, unknownPayload, token, requestId);
    }
    return await handleLegacyJson(supabase, unknownPayload, token, requestId);
  } catch (error) {
    console.error("Unhandled ingest failure", {
      requestId,
      message: error instanceof Error ? error.message : "unknown",
    });
    return serviceUnavailable(requestId, "runtime");
  }
});

async function handleTlv(
  supabase: SupabaseClient,
  unknownPayload: unknown,
  token: string,
  requestId: string,
) {
  let parsed: ParsedTlvRequest;
  try {
    parsed = parseTlvRequest(unknownPayload);
  } catch (error) {
    if (error instanceof TlvDecodeError) {
      return json({ error: "invalid_tlv", code: error.code, detail: error.message, request_id: requestId }, 400);
    }
    throw error;
  }

  const recordedAtMilliseconds = parsed.packet.timeUnix * 1_000;
  if (recordedAtMilliseconds > Date.now() + MAX_FUTURE_SKEW_MS) {
    return json({ error: "invalid_tlv", code: "time_too_far_in_future", request_id: requestId }, 400);
  }
  if (
    parsed.metadata.gatewayRxTimeUnix !== null &&
    parsed.metadata.gatewayRxTimeUnix * 1_000 > Date.now() + MAX_FUTURE_SKEW_MS
  ) {
    return json({ error: "invalid_tlv", code: "gateway_time_too_far_in_future", request_id: requestId }, 400);
  }

  const tokenHash = await sha256Text(token);
  const deviceQuery = supabase
    .from("devices")
    .select("device_id,household_id")
    .eq("device_id", parsed.packet.deviceGuid16)
    .eq("enabled", true)
    .maybeSingle();

  if (parsed.metadata.ingestPath === "cellular_direct") {
    const [credentialResult, deviceResult] = await Promise.all([
      supabase
        .from("device_ingest_credentials")
        .select("device_id")
        .eq("device_id", parsed.packet.deviceGuid16)
        .eq("token_hash", tokenHash)
        .eq("enabled", true)
        .maybeSingle(),
      deviceQuery,
    ]);
    if (credentialResult.error || deviceResult.error) {
      return lookupFailure(requestId, credentialResult.error?.code, deviceResult.error?.code);
    }
    if (!credentialResult.data || !deviceResult.data?.household_id) return unauthorized(requestId);
    // LTE fallback carries the exact same signed, hub-addressed observation
    // as LoRa. The collar bearer authenticates the sender; the destination must
    // additionally be an enabled hub in that collar's Family.
    if (parsed.packet.destinationId16 !== 0) {
      const gatewayResult = await supabase
        .from("gateways")
        .select("household_id")
        .eq("gateway_guid16", parsed.packet.destinationId16)
        .eq("enabled", true)
        .maybeSingle();
      if (gatewayResult.error) return lookupFailure(requestId, gatewayResult.error.code);
      if (!gatewayResult.data?.household_id
          || gatewayResult.data.household_id !== deviceResult.data.household_id) {
        return unauthorized(requestId);
      }
    }
  } else {
    const gatewayGuid16 = parsed.metadata.gatewayGuid16;
    if (gatewayGuid16 === null) return unauthorized(requestId);
    const [credentialResult, gatewayResult, deviceResult] = await Promise.all([
      supabase
        .from("gateway_ingest_credentials")
        .select("gateway_guid16")
        .eq("gateway_guid16", gatewayGuid16)
        .eq("token_hash", tokenHash)
        .eq("enabled", true)
        .maybeSingle(),
      supabase
        .from("gateways")
        .select("gateway_guid16,household_id")
        .eq("gateway_guid16", gatewayGuid16)
        .eq("enabled", true)
        .maybeSingle(),
      deviceQuery,
    ]);
    if (credentialResult.error || gatewayResult.error || deviceResult.error) {
      return lookupFailure(
        requestId,
        credentialResult.error?.code,
        gatewayResult.error?.code,
        deviceResult.error?.code,
      );
    }
    if (
      !credentialResult.data ||
      !gatewayResult.data ||
      !deviceResult.data?.household_id ||
      gatewayResult.data.household_id !== deviceResult.data.household_id
    ) return unauthorized(requestId);
  }

  const packet = parsed.packet;
  const metadata = parsed.metadata;
  const warnings = tlvWarnings(packet);
  if (warnings.length > 0) {
    console.warn("TLV accepted with protocol warnings", {
      requestId,
      deviceId: packet.deviceGuid16,
      messageSequenceId: packet.messageSequenceId,
      warnings,
    });
  }
  const payloadHash = await sha256Hex(packet.rawBytes);
  const rpcResult = await supabase.rpc("ingest_tlv_observation", {
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
    p_payload_hash: payloadHash,
    p_payload_b64: bytesToBase64(packet.rawBytes),
    p_hmac_body_b64: bytesToBase64(packet.authenticatedBytes),
    p_hmac_tag_hex: bytesToHex(packet.authenticationTag),
    p_ingest_path: metadata.ingestPath,
    p_link_type: metadata.linkType,
    p_gateway_guid16: metadata.gatewayGuid16,
    p_gateway_rx_time_unix: metadata.gatewayRxTimeUnix,
    p_link_rssi_dbm: metadata.linkRssiDbm,
    p_link_snr_db: metadata.linkSnrDb,
    p_cell_rsrp_dbm: metadata.cellRsrpDbm,
    p_cell_rsrq_db: metadata.cellRsrqDb,
    p_cell_sinr_db: metadata.cellSinrDb,
  });

  if (rpcResult.error) {
    console.error("TLV ingestion RPC failed", { requestId, code: rpcResult.error.code });
    return serviceUnavailable(requestId, "tlv_ingestion", [rpcResult.error.code]);
  }

  const row = firstRpcRow<TlvIngestResult>(rpcResult.data);
  if (!row) return serviceUnavailable(requestId, "tlv_ingestion_result");
  if (!row.accepted) {
    if (row.error_code === "message_identity_conflict") {
      return json({
        error: "message_identity_conflict",
        device_id: packet.deviceGuid16,
        message_id: packet.messageSequenceId,
        request_id: requestId,
      }, 409);
    }
    return unauthorized(requestId);
  }

  const ackedCommand = packet.txReason === TX_REASON_ACK
    ? await acknowledgeCommandIfPresent(supabase, packet.deviceGuid16, packet.tlvs, requestId)
    : null;
  // Both transports are request/response delivery opportunities. LTE collars
  // consume this response directly; a Home Hub converts the returned command
  // to the existing LoRa downlink packet while the collar RX window is open.
  const pendingCommand = await claimNextCommand(
    supabase,
    packet.deviceGuid16,
    metadata.ingestPath,
    requestId,
  );

  return json({
    accepted: true,
    format: "tlv",
    duplicate: row.duplicate,
    device_id: packet.deviceGuid16,
    message_id: packet.messageSequenceId,
    observation_id: row.observation_id,
    position_updated: row.position_id !== null,
    payload_hash: payloadHash,
    ingest_path: metadata.ingestPath,
    link_type: metadata.linkType,
    command_pending: pendingCommand !== null,
    command: commandEnvelope(pendingCommand),
    acked_command: ackedCommand === null ? null : {
      id: ackedCommand.id,
      sequence_id: ackedCommand.command_sequence_id,
      type: ackedCommand.command_type,
      status: ackedCommand.status,
      acknowledged_at: ackedCommand.acknowledged_at,
    },
    warnings,
    received_at: row.received_at,
    request_id: requestId,
  }, row.duplicate ? 200 : 201);
}

async function claimNextCommand(
  supabase: SupabaseClient,
  deviceId: number,
  transport: string,
  requestId: string,
) {
  const result = await supabase.rpc("bluepaws_claim_next_device_command", {
    requested_device_id: deviceId,
    requested_transport: transport,
  });
  if (result.error) {
    console.error("Command claim failed", { requestId, code: result.error.code });
    return null;
  }
  return firstRpcRow<PendingDeviceCommand>(result.data);
}

async function acknowledgeCommandIfPresent(
  supabase: SupabaseClient,
  deviceId: number,
  tlvs: Record<string, unknown>,
  requestId: string,
) {
  const ackedSequence = tlvs.acked_msg_seq_id;
  if (!Number.isInteger(ackedSequence)) return null;

  const result = await supabase.rpc("bluepaws_ack_device_command", {
    requested_device_id: deviceId,
    acked_command_sequence_id: ackedSequence,
  });
  if (result.error) {
    console.error("Command ACK failed", { requestId, code: result.error.code });
    return null;
  }
  return firstRpcRow<AckedDeviceCommand>(result.data);
}

function tlvWarnings(packet: ParsedTlvRequest["packet"]) {
  const warnings: string[] = [];
  if (packet.txReason === TX_REASON_WAKE_CHECKIN && (packet.flags & FLAG_HOME_BEACON_SEEN) === 0) {
    warnings.push("wake_checkin_without_home_beacon_seen");
  }
  return warnings;
}

async function handleLegacyJson(
  supabase: SupabaseClient,
  unknownPayload: unknown,
  token: string,
  requestId: string,
) {
  const validation = validatePayload(unknownPayload);
  if (!validation.ok) {
    return json({ error: "invalid_payload", detail: validation.error, request_id: requestId }, 400);
  }
  const payload = validation.value;
  const tokenHash = await sha256Text(token);
  const [credentialResult, deviceResult] = await Promise.all([
    supabase
      .from("device_ingest_credentials")
      .select("device_id")
      .eq("device_id", payload.device_id)
      .eq("token_hash", tokenHash)
      .eq("enabled", true)
      .maybeSingle(),
    supabase
      .from("devices")
      .select("device_id,household_id")
      .eq("device_id", payload.device_id)
      .eq("enabled", true)
      .maybeSingle(),
  ]);

  if (credentialResult.error || deviceResult.error) {
    return lookupFailure(requestId, credentialResult.error?.code, deviceResult.error?.code);
  }
  if (!credentialResult.data || !deviceResult.data?.household_id) return unauthorized(requestId);

  const row = {
    device_uid: payload.device_id,
    message_id: payload.message_id,
    latitude: payload.latitude,
    longitude: payload.longitude,
    battery: payload.battery,
    source: LEGACY_SOURCE,
    recorded_at: payload.recorded_at,
    schema_version: payload.schema_version,
  };
  const insertResult = await supabase
    .from("positions")
    .upsert(row, {
      onConflict: "device_uid,message_id",
      ignoreDuplicates: true,
    })
    .select("id,device_uid,message_id,latitude,longitude,battery,recorded_at,received_at,schema_version")
    .maybeSingle<StoredPosition>();

  if (insertResult.error) {
    console.error("Position insert failed", { requestId, code: insertResult.error.code });
    return serviceUnavailable(requestId, "position_insert");
  }
  if (insertResult.data) return acceptedLegacy(insertResult.data, requestId, false, 201);

  const existingResult = await supabase
    .from("positions")
    .select("id,device_uid,message_id,latitude,longitude,battery,recorded_at,received_at,schema_version")
    .eq("device_uid", payload.device_id)
    .eq("message_id", payload.message_id)
    .maybeSingle<StoredPosition>();

  if (existingResult.error || !existingResult.data) {
    console.error("Duplicate lookup failed", { requestId, code: existingResult.error?.code });
    return serviceUnavailable(requestId, "duplicate_lookup");
  }
  if (!payloadMatches(existingResult.data, payload)) {
    return json({
      error: "message_id_conflict",
      device_id: payload.device_id,
      message_id: payload.message_id,
      request_id: requestId,
    }, 409);
  }
  return acceptedLegacy(existingResult.data, requestId, true, 200);
}

function validatePayload(value: unknown): { ok: true; value: PositionPayload } | { ok: false; error: string } {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return { ok: false, error: "body must be a JSON object" };
  }
  const body = value as Record<string, unknown>;
  const expected = new Set(["schema_version", "device_id", "message_id", "latitude", "longitude", "battery", "recorded_at"]);
  if (Object.keys(body).some((key) => !expected.has(key))) {
    return { ok: false, error: "body contains an unknown field" };
  }
  if (body.schema_version !== 1) return { ok: false, error: "schema_version must be 1" };
  if (!integerInRange(body.device_id, 1, 65_535)) return { ok: false, error: "device_id must be an integer from 1 to 65535" };
  if (!integerInRange(body.message_id, 0, 2_147_483_647)) return { ok: false, error: "message_id must be a non-negative 32-bit integer" };
  if (!numberInRange(body.latitude, -90, 90)) return { ok: false, error: "latitude must be a number from -90 to 90" };
  if (!numberInRange(body.longitude, -180, 180)) return { ok: false, error: "longitude must be a number from -180 to 180" };
  if (body.battery !== null && !integerInRange(body.battery, 0, 100)) return { ok: false, error: "battery must be null or an integer from 0 to 100" };
  if (typeof body.recorded_at !== "string") return { ok: false, error: "recorded_at must be an ISO-8601 timestamp" };
  const recordedAt = Date.parse(body.recorded_at);
  if (!Number.isFinite(recordedAt)) return { ok: false, error: "recorded_at must be an ISO-8601 timestamp" };
  if (recordedAt > Date.now() + MAX_FUTURE_SKEW_MS) return { ok: false, error: "recorded_at is too far in the future" };

  return {
    ok: true,
    value: {
      schema_version: 1,
      device_id: body.device_id as number,
      message_id: body.message_id as number,
      latitude: body.latitude as number,
      longitude: body.longitude as number,
      battery: body.battery as number | null,
      recorded_at: new Date(recordedAt).toISOString(),
    },
  };
}

function integerInRange(value: unknown, minimum: number, maximum: number) {
  return Number.isInteger(value) && (value as number) >= minimum && (value as number) <= maximum;
}

function numberInRange(value: unknown, minimum: number, maximum: number) {
  return typeof value === "number" && Number.isFinite(value) && value >= minimum && value <= maximum;
}

function readBearerToken(header: string | null) {
  const match = header?.match(/^Bearer ([A-Za-z0-9_-]{32,256})$/);
  return match?.[1] ?? null;
}

async function sha256Text(value: string) {
  return sha256Hex(new TextEncoder().encode(value));
}

function serverClient() {
  return createClient(requiredEnvironment("SUPABASE_URL"), serviceRoleKey(), {
    auth: { persistSession: false, autoRefreshToken: false },
  });
}

function requiredEnvironment(name: string) {
  const value = Deno.env.get(name);
  if (!value) throw new Error(`Missing ${name}`);
  return value;
}

function serviceRoleKey() {
  const legacyKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (legacyKey) return legacyKey;
  const configuredKeys = Deno.env.get("SUPABASE_SECRET_KEYS");
  if (configuredKeys) {
    try {
      const parsed = JSON.parse(configuredKeys);
      if (Array.isArray(parsed) && typeof parsed[0] === "string") return parsed[0];
      if (parsed && typeof parsed === "object") {
        for (const key of ["service_role", "default", "secret"]) {
          const candidate = (parsed as Record<string, unknown>)[key];
          if (typeof candidate === "string") return candidate;
        }
      }
    } catch {
      if (configuredKeys.startsWith("sb_secret_")) return configuredKeys;
    }
  }
  throw new Error("Missing a Supabase server key");
}

function payloadMatches(row: StoredPosition, payload: PositionPayload) {
  return row.device_uid === payload.device_id &&
    row.message_id === payload.message_id &&
    row.latitude === payload.latitude &&
    row.longitude === payload.longitude &&
    row.battery === payload.battery &&
    row.schema_version === payload.schema_version &&
    Date.parse(row.recorded_at) === Date.parse(payload.recorded_at);
}

function acceptedLegacy(row: StoredPosition, requestId: string, duplicate: boolean, status: number) {
  return json({
    accepted: true,
    format: "json",
    duplicate,
    device_id: row.device_uid,
    message_id: row.message_id,
    received_at: row.received_at,
    request_id: requestId,
  }, status);
}

function firstRpcRow<T>(value: unknown): T | null {
  if (Array.isArray(value)) return (value[0] as T | undefined) ?? null;
  return value && typeof value === "object" ? value as T : null;
}

function lookupFailure(requestId: string, ...codes: Array<string | undefined>) {
  console.error("Credential lookup failed", { requestId, codes: codes.filter(Boolean) });
  return serviceUnavailable(requestId, "credential_lookup", codes.filter((code): code is string => Boolean(code)));
}

function unauthorized(requestId: string) {
  return json({ error: "unauthorized", request_id: requestId }, 401);
}

function serviceUnavailable(requestId: string, stage: string, codes?: string[]) {
  return json({ error: "service_unavailable", stage, codes, request_id: requestId }, 503);
}

function json(body: unknown, status: number, extraHeaders: HeadersInit = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      ...extraHeaders,
    },
  });
}
