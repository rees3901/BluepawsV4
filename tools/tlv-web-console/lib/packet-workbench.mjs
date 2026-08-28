import { buildStandalonePacket, decodeTlvPacket, HEADER_SIZE } from "./tlv-core.mjs";

export function workbenchDefaults() {
  return { deviceId: 1001, destinationId: 16, sequence: 1, timestamp: Math.floor(Date.now() / 1000),
    status: 1, powerProfile: 1, txReason: 0, flags: 3, latitude: 0, longitude: 0,
    batteryMv: 3700, accuracyM: 8, fixAgeS: 0, satelliteCount: 9, tlvHex: "" };
}

export function readHex(text, label = "Hex") {
  const value = String(text ?? "").trim();
  if (!value) return Buffer.alloc(0);
  // Accept compact hex, byte-separated hex, and 0xNN byte lists. Never strip
  // arbitrary text: Buffer.from(hex) otherwise silently truncates bad input.
  const bytes = /^(?:0x[0-9a-f]{2})(?:[\s,]+0x[0-9a-f]{2})*$/i.test(value)
    ? value.replace(/0x/gi, "").replace(/[\s,]/g, "") : value.replace(/\s/g, "");
  if (!/^(?:[0-9a-f]{2})+$/i.test(bytes)) throw new Error(`${label} must contain complete hexadecimal byte pairs`);
  return Buffer.from(bytes, "hex");
}

function readBase64(text, label = "Base64") {
  const value = text.replace(/\s/g, "");
  if (!value || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
    throw new Error(`${label} must be standard padded base64`);
  }
  const bytes = Buffer.from(value, "base64");
  if (bytes.toString("base64") !== value) throw new Error(`${label} is not canonical base64`);
  return bytes;
}

export function readPacketInput(input, format = "auto") {
  if (typeof input !== "string" || !input.trim()) throw new Error("Paste a TLV packet first");
  if (input.length > 8192) throw new Error("Paste one packet, not a full serial log (maximum 8192 characters)");
  if (!["auto", "hex", "base64"].includes(format)) throw new Error("Choose auto, hex or base64 input");
  let text = input.trim(), claimedLength = null;
  const records = text.split(/\r?\n/).map(line => {
    const collar = line.match(/\[PKT\]\s+(\d+)\s+bytes:\s*(.*)$/i);
    const sniffer = line.match(/\[RX\]\s+Hex:\s*(.*)$/i);
    return collar ? { text: collar[2], length: Number(collar[1]) } : sniffer ? { text: sniffer[1] } : null;
  }).filter(Boolean);
  if (records.length > 1) throw new Error("Multiple packets found; paste one packet at a time");
  if (records.length) {
    if (format === "base64") throw new Error("Serial hex records require Auto or Hex input");
    text = records[0].text; claimedLength = records[0].length; format = "hex";
  }
  if (format === "auto") format = /^[\da-f\s]+$/i.test(text) || /^0x/i.test(text) ? "hex" : "base64";
  const packet = format === "hex" ? readHex(text, "Packet hex") : readBase64(text);
  if (claimedLength != null && claimedLength !== packet.length) throw new Error("Serial byte count does not match the pasted packet");
  return { packet, format: records.length ? "serial hex" : format };
}

function resolveKey(auth, sourceId, devices) {
  const mode = auth?.mode ?? "none";
  if (mode === "none") return null;
  let key;
  if (mode === "loaded") {
    const device = devices.find(item => item.device_id === sourceId);
    if (!device) throw new Error(`No loaded collar HMAC key for source ${sourceId}; decode without verification or supply a custom key`);
    key = readBase64(String(device.hmac_key_b64), "Loaded HMAC key");
  } else if (mode === "custom") {
    const value = String(auth.key ?? "").trim();
    key = /^[\da-f\s]+$/i.test(value) || /^0x/i.test(value) ? readHex(value, "HMAC key") : readBase64(value, "HMAC key");
  } else throw new Error("Unknown HMAC key mode");
  if (key.length !== 32) throw new Error("HMAC key must contain exactly 32 bytes (64 hex digits or base64)");
  return key;
}

export function parseWorkbenchPacket(request, devices = []) {
  const { packet, format } = readPacketInput(request.input, request.format);
  const decoded = decodeTlvPacket(packet); // Validate before using the source ID.
  const key = resolveKey(request.auth, decoded.header.source_id16, devices);
  return packetReport(packet, key ? decodeTlvPacket(packet, key) : decoded, format);
}

