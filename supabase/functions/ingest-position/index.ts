import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient } from "@supabase/supabase-js";

const MAX_BODY_BYTES = 4_096;
const MAX_FUTURE_SKEW_MS = 5 * 60 * 1_000;
const SOURCE = "edge-api";

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

Deno.serve(async (request: Request) => {
  const requestId = crypto.randomUUID();

  if (request.method !== "POST") {
    return json({ error: "method_not_allowed", request_id: requestId }, 405, {
      Allow: "POST",
    });
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

  const validation = validatePayload(unknownPayload);
  if (!validation.ok) {
    return json(
      { error: "invalid_payload", detail: validation.error, request_id: requestId },
      400,
    );
  }
  const payload = validation.value;

  const token = readBearerToken(request.headers.get("authorization"));
  if (!token) return unauthorized(requestId);

  try {
    const supabase = createClient(
      requiredEnvironment("SUPABASE_URL"),
      serviceRoleKey(),
      { auth: { persistSession: false, autoRefreshToken: false } },
    );
    const tokenHash = await sha256(token);

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
      console.error("Credential lookup failed", {
        requestId,
        credentialError: credentialResult.error?.code,
        deviceError: deviceResult.error?.code,
      });
      return serviceUnavailable(
        requestId,
        "credential_lookup",
        [credentialResult.error?.code, deviceResult.error?.code].filter(
          (code): code is string => Boolean(code),
        ),
      );
    }
    if (!credentialResult.data || !deviceResult.data) return unauthorized(requestId);
    if (!deviceResult.data.household_id) {
      console.error("Provisioned device has no household", {
        requestId,
        deviceId: payload.device_id,
      });
      return serviceUnavailable(requestId, "device_provisioning");
    }

    const row = {
      device_uid: payload.device_id,
      message_id: payload.message_id,
      latitude: payload.latitude,
      longitude: payload.longitude,
      battery: payload.battery,
      source: SOURCE,
      recorded_at: payload.recorded_at,
      schema_version: payload.schema_version,
    };
    const insertResult = await supabase
      .from("positions")
      .upsert(row, {
        onConflict: "device_uid,message_id",
        ignoreDuplicates: true,
      })
      .select(
        "id,device_uid,message_id,latitude,longitude,battery,recorded_at,received_at,schema_version",
      )
      .maybeSingle<StoredPosition>();

    if (insertResult.error) {
      console.error("Position insert failed", {
        requestId,
        code: insertResult.error.code,
      });
      return serviceUnavailable(requestId, "position_insert");
    }

    if (insertResult.data) {
      return accepted(insertResult.data, requestId, false, 201);
    }

    const existingResult = await supabase
      .from("positions")
      .select(
        "id,device_uid,message_id,latitude,longitude,battery,recorded_at,received_at,schema_version",
      )
      .eq("device_uid", payload.device_id)
      .eq("message_id", payload.message_id)
      .maybeSingle<StoredPosition>();

    if (existingResult.error || !existingResult.data) {
      console.error("Duplicate lookup failed", {
        requestId,
        code: existingResult.error?.code,
      });
      return serviceUnavailable(requestId, "duplicate_lookup");
    }

    if (!payloadMatches(existingResult.data, payload)) {
      return json(
        {
          error: "message_id_conflict",
          device_id: payload.device_id,
          message_id: payload.message_id,
          request_id: requestId,
        },
        409,
      );
    }

    return accepted(existingResult.data, requestId, true, 200);
  } catch (error) {
    console.error("Unhandled ingest failure", {
      requestId,
      message: error instanceof Error ? error.message : "unknown",
    });
    return serviceUnavailable(requestId, "runtime");
  }
});

function validatePayload(
  value: unknown,
): { ok: true; value: PositionPayload } | { ok: false; error: string } {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return { ok: false, error: "body must be a JSON object" };
  }
  const body = value as Record<string, unknown>;
  const expected = new Set([
    "schema_version",
    "device_id",
    "message_id",
    "latitude",
    "longitude",
    "battery",
    "recorded_at",
  ]);
  if (Object.keys(body).some((key) => !expected.has(key))) {
    return { ok: false, error: "body contains an unknown field" };
  }
  if (body.schema_version !== 1) {
    return { ok: false, error: "schema_version must be 1" };
  }
  if (!integerInRange(body.device_id, 1, 65_535)) {
    return { ok: false, error: "device_id must be an integer from 1 to 65535" };
  }
  if (!integerInRange(body.message_id, 0, 2_147_483_647)) {
    return { ok: false, error: "message_id must be a non-negative 32-bit integer" };
  }
  if (!numberInRange(body.latitude, -90, 90)) {
    return { ok: false, error: "latitude must be a number from -90 to 90" };
  }
  if (!numberInRange(body.longitude, -180, 180)) {
    return { ok: false, error: "longitude must be a number from -180 to 180" };
  }
  if (body.battery !== null && !integerInRange(body.battery, 0, 100)) {
    return { ok: false, error: "battery must be null or an integer from 0 to 100" };
  }
  if (typeof body.recorded_at !== "string") {
    return { ok: false, error: "recorded_at must be an ISO-8601 timestamp" };
  }
  const recordedAt = Date.parse(body.recorded_at);
  if (!Number.isFinite(recordedAt)) {
    return { ok: false, error: "recorded_at must be an ISO-8601 timestamp" };
  }
  if (recordedAt > Date.now() + MAX_FUTURE_SKEW_MS) {
    return { ok: false, error: "recorded_at is too far in the future" };
  }

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

async function sha256(value: string) {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
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
  return (
    row.device_uid === payload.device_id &&
    row.message_id === payload.message_id &&
    row.latitude === payload.latitude &&
    row.longitude === payload.longitude &&
    row.battery === payload.battery &&
    row.schema_version === payload.schema_version &&
    Date.parse(row.recorded_at) === Date.parse(payload.recorded_at)
  );
}

function accepted(
  row: StoredPosition,
  requestId: string,
  duplicate: boolean,
  status: number,
) {
  return json(
    {
      accepted: true,
      duplicate,
      device_id: row.device_uid,
      message_id: row.message_id,
      received_at: row.received_at,
      request_id: requestId,
    },
    status,
  );
}

function unauthorized(requestId: string) {
  return json({ error: "unauthorized", request_id: requestId }, 401);
}

function serviceUnavailable(requestId: string, stage: string, codes?: string[]) {
  return json(
    { error: "service_unavailable", stage, codes, request_id: requestId },
    503,
  );
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
