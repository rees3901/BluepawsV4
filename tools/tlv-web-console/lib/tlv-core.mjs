import crypto from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

export const DEFAULT_ENDPOINT = "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position";
export const HEADER_SIZE = 32;
export const AUTH_TAG_SIZE = 8;
export const MAX_TLV_SIZE = 24;
export const PROTOCOL_VERSION = 2;
export const CLOUD_DESTINATION_ID = 0x0000;
export const BROADCAST_ID = 0xFFFF;
export const MAX_PHYSICAL_ID = 65_534;

export const STATUS_CODES = {
  HOME: 0,
  OUT: 1,
  LOST: 2,
  ERROR: 3,
};

export const POWER_PROFILE_CODES = {
  POWER_SAVE: 0,
  NORMAL: 1,
  ACTIVE: 2,
  LOST_ALERT: 3,
  DEBUG: 4,
};

export const TX_REASON_CODES = {
  TELEMETRY: 0,
  ACK: 1,
  PING: 2,
  INTERRUPT: 3,
  BOOT: 4,
  ALERT: 5,
  CONFIG: 6,
  WAKE_CHECKIN: 7,
};

export const FLAG_MASKS = {
  GNSS_VALID: 0x01,
  FIX_3D: 0x02,
  LOW_BATTERY: 0x04,
  HOME_BEACON_SEEN: 0x08,
  GEOFENCE_BREACHED: 0x10,
  CHARGING: 0x20,
  STALE_FIX: 0x40,
  ERROR_PRESENT: 0x80,
};

export const KNOWN_TLVS = {
  fw_ver: { type: 0x04, length: 2, label: "Firmware version" },
  reset_reason: { type: 0x06, length: 1, label: "Reset reason" },
  uptime_s: { type: 0x10, length: 4, label: "Uptime seconds" },
  activity_score: { type: 0x13, length: 1, label: "Activity score" },
  acked_msg_seq_id: { type: 0x20, length: 2, label: "Acked message sequence" },
  profile: { type: 0xF1, length: 1, label: "Power profile command/ACK" },
};

export const RECIPES = {
  manual: {
    label: "Manual",
    description: "Use the values shown in each device row.",
    count: 5,
    interval: 5,
    movementMetres: 200,
  },
  basic_sunny_day: {
    label: "Basic sunny day",
    description: "Valid LTE telemetry with gentle movement and normal status/profile.",
    count: 10,
    interval: 2,
    movementMetres: 50,
  },
  moving_pet: {
    label: "Moving pet",
    description: "Valid telemetry with a stronger random walk.",
    count: 12,
    interval: 2,
    movementMetres: 200,
  },
  status_profile_sweep: {
    label: "Status/profile sweep",
    description: "Cycles every status and power-profile code for end-to-end GUI verification.",
    count: 20,
    interval: 2,
    movementMetres: 50,
  },
  lora_sunny_day: {
    label: "LoRa relay sunny day",
    description: "Valid header-only packets relayed through the selected gateway.",
    count: 10,
    interval: 2,
    movementMetres: 50,
    transport: "lora_hub",
  },
  bad_day: {
    label: "Bad day — only 2 of 10 valid",
    description: "Mostly corrupt HMAC tags; useful for rejection checks.",
    count: 10,
    interval: 1,
    movementMetres: 100,
  },
  duplicate_retry_storm: {
    label: "Duplicate retry storm",
    description: "Sends the exact same packet repeatedly.",
    count: 6,
    interval: 1,
    movementMetres: 0,
  },
  sequence_rollover: {
    label: "Sequence rollover",
    description: "Starts at sequence 65533 and wraps through zero.",
    count: 5,
    interval: 1,
    movementMetres: 25,
  },
  radio_fade: {
    label: "LTE radio fade",
    description: "Valid packets with progressively worse link measurements.",
    count: 10,
    interval: 2,
    movementMetres: 0,
  },
};

export function defaultDeviceSettings(deviceId, index = 0) {
  return {
    enabled: true,
    deviceId,
    destinationId: CLOUD_DESTINATION_ID,
    sequence: randomInt(1, 65_000),
    timestamp: nowUnix(),
    status: STATUS_CODES.OUT,
    powerProfile: POWER_PROFILE_CODES.NORMAL,
    txReason: TX_REASON_CODES.TELEMETRY,
    flags: FLAG_MASKS.GNSS_VALID | FLAG_MASKS.FIX_3D,
    latitude: round7(51.907055 + index * 0.00035),
    longitude: round7(-2.25666 - index * 0.00035),
    driftMetres: 300,
    batteryMv: 3900,
    accuracyM: 8,
    fixAgeS: 0,
    satelliteCount: 9,
    tagMode: "valid",
    customTagHex: "",
    includeTlvs: false,
    knownTlvs: {
      fw_ver: "1.2",
      reset_reason: 1,
      uptime_s: 60,
      activity_score: 42,
      acked_msg_seq_id: 0,
      profile: "",
    },
    customTlvs: [],
  };
}

