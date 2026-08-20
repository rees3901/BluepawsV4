import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import test from "node:test";
import { bytesToBase64, parseTlvPacket, parseTlvRequest, sha256Hex, TlvDecodeError } from "./tlv.ts";

const KEY = Uint8Array.from({ length: 32 }, (_value, index) => index);

test("decodes the locked v1.1 header, selected TLVs, and transport wrapper", () => {
  const raw = buildPacket();
  const parsed = parseTlvRequest({
    ingest_path: "lora_hub",
    link_type: "lora",
    gateway_guid16: "0016",
    gateway_rx_time_unix: 1_786_537_811,
    link_rssi_dbm: -92,
    link_snr_db: 6.25,
    payload_b64: bytesToBase64(raw),
  });

  assert.equal(parsed.metadata.gatewayGuid16, 0x0016);
  assert.equal(parsed.packet.deviceGuid16, 0x04a7);
  assert.equal(parsed.packet.messageSequenceId, 10_542);
  assert.equal(parsed.packet.status, 1);
  assert.equal(parsed.packet.powerProfile, 1);
  assert.equal(parsed.packet.latitude, 51.9058165);
  assert.equal(parsed.packet.longitude, -2.2394678);
  assert.deepEqual(parsed.packet.tlvs, {
    fw_ver: 0x0103,
    uptime_s: 86_400,
    activity_score: 42,
    acked_msg_seq_id: 10_541,
    reset_reason: 1,
  });
  assert.deepEqual(parsed.packet.authenticationTag, raw.slice(-8));
});

test("produces a stable backend payload hash without adding bytes to the packet", async () => {
  assert.equal(await sha256Hex(buildPacket()), "8b4e42daccafbffcf76fb12dd2a28d02fbd0f0362ae19b677c1a6b185aa7c535");
});

test("preserves an unknown TLV while safely skipping it", () => {
  const packet = buildPacket(Uint8Array.of(0x99, 0x02, 0xde, 0xad));
  assert.deepEqual(parseTlvPacket(packet).tlvs, {
    unknown: [{ type: 0x99, length: 2, value_hex: "dead" }],
  });
});

test("rejects malformed packets and ambiguous known TLVs", () => {
  assertDecodeError(() => parseTlvPacket(buildPacket().slice(0, -1)), "packet_length_mismatch");
  assertDecodeError(() => parseTlvPacket(buildPacket(Uint8Array.of(0x04, 0x01, 0x01))), "invalid_known_tlv_length");
  assertDecodeError(() => parseTlvPacket(buildPacket(Uint8Array.of(0x06, 0x01, 0x01, 0x06, 0x01, 0x02))), "duplicate_tlv");

  const reserved = buildPacket();
  reserved[27] = 1;
  assertDecodeError(() => parseTlvPacket(reserved), "reserved_header_nonzero");
});

test("rejects wrappers whose path metadata disagrees", () => {
  assertDecodeError(() => parseTlvRequest({
    ingest_path: "cellular_direct",
    link_type: "lora",
    payload_b64: bytesToBase64(buildPacket()),
  }), "transport_mismatch");
});

test("recognizes a valid direct cellular wrapper", () => {
  const parsed = parseTlvRequest({
    ingest_path: "cellular_direct",
    link_type: "lte",
    link_rssi_dbm: -103,
    link_snr_db: 7.5,
    cell_rsrp_dbm: -101,
    cell_rsrq_db: -12,
    cell_sinr_db: 9,
    payload_b64: bytesToBase64(buildPacket()),
  });

  assert.equal(parsed.metadata.ingestPath, "cellular_direct");
  assert.equal(parsed.metadata.linkType, "lte");
  assert.equal(parsed.metadata.gatewayGuid16, null);
});

test("decodes every v1.1 status, power profile, and TX reason code from the header", () => {
  for (let status = 0; status <= 3; status += 1) {
    for (let powerProfile = 0; powerProfile <= 3; powerProfile += 1) {
      for (let txReason = 0; txReason <= 6; txReason += 1) {
        const packet = buildPacket();
        packet[9] = (powerProfile << 4) | status;
        packet[11] = txReason;

        const parsed = parseTlvPacket(packet);

        assert.equal(parsed.status, status);
        assert.equal(parsed.powerProfile, powerProfile);
        assert.equal(parsed.txReason, txReason);
      }
    }
  }
});

test("rejects reserved v1.1 status, power profile, and TX reason codes", () => {
  const reservedStatus = buildPacket();
  reservedStatus[9] = 0x04;
  assertDecodeError(() => parseTlvPacket(reservedStatus), "reserved_status");

  const reservedPowerProfile = buildPacket();
  reservedPowerProfile[9] = 0x40;
  assertDecodeError(
    () => parseTlvPacket(reservedPowerProfile),
    "reserved_power_profile",
  );

  const reservedTxReason = buildPacket();
  reservedTxReason[11] = 7;
  assertDecodeError(() => parseTlvPacket(reservedTxReason), "reserved_tx_reason");
});

function buildPacket(tlvs = selectedTlvs()) {
  const body = new Uint8Array(32 + tlvs.length);
  const view = new DataView(body.buffer);
  view.setUint8(0, 1);
  view.setUint16(1, 0x04a7, true);
  view.setUint16(3, 10_542, true);
  view.setUint32(5, 1_786_537_810, true);
  view.setUint8(9, 0x11);
  view.setUint8(10, 0x13);
  view.setUint8(11, 0);
  view.setInt32(12, 519_058_165, true);
  view.setInt32(16, -22_394_678, true);
  view.setUint16(20, 3_700, true);
  view.setUint16(22, 12, true);
  view.setUint16(24, 3, true);
  view.setUint8(26, 8);
  view.setUint8(31, tlvs.length);
  body.set(tlvs, 32);

  const tag = createHmac("sha256", KEY).update(body).digest().subarray(0, 8);
  const packet = new Uint8Array(body.length + tag.length);
  packet.set(body);
  packet.set(tag, body.length);
  return packet;
}

function selectedTlvs() {
  return Uint8Array.of(
    0x04, 0x02, 0x03, 0x01,
    0x10, 0x04, 0x80, 0x51, 0x01, 0x00,
    0x13, 0x01, 0x2a,
    0x20, 0x02, 0x2d, 0x29,
    0x06, 0x01, 0x01,
  );
}

function assertDecodeError(action: () => unknown, code: string) {
  assert.throws(action, (error: unknown) => error instanceof TlvDecodeError && error.code === code);
}
