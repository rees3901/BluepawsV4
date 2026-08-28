import assert from "node:assert/strict";
import test from "node:test";
import {
  POWER_PROFILE_CODES,
  STATUS_CODES,
  TX_REASON_CODES,
  applyRecipeToDevice,
  buildDiagnosticPacket,
  buildStandalonePacket,
  buildTransportWrapper,
  defaultDeviceSettings,
  defaultWrapperSettings,
  decodeTlvPacket,
  generateDeviceCredential,
  parseCredentialBundle,
  previewPacket,
  provisioningSql,
} from "../lib/tlv-core.mjs";
import { buildWorkbenchPacket, parseWorkbenchPacket, readPacketInput, workbenchDefaults } from "../lib/packet-workbench.mjs";

const testKey = Buffer.alloc(32, 0x42);
const customAuth = { mode: "custom", key: testKey.toString("hex") };
const workbenchSettings = overrides => ({ ...workbenchDefaults(), timestamp: 1786537811, ...overrides });

test("standalone builder matches the simulator codec without needing a registered device or bearer", () => {
  const settings = workbenchSettings({ destinationId: 0 });
  const diagnostic = buildDiagnosticPacket(settings, {
    device_id: settings.deviceId, bearer_token: "a".repeat(48), hmac_key_b64: testKey.toString("base64"),
  });
  const standalone = buildWorkbenchPacket({ settings, auth: customAuth });
  assert.equal(standalone.payload_b64, diagnostic.payload_b64);
  assert.equal(standalone.decoded.authentication.valid, true);
  const unsigned = buildWorkbenchPacket({ settings: workbenchSettings({ deviceId: 16, destinationId: 1001 }) });
  assert.equal(unsigned.decoded.header.source_id16, 16);
  assert.equal(unsigned.decoded.authentication.valid, null);
  assert.equal(unsigned.decoded.authentication.tag_hex, "0000000000000000");
  assert.match(unsigned.authentication_status, /Unsigned diagnostic/);
  assert.match(unsigned.warnings[0], /placeholder/);
});

test("reader accepts compact/spaced/0x hex, base64 and actual collar/sniffer serial formats", () => {
  const built = buildWorkbenchPacket({ settings: workbenchSettings(), auth: customAuth });
  const hex = built.packet_hex;
  for (const input of [hex, hex.replaceAll(" ", ""), hex.split(" ").map(byte => `0x${byte}`).join(", "),
    built.payload_b64, `\n${built.payload_b64.slice(0, 20)}\n${built.payload_b64.slice(20)}\n`,
    `12:34:56 [PKT] 40 bytes: ${hex}\n`, `[RX] Hex: ${hex}`]) {
    const parsed = parseWorkbenchPacket({ input, auth: customAuth });
    assert.equal(parsed.payload_b64, built.payload_b64);
    assert.equal(parsed.decoded.authentication.valid, true);
  }
});

test("reader rejects malformed encodings, ambiguous serial logs and invalid packet framing", () => {
  const built = buildWorkbenchPacket({ settings: workbenchSettings() });
  for (const [input, format, error] of [
    ["", "auto", /Paste/], ["0", "hex", /byte pairs/], ["02GG", "hex", /byte pairs/],
    ["!!!!", "base64", /base64/], ["AB==", "base64", /canonical/], ["AA", "base64", /padded/],
    ["x".repeat(8193), "auto", /maximum/], [built.packet_hex, "invalid", /Choose/],
    [`[PKT] 41 bytes: ${built.packet_hex}`, "auto", /byte count/],
    [`[RX] Hex: ${built.packet_hex}\n[RX] Hex: ${built.packet_hex}`, "auto", /Multiple packets/],
    [`[RX] Hex: ${built.packet_hex}`, "base64", /Serial hex/],
    ["00", "hex", /too short/],
  ]) assert.throws(() => parseWorkbenchPacket({ input, format }), error);
  const raw = readPacketInput(built.packet_hex).packet;
  assert.throws(() => decodeTlvPacket(Buffer.concat([raw, Buffer.alloc(1)])), /length/);
  raw[0] = 1;
  assert.throws(() => decodeTlvPacket(raw), /protocol version/);
  raw[0] = 2; raw[31] = 25;
  assert.throws(() => decodeTlvPacket(raw), /at most 24/);
});

