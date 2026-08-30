// Compile the real hub queue/ACK functions against deterministic host stubs.
// No serial, radio, network or production data is used.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { resolve } from 'node:path';
const source = readFileSync('hub/src/main.cpp', 'utf8').replaceAll('\r\n', '\n');
function fn(name) {
  const match = source.match(new RegExp(`^static [^\\n]*\\b${name}\\([^;]*?\\) \\{`, 'm'));
  if (!match) throw new Error(`Missing function ${name}`);
  return source.slice(match.index, source.indexOf('\n}', match.index) + 2);
}
function struct(name) { return source.match(new RegExp(`struct ${name} \\{[\\s\\S]*?\\n};`))[0]; }
const code = `
#include <cassert>
#include <atomic>
#include <vector>
#include "bp_protocol.h"
${readFileSync('shared/lib/BluepawsProtocol/bp_config.h','utf8').match(/enum bp_buzzer_pattern_t[^]*?};/)[0]}
${struct('cmd_entry_t')}
${struct('pending_cmd_t')}
constexpr int MAX_PENDING_CMDS=16, CMD_MAX_RETRIES=3, CMD_ACK_TIMEOUT_MS=10000, CMD_LISTEN_WINDOW_MS=15000;
constexpr uint32_t LOCAL_COMMAND_TTL_MS=600000, COMMAND_FEEDBACK_TTL_MS=900000;
constexpr int GATEWAY_GUID16=16, pdTRUE=1;
#define pdMS_TO_TICKS(n) (n)
uint32_t clockMs=1000, commandRxOpportunityUntil=0;
uint32_t millis() {return clockMs;}
uint32_t xTaskGetTickCount() {return clockMs;}
int pendingMutex=1, cmdQueue=1;
bool queueAvailable=true;
int xSemaphoreTake(int,int) {return 1;}
void xSemaphoreGive(int) {}
std::vector<cmd_entry_t> queued;
int xQueueSend(int,const cmd_entry_t* cmd,int) {if(!queueAvailable)return 0; queued.push_back(*cmd); return 1;}
int xQueueSendToFront(int q,const cmd_entry_t* cmd,int t) {return xQueueSend(q,cmd,t);}
struct Logger { template<typename... Args> void printf(const char*,Args...) {} } Serial;
pending_cmd_t pendingCmds[MAX_PENDING_CMDS]{};
std::atomic<uint32_t> cmdSeqCounter{0};
void broadcastCommand(const pending_cmd_t&) {}
${fn('handleAck')}
${fn('noteCommandSent')}
${fn('commandStillPending')}
${fn('queuePendingCommandForDevice')}
${fn('checkPendingAcks')}
${fn('sendCommandFind')}
uint16_t mode(uint16_t target,bp_profile_t profile,uint16_t seq=0,uint32_t age=0) {
 return sendCommandFind(target,PKT_CMD_MODE,profile,0,(bp_buzzer_pattern_t)0,seq,age);
}
void ack(uint16_t collar,uint16_t seq) {
 uint8_t buf[BP_MAX_PACKET_SIZE];
 pkt_init(buf,collar,16,99,0,STATUS_HOME,PROFILE_NORMAL,0,1);
 pkt_add_tlv_u16(buf,TLV_CMD_MSG_ID,seq); pkt_finalize(buf); handleAck(buf);
}
int main() {
 auto seq=mode(1001,PROFILE_ACTIVE); assert(seq);
 auto first=queued.back(); assert(commandStillPending(first));
 assert(pkt_device_id(first.buf)==16 && pkt_destination_id(first.buf)==1001);
 noteCommandSent(first);
 ack(1002,seq); assert(commandStillPending(first));
 ack(1001,seq); assert(!commandStillPending(first));
 assert(!strcmp(pendingCmds[0].state,"acked"));
 assert(mode(1001,PROFILE_ACTIVE,seq)==0); // Late cloud retry cannot revive ACKed command.
 auto second=mode(1001,PROFILE_NORMAL); assert(second);
 auto secondPacket=queued.back();
 assert(mode(1001,PROFILE_ACTIVE)); assert(!commandStillPending(secondPacket));
 auto third=queued.back(); assert(commandStillPending(third));
 clockMs+=599999; assert(commandStillPending(third));
 clockMs++; assert(!commandStillPending(third));
 ack(1001,third.cmdSeq); checkPendingAcks();
 bool expired=false; for(auto &p:pendingCmds) if(p.cmdSeq==third.cmdSeq) expired=!strcmp(p.state,"expired");
 assert(expired);
 auto count=queued.size(); queuePendingCommandForDevice(1001); assert(queued.size()==count);
 assert(mode(1001,PROFILE_ACTIVE,400,600000)==0);
 assert(mode(1001,PROFILE_ACTIVE,401,590000)==401);
 auto cloud=queued.back(); clockMs+=10000; assert(!commandStillPending(cloud)); checkPendingAcks();
 queueAvailable=false; assert(mode(1002,PROFILE_ACTIVE)==0);
 bool failed=false; for(auto &p:pendingCmds) if(p.targetId==1002) failed=!strcmp(p.state,"failed");
 assert(failed);
 puts("PASS: actual hub queue, addressing, ACK identity, supersession, expiry and queue failure");
}
`;
const dir = resolve('.pio/feedback-tests');
mkdirSync(dir, { recursive: true });
const cpp = resolve(dir, 'hub_commands.cpp');
const exe = resolve(dir, process.platform === 'win32' ? 'hub_commands.exe' : 'hub_commands');
writeFileSync(cpp, code);
const compiler = process.env.CXX || (process.platform === 'win32' ? 'C:/ProgramData/mingw64/mingw64/bin/g++.exe' : 'g++');
for (const [command,args] of [[compiler,['-std=c++17','-Ishared/lib/BluepawsProtocol',cpp,'-o',exe]],[exe,[]]]) {
  const result=spawnSync(command,args,{encoding:'utf8'});
  process.stdout.write(result.stdout || ''); process.stderr.write(result.stderr || '');
  if(result.status !== 0) throw result.error || new Error(`Failed ${command}: ${result.status}`);
}
