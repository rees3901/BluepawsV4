const FIXED_HEADER_BYTES = 32;
const AUTH_TAG_BYTES = 8;
const MAX_TLV_BYTES = 24;
const MIN_PACKET_BYTES = FIXED_HEADER_BYTES + AUTH_TAG_BYTES;
const MAX_PACKET_BYTES = FIXED_HEADER_BYTES + MAX_TLV_BYTES + AUTH_TAG_BYTES;

const KNOWN_TLVS = new Map<number, { name: string; length: number; read: (view: DataView, offset: number) => number }>([
  [0x04, { name: "fw_ver", length: 2, read: (view, offset) => view.getUint16(offset, true) }],
  [0x06, { name: "reset_reason", length: 1, read: (view, offset) => view.getUint8(offset) }],
  [0x10, { name: "uptime_s", length: 4, read: (view, offset) => view.getUint32(offset, true) }],
  [0x13, { name: "activity_score", length: 1, read: (view, offset) => view.getUint8(offset) }],
  [0x20, { name: "acked_msg_seq_id", length: 2, read: (view, offset) => view.getUint16(offset, true) }],
]);

const WRAPPER_FIELDS = new Set([
  "format",
  "ingest_path",
  "link_type",
  "gateway_guid16",
  "gateway_rx_time_unix",
  "link_rssi_dbm",
  "link_snr_db",
  "cell_rsrp_dbm",
  "cell_rsrq_db",
  "cell_sinr_db",
  "payload_b64",
]);

export type IngestPath = "lora_hub" | "cellular_direct";
type WrapperIngestPath = IngestPath | "lora_gateway";
export type LinkType = "lora" | "lte";

export interface TransportMetadata {
  ingestPath: IngestPath;
  linkType: LinkType;
  gatewayGuid16: number | null;
  gatewayRxTimeUnix: number | null;
  linkRssiDbm: number | null;
  linkSnrDb: number | null;
  cellRsrpDbm: number | null;
  cellRsrqDb: number | null;
  cellSinrDb: number | null;
}

export interface ParsedTlvPacket {
  protocolVersion: 1;
  deviceGuid16: number;
  messageSequenceId: number;
  timeUnix: number;
  status: number;
  powerProfile: number;
  flags: number;
  txReason: number;
  latitude: number | null;
  longitude: number | null;
  gnssValid: boolean;
  batteryMillivolts: number;
  accuracyMetres: number;
  fixAgeSeconds: number;
  satelliteCount: number;
  tlvs: Record<string, unknown>;
  rawBytes: Uint8Array;
  authenticatedBytes: Uint8Array;
  authenticationTag: Uint8Array;
}

export interface ParsedTlvRequest {
  metadata: TransportMetadata;
  packet: ParsedTlvPacket;
}

export class TlvDecodeError extends Error {
  readonly code: string;

  constructor(code: string, message: string) {
    super(message);
    this.code = code;
    this.name = "TlvDecodeError";
  }
}

export function isTlvRequest(value: unknown) {
  return Boolean(value && typeof value === "object" && !Array.isArray(value) && "payload_b64" in value);
}