export function buildWorkbenchPacket(request, devices = []) {
  const settings = request.settings;
  if (!settings || typeof settings !== "object" || Array.isArray(settings)) throw new Error("Builder fields are required");
  for (const field of Object.keys(workbenchDefaults()).filter(key => key !== "tlvHex")) {
    if (settings[field] == null || String(settings[field]).trim() === "") throw new Error(`${field} is required`);
  }
  const key = resolveKey(request.auth, Number(settings.deviceId), devices);
  const tlvs = readHex(settings.tlvHex, "TLV section");
  const built = buildStandalonePacket(settings, key, tlvs);
  const result = packetReport(built.packet, built.decoded, "built");
  if (!key) {
    result.authentication_status = "Unsigned diagnostic — zero authentication tag; not ready for ingestion";
    result.warnings.unshift("No HMAC key was supplied. The eight-byte tag is a placeholder, not a valid signature.");
  }
  return result;
}

function packetReport(packet, decoded, format) {
  const h = decoded.header, p = h.position, flags = h.flags.raw;
  const hexId = id => `0x${id.toString(16).padStart(4, "0").toUpperCase()} (${id})`;
  const role = id => id === 0 ? "cloud" : id === 65535 ? "broadcast" : id % 16 === 0 ? "hub" : "collar";
  const authentication = decoded.authentication.valid;
  const authenticationStatus = authentication === null ? "Not verified — no HMAC key supplied"
    : authentication ? "HMAC verified with the supplied key" : "HMAC mismatch — packet or key differs";
  const warnings = [];
  if ([0, 65535].includes(h.source_id16)) warnings.push("Source ID is reserved; it cannot identify a physical collar or hub.");
  if (h.reserved_bytes_hex !== "0000") warnings.push("Reserved header bytes are nonzero; production ingestion will reject this packet.");
  for (const field of ["status", "power_profile", "tx_reason"]) if (h[field].name === "UNKNOWN") warnings.push(`Reserved ${field} code ${h[field].code}.`);
  if (!(flags & 1)) warnings.push("GNSS_VALID is clear: coordinate bytes are not a valid current GPS fix.");
  if (flags & 0x40) warnings.push("STALE_FIX is set: the reported GPS fix is stale.");
  if (flags & 0x80) warnings.push("ERROR_PRESENT is set. Other flags are context, not proof of a particular fault cause.");
  if (authentication === false) warnings.push(authenticationStatus);
  const tlvHex = packet.subarray(HEADER_SIZE, HEADER_SIZE + decoded.packet.tlv_length_bytes).toString("hex").toUpperCase().match(/.{2}/g)?.join(" ") ?? "";
  const settings = { deviceId: h.source_id16, destinationId: h.destination_id16,
    sequence: h.message_sequence, timestamp: h.timestamp_unix, status: h.status.code,
    powerProfile: h.power_profile.code, txReason: h.tx_reason.code, flags,
    latitude: p.latitude, longitude: p.longitude, batteryMv: p.battery_mv,
    accuracyM: p.accuracy_m, fixAgeS: p.fix_age_s ?? 65535, satelliteCount: p.satellite_count ?? 255, tlvHex };
  return {
    input_format: format, packet_hex: packet.toString("hex").toUpperCase().match(/.{2}/g).join(" "),
    payload_b64: packet.toString("base64"), packet_size_bytes: packet.length,
    authentication_status: authenticationStatus, warnings, settings, decoded,
    rows: [
      ["Packet", `TLV v1.2 / wire version ${h.protocol_version} · ${packet.length} bytes · TLVs ${decoded.packet.tlv_length_bytes}/24 bytes`],
      ["Source", `${hexId(h.source_id16)} · ${role(h.source_id16)}`],
      ["Destination", `${hexId(h.destination_id16)} · ${role(h.destination_id16)}`],
      ["Sequence", String(h.message_sequence)],
      ["Timestamp", `${new Date(h.timestamp_unix * 1000).toISOString()} · Unix ${h.timestamp_unix}`],
      ["Status / profile", `${h.status.name} (${h.status.code}) · ${h.power_profile.name} (${h.power_profile.code})`],
      ["TX reason", `${h.tx_reason.name} (${h.tx_reason.code})`],
      ["Flags", `${h.flags.hex} · ${h.flags.set.join(", ") || "none"}`],
      ["Coordinates", `${p.latitude.toFixed(7)}, ${p.longitude.toFixed(7)}${flags & 1 ? "" : " · not a valid current fix"}`],
      ["Battery", `${p.battery_mv} mV`],
      ["GPS quality", `Accuracy ${p.accuracy_m} m · fix age ${p.fix_age_s === null ? "unknown (65535)" : p.fix_age_s + " s"} · satellites ${p.satellite_count ?? "unknown (255)"}`],
      ["Reserved bytes", h.reserved_bytes_hex],
      ...decoded.tlvs.map(tlv => [`TLV ${tlv.type} · ${tlv.name}`, `${tlv.value} · ${tlv.length_bytes} byte(s) · hex ${tlv.raw_value_hex}${tlv.name === "reset_reason" ? " · reset diagnostic, not an active fault code" : ""}`]),
    ],
  };
}