export function defaultWrapperSettings() {
  return {
    endpoint: DEFAULT_ENDPOINT,
    transport: "cellular_direct",
    gatewayGuid16: "0010",
    gatewayRxTimeUnix: nowUnix(),
    linkRssiDbm: -104,
    linkSnrDb: 7.0,
    cellRsrpDbm: -104,
    cellRsrqDb: -9.5,
    cellSinrDb: 7.0,
  };
}

export async function loadCredentialBundle(filePath) {
  const raw = await fs.readFile(filePath, "utf8");
  return parseCredentialBundle(raw);
}

export async function saveCredentialBundle(filePath, bundle) {
  await fs.mkdir(path.dirname(filePath), { recursive: true });
  await fs.writeFile(filePath, `${JSON.stringify(normalizeBundle(bundle), null, 2)}\n`, "utf8");
}

export function parseCredentialBundle(input) {
  const raw = typeof input === "string" ? JSON.parse(input) : input;
  const bundle = Array.isArray(raw)
    ? { schema_version: 1, devices: raw, gateways: [] }
    : raw;
  if (!bundle || typeof bundle !== "object" || Array.isArray(bundle)) {
    throw new Error("credentials JSON must contain an object bundle or legacy device array");
  }
  if ((bundle.schema_version ?? 1) !== 1) throw new Error("schema_version must be 1");
  const devices = Array.isArray(bundle.devices) ? bundle.devices.map(normalizeDeviceCredential) : [];
  const gateways = Array.isArray(bundle.gateways) ? bundle.gateways.map(normalizeGatewayCredential) : [];
  assertUnique(devices.map((device) => device.device_id), "device_id");
  assertUnique(gateways.map((gateway) => gateway.gateway_guid16), "gateway_guid16");
  return { schema_version: 1, devices, gateways };
}

export function normalizeBundle(bundle) {
  return parseCredentialBundle(bundle);
}

export function summarizeBundle(bundle) {
  const normalized = normalizeBundle(bundle);
  return {
    schema_version: 1,
    devices: normalized.devices.map((device) => ({
      device_id: device.device_id,
      bearer_preview: maskSecret(device.bearer_token),
      hmac_preview: maskSecret(device.hmac_key_b64),
    })),
    gateways: normalized.gateways.map((gateway) => ({
      gateway_guid16: gateway.gateway_guid16,
      display_name: gateway.display_name,
      bearer_preview: maskSecret(gateway.bearer_token),
    })),
  };
}

export function generateDeviceCredential(deviceId) {
  const id = toInteger(deviceId, "device ID");
  validateCollarId(id, "device ID");
  return {
    device_id: Number(deviceId),
    bearer_token: crypto.randomBytes(32).toString("base64url"),
    hmac_key_b64: crypto.randomBytes(32).toString("base64"),
  };
}

export function generateGatewayCredential(gatewayGuid16, displayName = "Bluepaws Test Hub") {
  const normalized = normalizeGatewayGuid16(gatewayGuid16);
  const cleanName = String(displayName || "").trim();
  if (cleanName.length < 1 || cleanName.length > 80) {
    throw new Error("gateway display name must contain 1..80 characters");
  }
  return {
    gateway_guid16: normalized,
    display_name: cleanName,
    bearer_token: crypto.randomBytes(32).toString("base64url"),
  };
}

export function upsertDevice(bundle, credential) {
  const normalized = normalizeBundle(bundle);
  const device = normalizeDeviceCredential(credential);
  const devices = normalized.devices.filter((item) => item.device_id !== device.device_id);
  devices.push(device);
  devices.sort((left, right) => left.device_id - right.device_id);
  return { ...normalized, devices };
}

export function deleteDevice(bundle, deviceId) {
  const id = toInteger(deviceId, "device ID");
  const normalized = normalizeBundle(bundle);
  return { ...normalized, devices: normalized.devices.filter((device) => device.device_id !== id) };
}

export function upsertGateway(bundle, credential) {
  const normalized = normalizeBundle(bundle);
  const gateway = normalizeGatewayCredential(credential);
  const gateways = normalized.gateways.filter((item) => item.gateway_guid16 !== gateway.gateway_guid16);
  gateways.push(gateway);
  gateways.sort((left, right) => left.gateway_guid16.localeCompare(right.gateway_guid16));
  return { ...normalized, gateways };
}

