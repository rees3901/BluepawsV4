import type { DeviceReportTableRow } from "./deviceReports";

// Only interpret canonical uplink fields. Never infer measurements from an
// omitted TLV, or decode a board-specific reset number as a universal cause.
function uint(value: unknown, max: number): number | null {
  return typeof value === "number" && Number.isInteger(value) && value >= 0 && value <= max ? value : null;
}

export function packetReportDiagnostics(flags: unknown, reason: number, data: unknown, packetAvailable = true): DeviceReportTableRow[] {
  const tlvs = packetAvailable && data !== null && typeof data === "object" && !Array.isArray(data) ? data as Record<string, unknown> : {};
  const read = (key: string, max: number) => Object.hasOwn(tlvs, key) ? uint(tlvs[key], max) : null;
  const unavailable = (key: string) => Object.hasOwn(tlvs, key) ? "Invalid value" : "Not supplied";
  const bits = packetAvailable ? uint(flags, 255) : null;
  const rows: DeviceReportTableRow[] = [];
  const flag = (field: string, mask: number, description: string, set: string, clear: string) => rows.push({
    field, data: bits === null ? "Not supplied" : bits & mask ? "Set in this packet" : "Not set in this packet",
    description, meaning: bits === null ? "Original packet flags are unavailable. Do not infer a negative state from a dashboard snapshot." : bits & mask ? set : clear,
  });
  flag("Home beacon (packet)", 0x08, "HOME_BEACON_SEEN bit supplied by the collar, separate from the map's Home status.",
    reason === 1 ? "This ACK carries the Home flag, which may reflect earlier beacon evidence. An ACK does not establish a new beacon detection or GPS fix."
      : "The packet carries Home-beacon evidence from its creation time. It does not prove the collar is still Home now, or refresh its GPS position.",
    reason === 7 ? "This wake check-in contains no Home-beacon flag. Do not treat the check-in alone as confirmation of Home presence."
      : "No Home-beacon confirmation in this packet. This alone does not prove the pet is outside; reception or scanning may have missed the beacon.");
  flag("Stale GPS (packet)", 0x40, "Explicit STALE_FIX warning supplied by the collar, not the GUI's calculated GPS age.",
    "The collar explicitly flagged an old fix. Treat the location as last known and check its timestamp; recent contact does not make it current.",
    "The collar did not set its stale-fix bit. GPS may still be old according to the GUI, or absent from a lightweight check-in. This is not proof of a fresh fix.");
  flag("Reported error (packet)", 0x80, "ERROR_PRESENT diagnostic bit from this one collar packet.",
    bits !== null && (bits & 0x40) !== 0 ? "An error and stale-fix warning occur together. Stale GPS accompanies the error, but these bits do not establish its root cause."
      : "The collar reported an error without a specific cause in this bit. Check accompanying diagnostics; do not assume GPS, radio or battery failure.",
    "No ERROR_PRESENT bit in this packet. This does not guarantee all hardware is healthy or that GPS is current.");

  const fw = read("fw_ver", 65535);
  if (fw !== null || reason === 4 || Object.hasOwn(tlvs, "fw_ver")) rows.push({
    field: "Firmware version", data: fw === null ? unavailable("fw_ver") : `${fw >> 8}.${fw & 255}`,
    description: "Firmware major/minor version declared by this packet's optional TLV.",
    meaning: fw === null ? "No usable version was supplied in this report. Do not borrow a version from another device or an older packet."
      : "Use this declared version to match firmware support and release records. It does not identify the exact Git commit, board type or prove the latest firmware is installed.",
  });
  const ack = read("acked_msg_seq_id", 65535);
  if (reason === 1 || Object.hasOwn(tlvs, "acked_msg_seq_id")) rows.push({
    field: "Acknowledged command", data: ack === null ? unavailable("acked_msg_seq_id") : `Sequence ${ack}`,
    description: "Command/message sequence referenced by the ACK TLV, not this report's own sequence counter.",
    meaning: ack === null ? "This ACK cannot be linked to a command by this field. Do not infer which pending command completed."
      : reason !== 1 ? "An acknowledgement ID appeared outside an ACK message. Do not treat it as confirmed command completion without the matching command record."
        : "The collar acknowledges this sequence. Match both collar identity and command record; this alone does not prove a physical action completed or that a newer command was applied.",
  });
  const reset = read("reset_reason", 255);
  if (reason === 4 || Object.hasOwn(tlvs, "reset_reason")) rows.push({
    field: "Reset reason", data: reset === null ? unavailable("reset_reason") : `0x${reset.toString(16).padStart(2,"0").toUpperCase()}`,
    description: "Board-specific reset/cold-start diagnostic code, normally sent in a boot report.",
    meaning: reset === null ? "No usable reset diagnostic was supplied. The cause of a restart is unknown."
      : reason !== 4 ? "Reset metadata is present outside a boot report. It describes a reset, not necessarily the current runtime error. Decode using this firmware and board's reset definitions."
        : "A reset diagnostic was supplied at boot. Its meaning depends on the board and firmware; do not label it watchdog, brownout or a current fault without the matching decoder.", advanced: true,
  });
  const uptime = read("uptime_s", 4294967295);
  rows.push({field: "Reported uptime", data: uptime === null ? unavailable("uptime_s") : `${uptime} seconds`,
    description: "Runtime counter supplied by this packet's optional uptime TLV.",
    meaning: uptime === null ? "No usable runtime counter was supplied; do not infer a restart or treat absence as zero seconds."
      : "Compare reports from the same firmware and board. Sleep may reset or pause the timer; a low or falling counter alone does not prove a crash or continuous time since power-on.", advanced: true});
  rows.push({field: "Raw packet flags", data: bits === null ? "Not supplied" : `0x${bits.toString(16).padStart(2,"0").toUpperCase()}`,
    description: "Original eight-bit diagnostic bitmask before any GUI interpretation.",
    meaning: bits === null ? "No original packet flags are available for this snapshot."
      : "For support/debugging. Charging, geofence and other unset bits are not reliable negative readings unless this firmware implements them.", advanced: true});
  return rows;
}
