export interface CommandFeedback {
  id: string;
  device_id: number;
  command_type: string;
  command_payload: { profile?: string };
  status: string;
  requested_at: string;
  expires_at: string;
}

export interface CollarFeedback {
  device_id: number;
  observation_id: number | null;
  flags: number | null;
  rxWindowUntil: number;
  command: CommandFeedback | null;
}

const PROFILE_LABELS: Record<string, string> = {
  normal: "Normal", power_save: "Power Save", active: "Active",
  lost_alert: "Emergency Lost", debug: "Debug",
};

export function commandMessage(command: CommandFeedback | null | undefined, now: number) {
  if (!command) return null;
  const submitted = Date.parse(command.requested_at);
  const expiry = Date.parse(command.expires_at);
  if (!Number.isFinite(submitted) || !Number.isFinite(expiry) || now < submitted || now - submitted >= 900_000) return null;
  let status = command.status;
  if ((status === "pending" || status === "sent") && now >= expiry) status = "expired";
  const labels: Record<string, string> = {
    pending: "Command pending", sent: "Command pending · awaiting ACK",
    acked: "Command acknowledged", expired: "Command expired · no ACK",
    failed: "Command failed", cancelled: "Command replaced or cancelled",
  };
  if (!labels[status]) return null;
  const detail = command.command_type === "set_profile"
    ? `profile → ${PROFILE_LABELS[command.command_payload.profile ?? ""] ?? "Unknown"}`
    : command.command_type.replaceAll("_", " ");
  return { text: `${labels[status]}: ${detail}`, pending: status === "pending" || status === "sent", status };
}

// Server supplies remaining time, not a new ten-second timer. Subtract the
// whole request duration (conservative) and never extend the same observation.
export function receiveDeadline(remaining: unknown, now: number, requestMs: number, sameObservationUntil?: number) {
  if (typeof remaining !== "number" || !Number.isFinite(remaining) || remaining <= 0 || remaining > 10_000) return 0;
  const until = now + Math.max(0, remaining - Math.max(0, requestMs));
  return sameObservationUntil === undefined ? until : Math.min(until, sameObservationUntil);
}
