// Compile actual firmware packet initializer calls and hub policy with stubs.
// No hardware, network, real keys or flash writes.
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { resolve } from 'node:path';

const collar = readFileSync('collar/src/main.cpp', 'utf8').replaceAll('\r\n', '\n');
const hub = readFileSync('hub/src/main.cpp', 'utf8').replaceAll('\r\n', '\n');
const calls = [...collar.matchAll(/pkt_init\(buf, MY_DEVICE_ID,[\s\S]*?;/g)].map(m => m[0]);
assert.equal(calls.length, 7, 'review any new packet send path');
assert.equal(calls.filter(c => c.includes('MY_HOME_HUB_ID')).length, 4);
assert.equal(calls.filter(c => c.includes('destinationId')).length, 3, 'ACKs still address command origin');
const relay = hub.match(/static bool hubProfileAllowsCloudRelay\(\) \{[\s\S]*?\n}/)[0];
const reject = hub.match(/if \(destinationId != BP_DEST_CLOUD[\s\S]*?\n    }/)[0]
  .replace(/Serial.printf\([^;]+;/, '').replace('return;', 'return false;');
const clock = collar.match(/static uint32_t compileTimeUnix\(\) \{[\s\S]*?\n}/)[0];
assert(!clock.includes('__DATE__') && !clock.includes('__TIME__'), 'do not reintroduce local-time/month parsing');

const code = `
#include <cassert>
#include "bp_protocol.h"
#include "bp_hmac_sha256.h"
#include "collar_routing.h"
#define MY_DEVICE_ID 1001
#define BLUEPAWS_BUILD_UNIX_TIME 1787834096UL
enum { HUB_COMM_HOME, HUB_COMM_PORTABLE, HUB_COMM_OFF_GRID };
int hubCommProfile=HUB_COMM_HOME;
constexpr uint16_t GATEWAY_GUID16=16;
${relay}
${clock}
bool accepts(uint16_t destinationId) { ${reject} return true; }
int main() {
  uint8_t buf[BP_MAX_PACKET_SIZE], key[32]{}, tag[8];
  uint16_t messageSeq=1192, destinationId=32;
  uint32_t unixTime=1787834096;
  uint8_t status=STATUS_OUT_AND_ABOUT, currentProfile=PROFILE_NORMAL, flags=3, txReason=TX_INTERRUPT;
  ${calls.map((call, index) => `
  ${call}
  assert(pkt_source_id(buf)==1001);
  assert(pkt_destination_id(buf)==${call.includes('MY_HOME_HUB_ID') ? 'MY_HOME_HUB_ID' : 'destinationId'});
  assert(buf[0]==2 && buf[29]==0 && buf[30]==0);
  assert(pkt_msg_seq(buf)==messageSeq);
  { auto length=pkt_finalize(buf);
    bp_hmac_sha256_truncated8(key,32,buf,length-8,buf+length-8);
    bp_hmac_sha256_truncated8(key,32,buf,length-8,tag);
    assert(!memcmp(tag,buf+length-8,8));
    // Retargeting at the hub or LTE modem would invalidate the original tag.
    buf[3]^=0x10;
    bp_hmac_sha256_truncated8(key,32,buf,length-8,tag);
    assert(memcmp(tag,buf+length-8,8));
  }
  `).join('\n')}
  assert(accepts(16)); assert(!accepts(32)); assert(!accepts(1001));
  assert(accepts(0)); // Legacy compatibility, not the new collar default.
  assert(hubProfileAllowsCloudRelay());
  hubCommProfile=HUB_COMM_PORTABLE; assert(hubProfileAllowsCloudRelay());
  hubCommProfile=HUB_COMM_OFF_GRID; assert(!hubProfileAllowsCloudRelay());
  assert(compileTimeUnix()==1787834096UL); // August UTC, never silently January.
  puts("PASS: seven collar send paths, HMAC coverage, hub routing/modes, UTC clock");
}
`;
const dir = resolve('.pio/routing-tests');
mkdirSync(dir, {recursive:true});
const cpp = resolve(dir, 'routing.cpp');
const exe = resolve(dir, process.platform === 'win32' ? 'routing.exe' : 'routing');
writeFileSync(cpp, code);
const compiler = process.env.CXX || (process.platform === 'win32' ? 'C:/ProgramData/mingw64/mingw64/bin/g++.exe' : 'g++');
const args = ['-std=c++17', '-Ishared/lib/BluepawsProtocol', '-Icollar/include', cpp, '-o', exe];
let result = spawnSync(compiler, ['-DMY_HOME_HUB_ID=16', ...args], {encoding:'utf8'});
assert.equal(result.status, 0, result.stderr);
result = spawnSync(exe, [], {encoding:'utf8'});
assert.equal(result.status, 0, result.stdout + result.stderr);
process.stdout.write(result.stdout);
for (const definition of [null, 0, 1001, 65535, 65536]) {
  result = spawnSync(compiler, [...(definition === null ? [] : ['-DMY_HOME_HUB_ID='+definition]), ...args], {encoding:'utf8'});
  assert.notEqual(result.status, 0, 'missing/invalid hub provisioning must fail compilation');
}
console.log('PASS: missing/invalid affiliated hub IDs fail the build');