export function provisioningSql(bundle, householdId, keyVersion = 1) {
  const normalized = normalizeBundle(bundle);
  const cleanHousehold = validateUuid(householdId, "Family/household UUID");
  const version = toInteger(keyVersion, "key version");
  validateRange(version, 1, 32_767, "key version");
  const lines = [
    "-- Generated by the Bluepaws TLV web console.",
    "-- Contains plaintext HMAC material until Vault encrypts it.",
    "-- Run once in the Supabase SQL Editor, then do not keep copies of this SQL.",
    "begin;",
    "",
  ];
  for (const device of normalized.devices) {
    const tokenHash = sha256Hex(Buffer.from(device.bearer_token, "utf8"));
    const secretName = `bluepaws-device-${device.device_id}-hmac-v${version}`;
    lines.push(
      "insert into public.devices (device_id, household_id, display_name, enabled)",
      `values (${device.device_id}, '${cleanHousehold}'::uuid, 'Device ${device.device_id}', true)`,
      "on conflict (device_id) do update",
      "set enabled = true;",
      "",
      "insert into public.device_ingest_credentials (device_id, token_hash, enabled, rotated_at)",
      `values (${device.device_id}, '${tokenHash}', true, now())`,
      "on conflict (device_id) do update",
      "set token_hash = excluded.token_hash, enabled = true, rotated_at = now();",
      "",
      "with new_secret as (",
      "  select vault.create_secret(",
      `    '${device.hmac_key_b64}',`,
      `    '${secretName}',`,
      `    'Bluepaws TLV HMAC key for device ${device.device_id}, version ${version}'`,
      "  ) as vault_secret_id",
      ")",
      "insert into public.device_hmac_keys (device_id, key_version, vault_secret_id)",
      `select ${device.device_id}, ${version}, vault_secret_id from new_secret;`,
      "",
    );
  }
  for (const gateway of normalized.gateways) {
    const gatewayNumber = Number.parseInt(gateway.gateway_guid16, 16);
    const tokenHash = sha256Hex(Buffer.from(gateway.bearer_token, "utf8"));
    lines.push(
      "insert into public.gateways (gateway_guid16, household_id, display_name, enabled)",
      `values (${gatewayNumber}, '${cleanHousehold}'::uuid, '${escapeSql(gateway.display_name)}', true)`,
      "on conflict (gateway_guid16) do update",
      "set display_name = excluded.display_name,",
      "    enabled = true;",
      "",
      "insert into public.gateway_ingest_credentials (gateway_guid16, token_hash, enabled, rotated_at)",
      `values (${gatewayNumber}, '${tokenHash}', true, now())`,
      "on conflict (gateway_guid16) do update",
      "set token_hash = excluded.token_hash, enabled = true, rotated_at = now();",
      "",
    );
  }
  lines.push("commit;", "");
  return lines.join("\n");
}

export function buildDiagnosticPacket(deviceSettings, credential) {
  const device = normalizeDeviceCredential(credential);
  const settings = normalizeDeviceSettings(deviceSettings, device.device_id);
  return encodePacket(settings, decodeHmacKey(device.hmac_key_b64), buildTlvEntries(settings));
}

// No credential registry, bearer token, wrapper or network operation is involved.
// A missing key produces an explicitly unsigned diagnostic packet with a zero tag.
export function buildStandalonePacket(input, hmacKey = null, tlvBytes = Buffer.alloc(0)) {
  const settings = normalizeDeviceSettings({ ...input, tagMode: "valid" }, 1001, true);
  if (hmacKey !== null && hmacKey.length !== 32) throw new Error("HMAC key must contain exactly 32 bytes");
  const tlvs = Buffer.from(tlvBytes);
  if (tlvs.length > MAX_TLV_SIZE) throw new Error(`TLV section allows at most ${MAX_TLV_SIZE} bytes`);
  decodeTlvs(tlvs);
  return encodePacket(settings, hmacKey, tlvs);
}

function encodePacket(settings, hmacKey, tlvs) {
  const body = Buffer.alloc(HEADER_SIZE + tlvs.length);
  body.writeUInt8(PROTOCOL_VERSION, 0);
  body.writeUInt16LE(settings.deviceId, 1);
  body.writeUInt16LE(settings.destinationId, 3);
  body.writeUInt16LE(settings.sequence, 5);
  body.writeUInt32LE(settings.timestamp, 7);
  body.writeUInt8((settings.powerProfile << 4) | settings.status, 11);
  body.writeUInt8(settings.flags, 12);
  body.writeUInt8(settings.txReason, 13);
  body.writeInt32LE(Math.round(settings.latitude * 10_000_000), 14);
  body.writeInt32LE(Math.round(settings.longitude * 10_000_000), 18);
  body.writeUInt16LE(settings.batteryMv, 22);
  body.writeUInt16LE(settings.accuracyM, 24);
  body.writeUInt16LE(settings.fixAgeS, 26);
  body.writeUInt8(settings.satelliteCount, 28);
  body.writeUInt8(tlvs.length, 31);
  tlvs.copy(body, HEADER_SIZE);
  const expectedTag = hmacKey ? crypto.createHmac("sha256", hmacKey).update(body).digest().subarray(0, AUTH_TAG_SIZE) : null;
  const transmittedTag = expectedTag ? hmacTag(settings, expectedTag) : Buffer.alloc(AUTH_TAG_SIZE);
  const packet = Buffer.concat([body, transmittedTag]);
  return {
    packet,
    body,
    expectedTag,
    transmittedTag,
    payload_b64: packet.toString("base64"),
    packet_hex: packet.toString("hex").toUpperCase(),
    payload_hash: sha256Hex(packet),
    tlv_length: tlvs.length,
    decoded: decodeTlvPacket(packet, hmacKey),
  };
}