test("verification is explicit and key lookup does not mutate or disclose credentials", () => {
  const settings = Object.freeze(workbenchSettings());
  const devices = Object.freeze([Object.freeze({ device_id: 1001, hmac_key_b64: testKey.toString("base64") })]);
  const built = buildWorkbenchPacket({ settings, auth: { mode: "loaded" } }, devices);
  const verified = parseWorkbenchPacket({ input: built.payload_b64, auth: { mode: "loaded" } }, devices);
  assert.equal(verified.decoded.authentication.valid, true);
  assert.equal(parseWorkbenchPacket({ input: built.payload_b64 }).decoded.authentication.valid, null);
  const wrongKey = { mode: "custom", key: Buffer.alloc(32, 0x33).toString("base64") };
  const mismatch = parseWorkbenchPacket({ input: built.payload_b64, auth: wrongKey });
  assert.equal(mismatch.decoded.authentication.valid, false);
  assert.match(mismatch.authentication_status, /mismatch/);
  for (const key of [testKey.toString("hex"), testKey.toString("base64")]) assert.ok(!JSON.stringify(verified).includes(key));
  assert.throws(() => buildWorkbenchPacket({ settings, auth: { mode: "loaded" } }), /No loaded/);
  assert.throws(() => buildWorkbenchPacket({ settings, auth: { mode: "custom", key: "01" } }), /32 bytes/);
  assert.throws(() => buildWorkbenchPacket({ settings, auth: { mode: "unexpected" } }), /Unknown HMAC/);
});

test("parse-to-build round trip preserves fields, sentinel values, unknown TLVs and exact byte order", () => {
  const settings = workbenchSettings({ deviceId: "0x0010", destinationId: "0xFFFF", status: 0, powerProfile: 4,
    sequence: 65535, txReason: 4, flags: 0xC8, latitude: -89.1234567, longitude: 179.7654321,
    fixAgeS: 65535, satelliteCount: 255, tlvHex: "20 02 FF FF 06 01 02 04 02 02 01 10 04 FF FF FF FF 13 01 64 F1 01 04" });
  const built = buildWorkbenchPacket({ settings, auth: customAuth });
  const parsed = parseWorkbenchPacket({ input: built.packet_hex, auth: customAuth });
  assert.equal(parsed.settings.fixAgeS, 65535);
  assert.equal(parsed.settings.satelliteCount, 255);
  assert.equal(parsed.settings.flags, 0xC8);
  assert.match(parsed.rows.find(([name]) => name === "GPS quality")[1], /unknown/);
  assert.match(parsed.warnings.join(" "), /GNSS_VALID.*STALE_FIX.*ERROR_PRESENT/);
  assert.equal(buildWorkbenchPacket({ settings: parsed.settings, auth: customAuth }).payload_b64, built.payload_b64);
  for (const tlvHex of ["EE 00 EE 01 42", `EE 16 ${"AB ".repeat(22)}`]) {
    const unknown = buildWorkbenchPacket({ settings: workbenchSettings({ tlvHex }), auth: customAuth });
    const decoded = parseWorkbenchPacket({ input: unknown.payload_b64 });
    assert.equal(buildWorkbenchPacket({ settings: decoded.settings, auth: customAuth }).payload_b64, unknown.payload_b64);
    assert.equal(decoded.decoded.tlvs[0].name, "unknown");
  }
});

test("builder validates ranges and TLV structure instead of silently truncating", () => {
  for (const [tlvHex, error] of [
    ["06", /length byte/], ["06 02 01", /invalid length/], ["06 02 01 02", /must contain 1/],
    ["06 00", /must contain 1/], ["06 01 01 06 01 02", /duplicated/], ["EE 17 " + "00 ".repeat(23), /at most 24/],
  ]) assert.throws(() => buildWorkbenchPacket({ settings: workbenchSettings({ tlvHex }) }), error);
  for (const overrides of [{ deviceId: 0 }, { deviceId: 65535 }, { destinationId: 65536 }, { sequence: 1.2 },
    { timestamp: -1 }, { flags: 256 }, { status: 4 }, { powerProfile: 5 }, { txReason: 8 },
    { latitude: 91 }, { longitude: -181 }, { batteryMv: 65536 }, { satelliteCount: 256 }, { sequence: "" }]) {
    assert.throws(() => buildWorkbenchPacket({ settings: workbenchSettings(overrides) }));
  }
  assert.throws(() => buildStandalonePacket(workbenchSettings(), Buffer.alloc(8)), /32 bytes/);
});

test("reader reports reserved header codes without mistaking decoding for packet acceptance", () => {
  const raw = buildStandalonePacket(workbenchSettings()).packet;
  raw.writeUInt16LE(0, 1); raw[11] = 0xFF; raw[13] = 255; raw[29] = 1;
  const result = parseWorkbenchPacket({ input: raw.toString("base64") });
  assert.equal(result.decoded.authentication.valid, null);
  assert.equal(result.decoded.header.status.name, "UNKNOWN");
  const warnings = result.warnings.join(" ");
  assert.match(warnings, /Source ID is reserved/);
  assert.match(warnings, /Reserved header bytes/);
  assert.match(warnings, /Reserved status code 15/);
  assert.match(warnings, /Reserved power_profile code 15/);
  assert.match(warnings, /Reserved tx_reason code 255/);
});