export function parseTlvRequest(value: unknown): ParsedTlvRequest {
  const body = objectBody(value, "body must be a JSON object");
  rejectUnknownFields(body, WRAPPER_FIELDS);

  if (body.format !== undefined && body.format !== "tlv") {
    fail("invalid_format", "format must be tlv when supplied");
  }

  const ingestPath = body.ingest_path as WrapperIngestPath;
  const linkType = body.link_type;
  if (ingestPath !== "lora_hub" && ingestPath !== "lora_gateway" && ingestPath !== "cellular_direct") {
    fail("invalid_ingest_path", "ingest_path must be lora_gateway, lora_hub, or cellular_direct");
  }
  const normalizedIngestPath: IngestPath = ingestPath === "lora_gateway" ? "lora_hub" : ingestPath;
  if (linkType !== "lora" && linkType !== "lte") {
    fail("invalid_link_type", "link_type must be lora or lte");
  }
  if ((normalizedIngestPath === "lora_hub" && linkType !== "lora") || (normalizedIngestPath === "cellular_direct" && linkType !== "lte")) {
    fail("transport_mismatch", "ingest_path and link_type do not describe the same transport");
  }

  let gatewayGuid16: number | null = null;
  let gatewayRxTimeUnix: number | null = null;
  if (normalizedIngestPath === "lora_hub") {
    if (typeof body.gateway_guid16 !== "string" || !/^[0-9a-fA-F]{4}$/.test(body.gateway_guid16)) {
      fail("invalid_gateway", "gateway_guid16 must be exactly four hexadecimal characters");
    }
    gatewayGuid16 = Number.parseInt(body.gateway_guid16, 16);
    if (gatewayGuid16 === 0) fail("invalid_gateway", "gateway_guid16 0000 is reserved");
    gatewayRxTimeUnix = integer(body.gateway_rx_time_unix, 0, 0xffff_ffff, "gateway_rx_time_unix");
    if (body.cell_rsrp_dbm !== undefined || body.cell_rsrq_db !== undefined || body.cell_sinr_db !== undefined) {
      fail("transport_mismatch", "cellular RF fields are not valid for a LoRa wrapper");
    }
  } else {
    if (body.gateway_guid16 !== undefined || body.gateway_rx_time_unix !== undefined) {
      fail("transport_mismatch", "gateway fields are not valid for a cellular_direct wrapper");
    }
  }

  if (typeof body.payload_b64 !== "string") fail("invalid_base64", "payload_b64 must be a Base64 string");
  const rawBytes = decodeBase64(body.payload_b64);

  return {
    metadata: {
      ingestPath: normalizedIngestPath,
      linkType,
      gatewayGuid16,
      gatewayRxTimeUnix,
      linkRssiDbm: optionalNumber(body.link_rssi_dbm, -200, 0, "link_rssi_dbm"),
      linkSnrDb: optionalNumber(body.link_snr_db, -100, 100, "link_snr_db"),
      cellRsrpDbm: optionalNumber(body.cell_rsrp_dbm, -200, 0, "cell_rsrp_dbm"),
      cellRsrqDb: optionalNumber(body.cell_rsrq_db, -100, 0, "cell_rsrq_db"),
      cellSinrDb: optionalNumber(body.cell_sinr_db, -100, 100, "cell_sinr_db"),
    },
    packet: parseTlvPacket(rawBytes),
  };
}

export function parseTlvPacket(rawBytes: Uint8Array): ParsedTlvPacket {
  if (rawBytes.byteLength < MIN_PACKET_BYTES || rawBytes.byteLength > MAX_PACKET_BYTES) {
    fail("invalid_packet_length", `decoded packet must be ${MIN_PACKET_BYTES}..${MAX_PACKET_BYTES} bytes`);
  }

  const view = new DataView(rawBytes.buffer, rawBytes.byteOffset, rawBytes.byteLength);
  const protocolVersion = view.getUint8(0);
  if (protocolVersion !== 1) fail("unsupported_protocol", "protocol version must be 1");

  const deviceGuid16 = view.getUint16(1, true);
  if (deviceGuid16 === 0) fail("invalid_device", "device_guid16 0000 is reserved");

  const state = view.getUint8(9);
  const status = state & 0x0f;
  const powerProfile = (state >>> 4) & 0x0f;
  if (status > 3) fail("reserved_status", "status uses a reserved v1 value");
  if (powerProfile > 3) fail("reserved_power_profile", "power profile uses a reserved v1 value");

  const txReason = view.getUint8(11);
  if (txReason > 7) fail("reserved_tx_reason", "tx_reason uses a reserved v1 value");

  for (let offset = 27; offset <= 30; offset += 1) {
    if (view.getUint8(offset) !== 0) fail("reserved_header_nonzero", "reserved header bytes must be zero");
  }

  const tlvLength = view.getUint8(31);
  if (tlvLength > MAX_TLV_BYTES) fail("invalid_tlv_length", "tlv_len must be 0..24");
  const expectedLength = FIXED_HEADER_BYTES + tlvLength + AUTH_TAG_BYTES;
  if (rawBytes.byteLength !== expectedLength) {
    fail("packet_length_mismatch", "decoded packet length does not match tlv_len");
  }

  const latitudeE7 = view.getInt32(12, true);
  const longitudeE7 = view.getInt32(16, true);
  const flags = view.getUint8(10);
  const gnssValid = (flags & 0x01) !== 0;
  const latitude = gnssValid ? latitudeE7 / 10_000_000 : null;
  const longitude = gnssValid ? longitudeE7 / 10_000_000 : null;
  if (latitude !== null && (latitude < -90 || latitude > 90)) fail("invalid_latitude", "lat_e7 is outside the valid latitude range");
  if (longitude !== null && (longitude < -180 || longitude > 180)) fail("invalid_longitude", "lon_e7 is outside the valid longitude range");

  const authenticatedLength = FIXED_HEADER_BYTES + tlvLength;
  return {
    protocolVersion: 1,
    deviceGuid16,
    messageSequenceId: view.getUint16(3, true),
    timeUnix: view.getUint32(5, true),
    status,
    powerProfile,
    flags,
    txReason,
    latitude,
    longitude,
    gnssValid,
    batteryMillivolts: view.getUint16(20, true),
    accuracyMetres: view.getUint16(22, true),
    fixAgeSeconds: view.getUint16(24, true),
    satelliteCount: view.getUint8(26),
    tlvs: parseTlvs(view, tlvLength),
    rawBytes,
    authenticatedBytes: rawBytes.slice(0, authenticatedLength),
    authenticationTag: rawBytes.slice(authenticatedLength),
  };
}