export function buildTransportWrapper(payloadB64, wrapperSettings = {}) {
  const decoded = Buffer.from(String(payloadB64), "base64");
  if (decoded.length < HEADER_SIZE + AUTH_TAG_SIZE || decoded.length !== HEADER_SIZE + decoded[31] + AUTH_TAG_SIZE) {
    throw new Error("payload_b64 does not decode to a valid TLV packet length");
  }
  const transport = wrapperSettings.transport || "cellular_direct";
  const wrapper = {};
  if (transport === "cellular_direct") {
    wrapper.format = "tlv";
    wrapper.ingest_path = "cellular_direct";
    wrapper.link_type = "lte";
    optionalNumber(wrapper, "cell_rsrp_dbm", wrapperSettings.cellRsrpDbm, -200, 0);
    optionalNumber(wrapper, "cell_rsrq_db", wrapperSettings.cellRsrqDb, -100, 0);
    optionalNumber(wrapper, "cell_sinr_db", wrapperSettings.cellSinrDb, -100, 100);
  } else if (transport === "lora_hub" || transport === "lora_gateway") {
    wrapper.format = "tlv";
    wrapper.ingest_path = "lora_gateway";
    wrapper.link_type = "lora";
    wrapper.gateway_guid16 = normalizeGatewayGuid16(wrapperSettings.gatewayGuid16);
    wrapper.gateway_rx_time_unix = toInteger(wrapperSettings.gatewayRxTimeUnix ?? nowUnix(), "gateway receive timestamp");
  } else {
    throw new Error("transport must be cellular_direct, lora_hub, or lora_gateway");
  }
  optionalNumber(wrapper, "link_rssi_dbm", wrapperSettings.linkRssiDbm, -200, 0);
  optionalNumber(wrapper, "link_snr_db", wrapperSettings.linkSnrDb, -100, 100);
  wrapper.payload_b64 = payloadB64;
  return wrapper;
}

export function previewPacket(deviceSettings, credential, wrapperSettings = {}) {
  const built = buildDiagnosticPacket(deviceSettings, credential);
  const wrapper = buildTransportWrapper(built.payload_b64, wrapperSettings);
  const compactJsonBytes = Buffer.byteLength(JSON.stringify(wrapper), "utf8");
  return {
    payload_b64: built.payload_b64,
    packet_hex: groupHex(built.packet_hex),
    packet_size_bytes: built.packet.length,
    tlv_length_bytes: built.tlv_length,
    expected_tag_hex: built.expectedTag.toString("hex").toUpperCase(),
    transmitted_tag_hex: built.transmittedTag.toString("hex").toUpperCase(),
    hmac_valid: crypto.timingSafeEqual(built.expectedTag, built.transmittedTag),
    payload_hash: built.payload_hash,
    decoded: built.decoded,
    wrapper,
    wrapper_size_bytes: compactJsonBytes,
  };
}

export async function sendPacket({ deviceSettings, credential, gatewayCredential, wrapperSettings, endpoint, timeoutSeconds = 15 }) {
  const preview = previewPacket(deviceSettings, credential, wrapperSettings);
  const transport = wrapperSettings.transport || "cellular_direct";
  const token = ["lora_hub", "lora_gateway"].includes(transport)
    ? normalizeGatewayCredential(gatewayCredential).bearer_token
    : normalizeDeviceCredential(credential).bearer_token;
  validateBearerToken(token, "bearer token");
  const cleanEndpoint = String(endpoint || DEFAULT_ENDPOINT).trim();
  if (!cleanEndpoint.startsWith("https://")) throw new Error("endpoint must use HTTPS");
  const headers = {
    Authorization: `Bearer ${token}`,
    "Content-Type": "application/json",
    "User-Agent": "bluepaws-tlv-web-console/1",
  };
  const requestBody = JSON.stringify(preview.wrapper);
  const requestPreview = {
    method: "POST",
    url: cleanEndpoint,
    headers: {
      ...headers,
      Authorization: `Bearer ${maskSecret(token)}`,
    },
    body: preview.wrapper,
    body_size_bytes: Buffer.byteLength(requestBody, "utf8"),
  };
  const started = Date.now();
  const response = await fetch(cleanEndpoint, {
    method: "POST",
    headers,
    body: requestBody,
    signal: AbortSignal.timeout(Math.max(1, Number(timeoutSeconds)) * 1000),
  });
  const text = await response.text();
  let body;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    body = { raw: text };
  }
  return {
    status: response.status,
    ok: response.ok,
    elapsed_ms: Date.now() - started,
    request: requestPreview,
    response: body,
    preview,
  };
}

