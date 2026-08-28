import type { SupabaseClient } from "@supabase/supabase-js";

export interface PendingDeviceCommand {
  id: string;
  command_sequence_id: number;
  command_type: string;
  command_payload: Record<string, unknown>;
  expires_at: string;
}

export function commandEnvelope(command: PendingDeviceCommand | null) {
  return command === null ? null : {
    id: command.id, sequence_id: command.command_sequence_id,
    type: command.command_type, payload: command.command_payload,
    expires_at: command.expires_at,
    expires_unix: Math.floor(Date.parse(command.expires_at) / 1000),
  };
}

// LTE is request/response, not an unsolicited push channel. This lightweight
// poll claims only this authenticated collar's commands, without fabricating
// telemetry, refreshing presence, or acknowledging anything.
export async function handleDeviceCommands(db: SupabaseClient, payload: unknown, token: string, requestId: string) {
  const reply = (body: object, status: number) => new Response(JSON.stringify({ ...body, request_id: requestId }),
    { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
  const p = payload as Record<string, unknown>;
  const id = p?.device_id;
  if (!p || p.format !== "device_commands" || p.ingest_path !== "cellular_direct"
    || typeof id !== "number" || !Number.isInteger(id) || id < 1 || id >= 65535 || id % 16 === 0)
    return reply({ error: "invalid_command_poll" }, 400);
  const hash = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token))),
    b => b.toString(16).padStart(2, "0")).join("");
  const { data: credential, error: authError } = await db.from("device_ingest_credentials")
    .select("device_id").eq("device_id", id).eq("token_hash", hash).eq("enabled", true).maybeSingle();
  if (authError) return reply({ error: "service_unavailable" }, 503);
  if (!credential) return reply({ error: "unauthorized" }, 401);
  const { data: device, error: deviceError } = await db.from("devices")
    .select("household_id").eq("device_id", id).eq("enabled", true).maybeSingle();
  if (deviceError) return reply({ error: "service_unavailable" }, 503);
  if (!device?.household_id) return reply({ error: "unauthorized" }, 401);
  const { data, error } = await db.rpc("bluepaws_claim_next_device_command", {
    requested_device_id: id, requested_transport: "cellular_direct",
  });
  if (error) return reply({ error: "service_unavailable" }, 503);
  const command = commandEnvelope(data?.[0] ?? null);
  return reply({ format: "device_commands", device_id: id, command_pending: command !== null, command }, 200);
}