test("builds a valid TLV packet and LTE wrapper", () => {
  const credential = generateDeviceCredential(1001);
  const settings = defaultDeviceSettings(1001);
  const preview = previewPacket(settings, credential, defaultWrapperSettings());

  assert.equal(settings.driftMetres, 300);
  assert.equal(preview.packet_size_bytes, 40);
  assert.equal(preview.hmac_valid, true);
  assert.equal(preview.decoded.header.protocol_version, 2);
  assert.equal(preview.decoded.header.source_id16, 1001);
  assert.equal(preview.decoded.header.destination_id16, 0);
  assert.equal(preview.decoded.header.device_id, 1001);
  assert.equal(preview.decoded.header.status.name, "OUT");
  assert.equal(preview.wrapper.ingest_path, "cellular_direct");
  assert.equal(preview.wrapper.format, "tlv");
  assert.equal(preview.wrapper.link_type, "lte");
});

test("encodes every visible status, power profile, and TX reason into the header", () => {
  const credential = generateDeviceCredential(1001);
  for (const [statusName, status] of Object.entries(STATUS_CODES)) {
    for (const [profileName, powerProfile] of Object.entries(POWER_PROFILE_CODES)) {
      for (const [reasonName, txReason] of Object.entries(TX_REASON_CODES)) {
        const packet = buildDiagnosticPacket({
          ...defaultDeviceSettings(1001),
          status,
          powerProfile,
          txReason,
        }, credential);

        assert.equal(packet.packet[11] & 0x0f, status, statusName);
        assert.equal(packet.packet[11] >>> 4, powerProfile, profileName);
        assert.equal(packet.packet[13], txReason, reasonName);
        assert.equal(packet.decoded.header.status.name, statusName);
        assert.equal(packet.decoded.header.power_profile.name, profileName);
        assert.equal(packet.decoded.header.tx_reason.name, reasonName);
      }
    }
  }
});

test("status/profile sweep recipe covers all status and profile combinations", () => {
  const base = defaultDeviceSettings(1001);
  const seen = new Set();
  for (let cycle = 0; cycle < 20; cycle += 1) {
    const next = applyRecipeToDevice(base, "status_profile_sweep", cycle);
    seen.add(`${next.status}:${next.powerProfile}`);
  }
  assert.equal(seen.size, 20);
});

test("builds a valid LoRa wrapper", () => {
  const credential = generateDeviceCredential(1001);
  const packet = buildDiagnosticPacket(defaultDeviceSettings(1001), credential);
  const wrapper = buildTransportWrapper(packet.payload_b64, {
    ...defaultWrapperSettings(),
    transport: "lora_hub",
    gatewayGuid16: "0010",
    gatewayRxTimeUnix: 1_786_537_811,
  });

  assert.equal(wrapper.ingest_path, "lora_gateway");
  assert.equal(wrapper.format, "tlv");
  assert.equal(wrapper.link_type, "lora");
  assert.equal(wrapper.gateway_guid16, "0010");
  assert.equal(wrapper.gateway_rx_time_unix, 1_786_537_811);
  assert.equal(wrapper.cell_rsrp_dbm, undefined);
});

test("loads legacy and bundle credential JSON", () => {
  const credential = generateDeviceCredential(2001);
  assert.equal(parseCredentialBundle(JSON.stringify([credential])).devices[0].device_id, 2001);
  assert.equal(parseCredentialBundle(JSON.stringify({ schema_version: 1, devices: [credential], gateways: [] })).devices[0].device_id, 2001);
});

test("enforces the v1.2 physical ID role allocation", () => {
  assert.throws(() => generateDeviceCredential(1008), /multiple of 16/);
  assert.throws(() => buildTransportWrapper(
    buildDiagnosticPacket(defaultDeviceSettings(1001), generateDeviceCredential(1001)).payload_b64,
    { ...defaultWrapperSettings(), transport: "lora_hub", gatewayGuid16: "0016" },
  ), /multiple of 16/);
});

test("provisioning SQL hashes bearer tokens and includes HMAC material for Vault", () => {
  const credential = {
    device_id: 2001,
    bearer_token: "a".repeat(48),
    hmac_key_b64: Buffer.alloc(32, 1).toString("base64"),
  };
  const sql = provisioningSql(
    { schema_version: 1, devices: [credential], gateways: [] },
    "6e799f91-3027-4c8f-b239-09531939e79e",
    1,
  );

  assert.match(sql, /insert into public\.devices/);
  assert.match(sql, /vault\.create_secret/);
  assert.match(sql, new RegExp(credential.hmac_key_b64.replace(/[+/=]/g, "\\$&")));
  assert.doesNotMatch(sql, /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/);
});