export function applyRecipeToDevice(settings, recipeKey, cycleIndex) {
  const next = { ...settings };
  if (recipeKey === "basic_sunny_day") {
    next.status = STATUS_CODES.OUT;
    next.powerProfile = POWER_PROFILE_CODES.NORMAL;
    next.txReason = TX_REASON_CODES.TELEMETRY;
    next.tagMode = "valid";
  } else if (recipeKey === "status_profile_sweep") {
    const statuses = [STATUS_CODES.HOME, STATUS_CODES.OUT, STATUS_CODES.LOST, STATUS_CODES.ERROR];
    const profiles = [POWER_PROFILE_CODES.NORMAL, POWER_PROFILE_CODES.POWER_SAVE, POWER_PROFILE_CODES.ACTIVE, POWER_PROFILE_CODES.LOST_ALERT, POWER_PROFILE_CODES.DEBUG];
    const reasons = Object.values(TX_REASON_CODES);
    next.status = statuses[cycleIndex % statuses.length];
    next.powerProfile = profiles[Math.floor(cycleIndex / statuses.length) % profiles.length];
    next.txReason = reasons[cycleIndex % reasons.length];
    next.tagMode = "valid";
  } else if (recipeKey === "bad_day") {
    next.tagMode = cycleIndex === 1 || cycleIndex === 6 ? "valid" : "corrupt";
  } else if (recipeKey === "sequence_rollover" && cycleIndex === 0) {
    next.sequence = 65_533;
    next.tagMode = "valid";
  } else if (recipeKey === "duplicate_retry_storm") {
    next.tagMode = "valid";
  } else if (recipeKey === "radio_fade") {
    next.tagMode = "valid";
  } else if (recipeKey === "moving_pet" || recipeKey === "lora_sunny_day") {
    next.tagMode = "valid";
  }
  return next;
}

export function advanceDeviceSettings(settings, { cycleIndex = 0, intervalSeconds = 1, movementMetres = 0, freezePacket = false } = {}) {
  if (freezePacket) return { ...settings };
  const next = { ...settings };
  next.sequence = (toInteger(settings.sequence, "sequence") + 1) & 0xffff;
  next.timestamp = Math.min(0xffff_ffff, nowUnix() + Math.round(cycleIndex * Number(intervalSeconds || 0)));
  if (movementMetres > 0) {
    const [lat, lon] = driftCoordinates(Number(settings.latitude), Number(settings.longitude), Number(movementMetres));
    next.latitude = lat;
    next.longitude = lon;
  }
  next.batteryMv = varyInteger(Number(settings.batteryMv), 3, 0, 65_535);
  next.accuracyM = varyInteger(Number(settings.accuracyM), 2, 0, 65_535);
  if (Number(settings.fixAgeS) !== 65_535) next.fixAgeS = varyInteger(Number(settings.fixAgeS), 1, 0, 65_534);
  if (Number(settings.satelliteCount) !== 255) next.satelliteCount = varyInteger(Number(settings.satelliteCount), 1, 0, 254);
  return next;
}

export function applyWrapperRecipe(wrapper, recipeKey, cycleIndex) {
  const next = { ...wrapper };
  if (recipeKey === "lora_sunny_day") next.transport = "lora_hub";
  if (recipeKey === "radio_fade") {
    next.linkRssiDbm = Number(next.linkRssiDbm ?? -104) - cycleIndex * 3;
    next.linkSnrDb = Number(next.linkSnrDb ?? 7) - cycleIndex;
    next.cellRsrpDbm = Number(next.cellRsrpDbm ?? -104) - cycleIndex * 3;
    next.cellRsrqDb = Number(next.cellRsrqDb ?? -9.5) - cycleIndex * 0.7;
    next.cellSinrDb = Number(next.cellSinrDb ?? 7) - cycleIndex * 1.2;
  }
  next.gatewayRxTimeUnix = nowUnix();
  return next;
}

