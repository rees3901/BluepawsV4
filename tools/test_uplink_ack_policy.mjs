// Contract test for the separate LoRa uplink receipt ACK and LTE fallback policy.
// Compiles the real shared packet/config headers and checks source ordering in
// the two hardware sketches without opening serial ports or transmitting RF.
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { spawnSync } from 'node:child_process';

const hub = readFileSync('hub/src/main.cpp', 'utf8').replaceAll('\r\n', '\n');
const collar = readFileSync('collar/src/main.cpp', 'utf8').replaceAll('\r\n', '\n');

const handler = hub.match(/static void handlePacket\([^]*?\n}\n\n\/\/ A receipt ACK/)[0];
assert(handler.indexOf('sendUplinkReceiptAck(buf)') < handler.indexOf('OfflineJournal::seal(record)'));
assert.match(handler, /if \(pktType != TX_ACK\)/, 'ACK packets must not create ACK loops');

const hubAckStart = hub.lastIndexOf('static bool sendUplinkReceiptAck(');
const hubAck = hub.slice(hubAckStart, hub.indexOf('\n}\n\n// Build a JSON', hubAckStart) + 2);
assert.match(hubAck, /TLV_ACKED_MSG_SEQ_ID/);
assert.doesNotMatch(hubAck, /TLV_PROFILE|cmdQueue/, 'receipt ACK must remain separate from commands');

const receiveLoop = collar.match(/static void listenForCommands\(\) \{[^]*?\n}\n\nstatic void noteUplinkAckResult/)[0];
assert.match(receiveLoop, /while \(\(uint32_t\)\(millis\(\) - windowStartedMs\) < CMD_LISTEN_WINDOW_MS\)/);
assert.match(receiveLoop, /transmitPacket\(lastTxPacket, lastTxPacketLen/,
  'retry must reuse the stored packet bytes');
assert.match(receiveLoop, /handleReceivedCommand\(rxBuf/,
  'receiver must continue to accept a separate command');

const code = String.raw`
#include <cassert>
#include <cstring>
#include "bp_protocol.h"
#include "bp_config.h"
int main() {
  static_assert(CMD_LISTEN_WINDOW_MS == 15000);
  static_assert(UPLINK_ACK_WAIT_MS == 2000);
  static_assert(UPLINK_MAX_ATTEMPTS == 2);
  assert(bp_profile_config(PROFILE_POWERSAVE)->lora_failed_cycles_before_cellular == 3);
  assert(bp_profile_config(PROFILE_NORMAL)->lora_failed_cycles_before_cellular == 3);
  assert(bp_profile_config(PROFILE_ACTIVE)->lora_failed_cycles_before_cellular == 2);
  assert(bp_profile_config(PROFILE_DEBUG)->lora_failed_cycles_before_cellular == 1);
  assert(bp_profile_config(PROFILE_LOST)->lora_failed_cycles_before_cellular == 1);

  uint8_t report[BP_MAX_PACKET_SIZE], ack[BP_MAX_PACKET_SIZE];
  pkt_init(report, 1001, 16, 412, 0, STATUS_HOME, PROFILE_NORMAL, 0, TX_WAKE_CHECKIN);
  auto reportLen = pkt_finalize(report);
  pkt_init(ack, 16, 1001, 7, 0, STATUS_HOME, PROFILE_NORMAL, 0, TX_ACK);
  assert(pkt_add_tlv_u16(ack, TLV_ACKED_MSG_SEQ_ID, pkt_msg_seq(report)));
  auto ackLen = pkt_finalize(ack);
  uint16_t acked = 0;
  assert(pkt_validate_crc(report, reportLen));
  assert(pkt_validate_crc(ack, ackLen));
  assert(pkt_source_id(ack) == pkt_destination_id(report));
  assert(pkt_destination_id(ack) == pkt_source_id(report));
  assert(pkt_pkt_type(ack) == TX_ACK);
  assert(pkt_tlv_get_u16(ack, TLV_ACKED_MSG_SEQ_ID, &acked) && acked == 412);
}
`;

const dir = resolve('.pio/uplink-ack-tests');
mkdirSync(dir, { recursive: true });
const cpp = resolve(dir, 'uplink_ack.cpp');
const exe = resolve(dir, process.platform === 'win32' ? 'uplink_ack.exe' : 'uplink_ack');
writeFileSync(cpp, code);
const compiler = process.env.CXX
  || (process.platform === 'win32' ? 'C:/ProgramData/mingw64/mingw64/bin/g++.exe' : 'g++');
let result = spawnSync(compiler,
  ['-std=c++17', '-Ishared/lib/BluepawsProtocol', cpp, '-o', exe], { encoding: 'utf8' });
assert.equal(result.status, 0, result.stderr);
result = spawnSync(exe, [], { encoding: 'utf8' });
assert.equal(result.status, 0, result.stdout + result.stderr);
console.log('PASS: separate uplink ACK, 15s receive loop, identical retry and profile LTE thresholds');
