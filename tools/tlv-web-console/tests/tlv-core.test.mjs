import assert from "node:assert/strict";
import test from "node:test";
import {
  POWER_PROFILE_CODES,
  STATUS_CODES,
  TX_REASON_CODES,
  applyRecipeToDevice,
  buildDiagnosticPacket,
  buildTransportWrapper,
  defaultDeviceSettings,
  defaultWrapperSettings,
  generateDeviceCredential,
  parseCredentialBundle,
  previewPacket,
  provisioningSql,
} from "../lib/tlv-core.mjs";

test("builds a valid TLV packet and LTE wrapper", () => {
  const credential = generateDeviceCredential(1001);
  const settings = defaultDeviceSettings(1001);
  const preview = previewPacket(settings, credential, defaultWrapperSettings());

  assert.equal(preview.packet_size_bytes, 40);
  assert.equal(preview.hmac_valid, true);
  assert.equal(preview.decoded.header.device_id, 1001);
  assert.equal(preview.decoded.header.status.name, "OUT");
  assert.equal(preview.wrapper.ingest_path, "cellular_direct");
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

        assert.equal(packet.packet[9] & 0x0f, status, statusName);
        assert.equal(packet.packet[9] >>> 4, powerProfile, profileName);
        assert.equal(packet.packet[11], txReason, reasonName);
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
  for (let cycle = 0; cycle < 16; cycle += 1) {
    const next = applyRecipeToDevice(base, "status_profile_sweep", cycle);
    seen.add(`${next.status}:${next.powerProfile}`);
  }
  assert.equal(seen.size, 16);
});

test("builds a valid LoRa wrapper", () => {
  const credential = generateDeviceCredential(1001);
  const packet = buildDiagnosticPacket(defaultDeviceSettings(1001), credential);
  const wrapper = buildTransportWrapper(packet.payload_b64, {
    ...defaultWrapperSettings(),
    transport: "lora_hub",
    gatewayGuid16: "0016",
    gatewayRxTimeUnix: 1_786_537_811,
  });

  assert.equal(wrapper.ingest_path, "lora_hub");
  assert.equal(wrapper.link_type, "lora");
  assert.equal(wrapper.gateway_guid16, "0016");
  assert.equal(wrapper.gateway_rx_time_unix, 1_786_537_811);
  assert.equal(wrapper.cell_rsrp_dbm, undefined);
});

test("loads legacy and bundle credential JSON", () => {
  const credential = generateDeviceCredential(2001);
  assert.equal(parseCredentialBundle(JSON.stringify([credential])).devices[0].device_id, 2001);
  assert.equal(parseCredentialBundle(JSON.stringify({ schema_version: 1, devices: [credential], gateways: [] })).devices[0].device_id, 2001);
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