export function decodeTlvPacket(packet, hmacKey = null) {
  const raw = Buffer.isBuffer(packet) ? packet : Buffer.from(packet);
  if (raw.length < HEADER_SIZE + AUTH_TAG_SIZE) throw new Error("packet is too short");
  const tlvLength = raw[31];
  if (tlvLength > MAX_TLV_SIZE) throw new Error(`TLV section allows at most ${MAX_TLV_SIZE} bytes`);
  if (raw.length !== HEADER_SIZE + tlvLength + AUTH_TAG_SIZE) throw new Error("packet length does not match tlv_len");
  if (raw[0] !== PROTOCOL_VERSION) throw new Error(`protocol version must be ${PROTOCOL_VERSION}`);
  const statusProfile = raw[11];
  const flags = raw[12];
  const transmittedTag = raw.subarray(raw.length - AUTH_TAG_SIZE);
  const authenticatedBytes = raw.subarray(0, raw.length - AUTH_TAG_SIZE);
  const expectedTag = hmacKey ? crypto.createHmac("sha256", hmacKey).update(authenticatedBytes).digest().subarray(0, AUTH_TAG_SIZE) : null;
  return {
    packet: {
      size_bytes: raw.length,
      header_size_bytes: HEADER_SIZE,
      tlv_length_bytes: tlvLength,
      authentication_tag_size_bytes: AUTH_TAG_SIZE,
      sha256: sha256Hex(raw),
    },
    header: {
      protocol_version: raw[0],
      source_id16: raw.readUInt16LE(1),
      destination_id16: raw.readUInt16LE(3),
      device_id: raw.readUInt16LE(1),
      message_sequence: raw.readUInt16LE(5),
      timestamp_unix: raw.readUInt32LE(7),
      status: namedCode(STATUS_CODES, statusProfile & 0x0f),
      power_profile: namedCode(POWER_PROFILE_CODES, statusProfile >>> 4),
      flags: {
        raw: flags,
        hex: `0x${flags.toString(16).padStart(2, "0").toUpperCase()}`,
        set: Object.entries(FLAG_MASKS).filter(([, mask]) => (flags & mask) !== 0).map(([name]) => name),
      },
      tx_reason: namedCode(TX_REASON_CODES, raw[13]),
      position: {
        latitude: round7(raw.readInt32LE(14) / 10_000_000),
        longitude: round7(raw.readInt32LE(18) / 10_000_000),
        battery_mv: raw.readUInt16LE(22),
        accuracy_m: raw.readUInt16LE(24),
        fix_age_s: raw.readUInt16LE(26) === 65_535 ? null : raw.readUInt16LE(26),
        satellite_count: raw[28] === 255 ? null : raw[28],
      },
      reserved_bytes_hex: raw.subarray(29, 31).toString("hex").toUpperCase(),
    },
    tlvs: decodeTlvs(raw.subarray(HEADER_SIZE, HEADER_SIZE + tlvLength)),
    authentication: {
      algorithm: "HMAC-SHA256-64",
      tag_hex: transmittedTag.toString("hex").toUpperCase(),
      expected_tag_hex: expectedTag ? expectedTag.toString("hex").toUpperCase() : null,
      valid: expectedTag ? crypto.timingSafeEqual(transmittedTag, expectedTag) : null,
    },
  };
}

function buildTlvEntries(settings) {
  if (!settings.includeTlvs) return Buffer.alloc(0);
  const entries = [];
  const known = settings.knownTlvs || {};
  if (known.fw_ver !== undefined && known.fw_ver !== "") {
    const [major, minor] = String(known.fw_ver).split(".").map((part) => toInteger(part, "firmware version"));
    validateRange(major, 0, 255, "firmware major");
    validateRange(minor, 0, 255, "firmware minor");
    entries.push(encodeKnownTlv(0x04, (major << 8) | minor, 2));
  }
  for (const [name, spec] of Object.entries(KNOWN_TLVS)) {
    if (name === "fw_ver") continue;
    if (known[name] === undefined || known[name] === "") continue;
    entries.push(encodeKnownTlv(spec.type, toInteger(known[name], name), spec.length));
  }
  for (const custom of settings.customTlvs || []) {
    const type = parseHexByte(custom.type, "custom TLV type");
    const value = Buffer.from(String(custom.valueHex || "").replace(/^0x/i, "").replace(/\s+/g, ""), "hex");
    if (!value.length || value.length > 22) throw new Error("custom TLV values must contain 1..22 bytes");
    entries.push(Buffer.concat([Buffer.from([type, value.length]), value]));
  }
  const tlvs = Buffer.concat(entries);
  if (tlvs.length > MAX_TLV_SIZE) throw new Error(`TLV section uses ${tlvs.length} bytes; v1.2 allows at most ${MAX_TLV_SIZE}`);
  return tlvs;
}

function encodeKnownTlv(type, value, length) {
  const maximum = 2 ** (length * 8) - 1;
  validateRange(value, 0, maximum, `TLV 0x${type.toString(16)}`);
  const buffer = Buffer.alloc(2 + length);
  buffer[0] = type;
  buffer[1] = length;
  buffer.writeUIntLE(value, 2, length);
  return buffer;
}