export async function sha256Hex(bytes: Uint8Array) {
  const digest = await crypto.subtle.digest("SHA-256", Uint8Array.from(bytes).buffer);
  return bytesToHex(new Uint8Array(digest));
}

export function bytesToBase64(bytes: Uint8Array) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

export function bytesToHex(bytes: Uint8Array) {
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

function parseTlvs(view: DataView, tlvLength: number) {
  const values: Record<string, unknown> = {};
  const unknown: Array<{ type: number; length: number; value_hex: string }> = [];
  const seenKnownTypes = new Set<number>();
  const end = FIXED_HEADER_BYTES + tlvLength;
  let offset = FIXED_HEADER_BYTES;

  while (offset < end) {
    if (offset + 2 > end) fail("truncated_tlv", "TLV is missing its type or length byte");
    const type = view.getUint8(offset);
    const length = view.getUint8(offset + 1);
    const valueOffset = offset + 2;
    if (valueOffset + length > end) fail("truncated_tlv", "TLV value exceeds tlv_len");

    const known = KNOWN_TLVS.get(type);
    if (known) {
      if (seenKnownTypes.has(type)) fail("duplicate_tlv", `TLV ${hexType(type)} appears more than once`);
      if (length !== known.length) fail("invalid_known_tlv_length", `TLV ${hexType(type)} must contain ${known.length} value bytes`);
      seenKnownTypes.add(type);
      values[known.name] = known.read(view, valueOffset);
    } else {
      const bytes = new Uint8Array(view.buffer, view.byteOffset + valueOffset, length);
      unknown.push({ type, length, value_hex: bytesToHex(bytes) });
    }
    offset = valueOffset + length;
  }

  if (unknown.length > 0) values.unknown = unknown;
  return values;
}

function decodeBase64(value: string) {
  if (value.length === 0 || value.length > 88 || value.length % 4 !== 0 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
    fail("invalid_base64", "payload_b64 is not canonical Base64");
  }
  try {
    const binary = atob(value);
    return Uint8Array.from(binary, (character) => character.charCodeAt(0));
  } catch {
    fail("invalid_base64", "payload_b64 cannot be decoded");
  }
}

function objectBody(value: unknown, message: string) {
  if (!value || typeof value !== "object" || Array.isArray(value)) fail("invalid_wrapper", message);
  return value as Record<string, unknown>;
}

function rejectUnknownFields(body: Record<string, unknown>, expected: Set<string>) {
  if (Object.keys(body).some((key) => !expected.has(key))) fail("unknown_wrapper_field", "body contains an unknown wrapper field");
}

function integer(value: unknown, minimum: number, maximum: number, field: string) {
  if (!Number.isInteger(value) || (value as number) < minimum || (value as number) > maximum) {
    fail("invalid_wrapper_field", `${field} must be an integer from ${minimum} to ${maximum}`);
  }
  return value as number;
}

function optionalNumber(value: unknown, minimum: number, maximum: number, field: string) {
  if (value === undefined) return null;
  if (typeof value !== "number" || !Number.isFinite(value) || value < minimum || value > maximum) {
    fail("invalid_wrapper_field", `${field} must be a number from ${minimum} to ${maximum}`);
  }
  return value;
}

function hexType(type: number) {
  return `0x${type.toString(16).padStart(2, "0").toUpperCase()}`;
}

function fail(code: string, message: string): never {
  throw new TlvDecodeError(code, message);
}
