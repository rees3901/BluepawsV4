/** Diagnostics from one report. Never combine these with an older map fix. */
export interface CollarFaultReport {
  flags: number;
  txReason?: number | null;
  resetReason?: number | null;
}

export function collarFault(report?: CollarFaultReport | null, legacyFault = false) {
  const flags = report?.flags;
  const hasFlags = typeof flags === "number" && Number.isInteger(flags) && flags >= 0 && flags <= 255;
  if (hasFlags ? !(flags & 0x80) : !legacyFault) return null;

  // Accompanying indicators, not proof of a root cause. ACK/config/ping/home
  // check-ins can deliberately omit GNSS. Keep in sync with HubFeedback.fault.
  const reasons: string[] = [];
  if (hasFlags) {
    if (flags & 0x40) reasons.push("stale GPS");
    else if (!(flags & 0x01) && [0, 3, 4, 5].includes(report?.txReason ?? -1)) reasons.push("GPS fix unavailable");
    if (flags & 0x04) reasons.push("low battery");
  }
  const detail = reasons.length ? reasons[0] + (reasons.length > 1 ? ` +${reasons.length - 1}` : "") : "cause unspecified";
  let title = reasons.length
    ? `Reported fault — ${reasons.join("; ")}. These indicators accompany ERROR_PRESENT; they do not establish the root cause.`
    : "Reported fault — cause unspecified. The report does not identify a specific cause.";
  const reset = report?.resetReason;
  if (hasFlags && typeof reset === "number" && Number.isInteger(reset) && reset >= 0 && reset <= 255) {
    title += ` Reset diagnostic 0x${reset.toString(16).padStart(2, "0").toUpperCase()} describes the previous reset, not necessarily this fault.`;
  }
  return { label: `Reported fault — ${detail}`, title };
}

interface FeedbackIdentity { device_id: number; observation_id: number | null; flags: number | null }

/** Fetch only faulty snapshots, in one batch. Failure must not hide their flag. */
export async function loadFaultReports(
  rows: FeedbackIdentity[],
  read: (ids: number[]) => PromiseLike<{ data: unknown; error: unknown }>,
): Promise<Record<number, CollarFaultReport>> {
  const wanted = rows.filter(row => row.observation_id !== null && Number.isSafeInteger(row.observation_id)
    && Number.isInteger(row.flags) && ((row.flags ?? 0) & 0x80) !== 0);
  if (!wanted.length) return {};
  try {
    const { data, error } = await read([...new Set(wanted.map(row => row.observation_id!))]);
    if (error || !Array.isArray(data)) return {};
    const reports: Record<number, CollarFaultReport> = {};
    for (const value of data) {
      if (!value || typeof value !== "object") continue;
      const row = value as Record<string, unknown>;
      const identity = wanted.find(item => item.observation_id === row.id && item.device_id === row.device_guid16 && item.flags === row.flags);
      if (!identity) continue;
      reports[identity.device_id] = {
        flags: identity.flags!,
        txReason: typeof row.tx_reason === "number" && Number.isInteger(row.tx_reason) ? row.tx_reason : null,
        resetReason: typeof row.reset_reason === "number" ? row.reset_reason : null,
      };
    }
    return reports;
  } catch {
    return {};
  }
}