function decodeTlvs(tlvBytes) {
  const result = [];
  const seenKnown = new Set();
  let offset = 0;
  while (offset < tlvBytes.length) {
    if (offset + 2 > tlvBytes.length) throw new Error("TLV is missing its length byte");
    const type = tlvBytes[offset];
    const length = tlvBytes[offset + 1];
    const value = tlvBytes.subarray(offset + 2, offset + 2 + length);
    if (value.length !== length) throw new Error(`TLV 0x${type.toString(16)} has invalid length`);
    const spec = Object.entries(KNOWN_TLVS).find(([, item]) => item.type === type);
    if (spec && length !== spec[1].length) throw new Error(`TLV ${spec[0]} must contain ${spec[1].length} bytes`);
    if (spec && seenKnown.has(type)) throw new Error(`TLV ${spec[0]} is duplicated`);
    if (spec) seenKnown.add(type);
    const entry = {
      type: `0x${type.toString(16).padStart(2, "0").toUpperCase()}`,
      length_bytes: length,
      raw_value_hex: value.toString("hex").toUpperCase(),
      name: spec ? spec[0] : "unknown",
      value: spec ? value.readUIntLE(0, value.length) : value.toString("hex").toUpperCase(),
    };
    if (type === 0x04) entry.value = `${(entry.value >>> 8) & 0xff}.${entry.value & 0xff}`;
    result.push(entry);
    offset += 2 + length;
  }
  return result;
}

function normalizeDeviceSettings(input, fallbackDeviceId, allowHubSource = false) {
  const settings = input || {};
  const deviceId = toInteger(settings.deviceId ?? fallbackDeviceId, "device ID");
  if (allowHubSource) validateRange(deviceId, 1, MAX_PHYSICAL_ID, "source ID");
  else validateCollarId(deviceId, "device ID");
  return {
    ...settings,
    deviceId,
    destinationId: boundedInteger(settings.destinationId ?? CLOUD_DESTINATION_ID, 0, BROADCAST_ID, "destination ID"),
    sequence: boundedInteger(settings.sequence, 0, 65_535, "message sequence"),
    timestamp: boundedInteger(settings.timestamp ?? nowUnix(), 0, 0xffff_ffff, "timestamp"),
    status: boundedInteger(settings.status, 0, 3, "status"),
    powerProfile: boundedInteger(settings.powerProfile, 0, 4, "power profile"),
    txReason: boundedInteger(settings.txReason, 0, 7, "TX reason"),
    flags: boundedInteger(settings.flags, 0, 255, "flags"),
    latitude: boundedNumber(settings.latitude, -90, 90, "latitude"),
    longitude: boundedNumber(settings.longitude, -180, 180, "longitude"),
    batteryMv: boundedInteger(settings.batteryMv, 0, 65_535, "battery millivolts"),
    accuracyM: boundedInteger(settings.accuracyM, 0, 65_535, "accuracy metres"),
    fixAgeS: boundedInteger(settings.fixAgeS, 0, 65_535, "fix age seconds"),
    satelliteCount: boundedInteger(settings.satelliteCount, 0, 255, "satellite count"),
    tagMode: settings.tagMode || "valid",
  };
}

function normalizeDeviceCredential(input) {
  const credential = input || {};
  const deviceId = toInteger(credential.device_id, "device_id");
  validateCollarId(deviceId, "device_id");
  const bearerToken = validateBearerToken(credential.bearer_token ?? credential.token, `device ${deviceId} bearer token`);
  const hmacKey = String(credential.hmac_key_b64 || "").trim();
  decodeHmacKey(hmacKey);
  return { device_id: deviceId, bearer_token: bearerToken, hmac_key_b64: hmacKey };
}

function normalizeGatewayCredential(input) {
  const credential = input || {};
  const gatewayGuid16 = normalizeGatewayGuid16(credential.gateway_guid16);
  const bearerToken = validateBearerToken(credential.bearer_token ?? credential.token, `gateway ${gatewayGuid16} bearer token`);
  const displayName = String(credential.display_name || `Gateway ${gatewayGuid16}`).trim();
  return { gateway_guid16: gatewayGuid16, display_name: displayName, bearer_token: bearerToken };
}

function normalizeGatewayGuid16(value) {
  if (typeof value === "number") {
    validateHubId(value, "gateway GUID16");
    return value.toString(16).padStart(4, "0").toUpperCase();
  }
  const text = String(value || "").trim().toUpperCase();
  if (!/^[0-9A-F]{4}$/.test(text)) {
    throw new Error("gateway GUID16 must be exactly four hexadecimal characters");
  }
  validateHubId(Number.parseInt(text, 16), "gateway GUID16");
  return text;
}

function validateCollarId(value, field) {
  validateRange(value, 1, MAX_PHYSICAL_ID, field);
  if ((value & 0x000F) === 0) throw new Error(`${field} must not be a multiple of 16 (reserved for hubs)`);
}

function validateHubId(value, field) {
  validateRange(value, 1, MAX_PHYSICAL_ID, field);
  if ((value & 0x000F) !== 0) throw new Error(`${field} must be a multiple of 16`);
}

function hmacTag(settings, expectedTag) {
  if (settings.tagMode === "valid" || !settings.tagMode) return expectedTag;
  if (settings.tagMode === "corrupt") {
    const corrupted = Buffer.from(expectedTag);
    corrupted[0] ^= 0x01;
    return corrupted;
  }
  if (settings.tagMode === "custom") {
    const custom = Buffer.from(String(settings.customTagHex || "").replace(/\s+/g, ""), "hex");
    if (custom.length !== AUTH_TAG_SIZE) throw new Error("custom HMAC tag must contain exactly 8 bytes");
    return custom;
  }
  throw new Error("HMAC mode must be valid, corrupt, or custom");
}

function decodeHmacKey(value) {
  const key = Buffer.from(String(value || "").trim(), "base64");
  if (key.length !== 32) throw new Error("HMAC key must decode to exactly 32 bytes");
  return key;
}

function validateBearerToken(value, field) {
  const token = String(value || "").trim();
  if (!/^[A-Za-z0-9_-]{32,256}$/.test(token)) throw new Error(`${field} must contain 32..256 URL-safe characters`);
  return token;
}

function optionalNumber(target, key, value, minimum, maximum) {
  if (value === undefined || value === null || value === "") return;
  target[key] = boundedNumber(value, minimum, maximum, key);
}

function boundedInteger(value, minimum, maximum, field) {
  const integer = toInteger(value, field);
  validateRange(integer, minimum, maximum, field);
  return integer;
}

function boundedNumber(value, minimum, maximum, field) {
  const number = Number(value);
  if (!Number.isFinite(number)) throw new Error(`${field} must be numeric`);
  if (number < minimum || number > maximum) throw new Error(`${field} must be from ${minimum} to ${maximum}`);
  return number;
}

function toInteger(value, field) {
  const integer = Number(value);
  if (!Number.isInteger(integer)) throw new Error(`${field} must be an integer`);
  return integer;
}

function validateRange(value, minimum, maximum, field) {
  if (value < minimum || value > maximum) throw new Error(`${field} must be from ${minimum} to ${maximum}`);
}

function parseHexByte(value, field) {
  const text = String(value || "").replace(/^0x/i, "");
  const number = Number.parseInt(text, 16);
  if (!Number.isInteger(number) || number < 0 || number > 255) throw new Error(`${field} must be a byte`);
  return number;
}

function assertUnique(values, field) {
  const seen = new Set();
  for (const value of values) {
    if (seen.has(value)) throw new Error(`${field} ${value} is duplicated`);
    seen.add(value);
  }
}

function namedCode(codes, value) {
  const name = Object.entries(codes).find(([, code]) => code === value)?.[0] || "UNKNOWN";
  return { code: value, name };
}

function maskSecret(value) {
  const text = String(value || "");
  if (text.length <= 10) return "••••";
  return `${text.slice(0, 4)}…${text.slice(-4)}`;
}

function groupHex(hex) {
  return String(hex).match(/.{1,2}/g)?.join(" ") || "";
}

function sha256Hex(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function validateUuid(value, field) {
  const text = String(value || "").trim();
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(text)) {
    throw new Error(`${field} must be a valid UUID`);
  }
  return text.toLowerCase();
}

function escapeSql(value) {
  return String(value).replaceAll("'", "''");
}

function nowUnix() {
  return Math.floor(Date.now() / 1000);
}

function randomInt(minimum, maximum) {
  return minimum + crypto.randomInt(maximum - minimum + 1);
}

function round7(value) {
  return Math.round(Number(value) * 10_000_000) / 10_000_000;
}

function driftCoordinates(latitude, longitude, maximumMetres) {
  const metres = Math.max(0, Math.min(300, Number(maximumMetres || 0)));
  if (!metres) return [round7(latitude), round7(longitude)];
  const earthMetresPerDegree = (Math.PI * 2 * 6_371_000) / 360;
  const radius = metres * Math.sqrt(Math.random());
  const angle = Math.random() * Math.PI * 2;
  const latDelta = (radius * Math.cos(angle)) / earthMetresPerDegree;
  const lonScale = earthMetresPerDegree * Math.max(Math.abs(Math.cos((latitude * Math.PI) / 180)), 1e-6);
  const lonDelta = (radius * Math.sin(angle)) / lonScale;
  return [
    round7(Math.max(-90, Math.min(90, latitude + latDelta))),
    round7(((longitude + lonDelta + 180) % 360) - 180),
  ];
}

function varyInteger(value, maximumDelta, minimum, maximum) {
  const delta = crypto.randomInt(maximumDelta * 2 + 1) - maximumDelta;
  return Math.max(minimum, Math.min(maximum, value + delta));
}
