// Offline tests of the actual Walter policy, packet/HMAC and HTTP contract.
// Run pio run -e walter once to install the pinned ArduinoJson dependency.
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { decodeTlvPacket } from './tlv-web-console/lib/tlv-core.mjs';
import { parseTlvRequest } from '../supabase/functions/ingest-position/tlv.ts';
const dir=resolve('.pio/walter-tests');
mkdirSync(dir,{recursive:true});
const source=resolve(dir,'policy.cpp');
const executable=resolve(dir,process.platform==='win32'?'policy.exe':'policy');
const firmware=readFileSync('collar/walter/src/main.cpp','utf8').replaceAll('\r\n','\n');
function firmwareFunction(name, type='bool') {
    const start=firmware.indexOf(`${type} ${name}(`);
    assert.notEqual(start,-1,`Missing firmware function ${name}`);
    return firmware.slice(start,firmware.indexOf('\n}',start)+2);
}
writeFileSync(source,String.raw`
#include <cassert>
#include <cstdio>
#include <string>
#include <atomic>
#include <vector>
#include <cstdarg>
#include <cmath>
#include "walter_policy.h"
#include "walter_http.h"
constexpr uint32_t utc=1787911200;
struct FakePreferences {
    uint32_t stored=1;
    bool fail=false;
    bool begin(const char*,bool) {return !fail;}
    uint32_t getUInt(const char*,uint32_t) {return stored;}
    size_t putUInt(const char*,uint32_t value) {if(fail)return 0;stored=value;return sizeof(value);}
    std::vector<uint8_t> bytes;
    size_t getBytesLength(const char*) {return bytes.size();}
    size_t getBytes(const char*,void* out,size_t size) {
        if(fail||size!=bytes.size())return 0;memcpy(out,bytes.data(),size);return size;
    }
    size_t putBytes(const char*,const void* data,size_t size) {
        if(fail)return 0;bytes.assign((const uint8_t*)data,(const uint8_t*)data+size);return size;
    }
} sequenceStore;
bool stateStoreReady=false;
bool sequenceReady=false;
uint32_t sequenceNext=0,sequenceEnd=0;
constexpr uint32_t WALTER_COMMAND_STATE_MAGIC=0x4250434dUL;
struct WalterCommandState {uint32_t magic;uint16_t sequence;uint8_t profile,reserved;char id[37];uint32_t checksum;};
${firmwareFunction('ensureStateStore')}
${firmwareFunction('commandStateChecksum','uint32_t')}
${firmwareFunction('nextSequence')}
const char* WALTER_APN="test.apn";
const char* WALTER_APN_USER="";
const char* WALTER_APN_PASSWORD="";
int WALTER_APN_AUTH=0;
const char* WALTER_BEARER_TOKEN="synthetic-token-for-offline-tests-only";
const char* WALTER_TLS_CA_PEM="-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----";
uint8_t hmacKey[32]={1};
${firmwareFunction('safeAtString')}
${firmwareFunction('packetCredentialsReady')}
${firmwareFunction('credentialsReady')}
${firmwareFunction('buildPdpAuthCommand')}
${firmwareFunction('registrationState','int')}
constexpr unsigned WALTER_MODEM_SIM_STATE_READY=0, WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR=1;
struct WalterModemRsp {unsigned result=0,type=0;struct {unsigned simState=0,cmeError=0;} data;};
std::atomic<bool> cancelRequested{false};
uint64_t fakeMs=0;
uint64_t monotonicMs(){return fakeMs;}
bool cancelOnPause=false;
bool pauseMs(uint32_t ms){fakeMs+=ms;if(cancelOnPause)cancelRequested=true;return !cancelRequested.load();}
struct QuietSerial {
    std::string log;
    void printf(const char* format,...) {char b[1024];va_list a;va_start(a,format);vsnprintf(b,sizeof(b),format,a);va_end(a);log+=b;}
    void println(const char* s="") {log+=s;log+='\n';}
} console;
unsigned modemCalls=0;
struct SimModem {
    std::vector<int> states;size_t calls=0;
    bool getSIMState(WalterModemRsp* rsp) {
        const int state=calls<states.size()?states[calls]:-1;++calls;
        if(state<0){rsp->result=1;rsp->type=1;rsp->data.cmeError=14;return false;}
        rsp->data.simState=state;return true;
    }
    bool gnssConfig(){++modemCalls;return true;}
    void httpClose(unsigned){++modemCalls;}
} modem;
${firmwareFunction('waitForSimReady')}
std::atomic<bool> offlineBench{true},simulatedHome{false},running{false};
std::atomic<uint32_t> loraTxCount{0},lteSkippedCount{0};
std::atomic<uint32_t> nextWakeUtc{0};
std::atomic<uint8_t> selectedProfile{PROFILE_NORMAL};
bool begun=false,cellularFailure=false,setupOk=true,uploadOk=false;
uint32_t nowUtc=utc;
uint32_t utcNow(){return nowUtc;}
walter::Fix lastFix;
constexpr unsigned WALTER_DEVICE_ID=1010,WALTER_HOME_HUB_ID=16,WALTER_HTTP_PROFILE=1;
bool prepareModem(){++modemCalls;begun=true;return setupOk;}
bool networkOn(){++modemCalls;return false;}
bool beginModem(){++modemCalls;begun=true;return true;}
bool radioOff(){++modemCalls;return true;}
bool acquireFix(){++modemCalls;return false;}
std::vector<uint8_t> sentPacket,uploadedPacket;
// Capture the exact buffer supplied to the encoder; real base64 is checked on COM26.
int mbedtls_base64_encode(unsigned char* dest,size_t,size_t* n,const uint8_t* data,size_t size){
    sentPacket.assign(data,data+size);strcpy(reinterpret_cast<char*>(dest),"fixture");*n=7;return 0;
}
bool upload(const uint8_t* data,uint8_t size){++modemCalls;uploadedPacket.assign(data,data+size);return uploadOk;}
${firmwareFunction('waitForNextCycle')}
void scheduleTests(){
    fakeMs=0;nowUtc=utc;cancelRequested=false;cancelOnPause=false;selectedProfile=PROFILE_NORMAL;
    assert(waitForNextCycle()&&fakeMs==600000&&!nextWakeUtc);
    selectedProfile=PROFILE_ACTIVE;fakeMs=0;
    assert(waitForNextCycle()&&fakeMs==60000&&!nextWakeUtc);
    selectedProfile=PROFILE_NORMAL;fakeMs=0;
    assert(waitForNextCycle()&&fakeMs==600000&&!nextWakeUtc);
    cancelOnPause=true;assert(!waitForNextCycle()&&!nextWakeUtc);
    cancelOnPause=false;cancelRequested=false;
}
${firmwareFunction('testLteOnly')}
${firmwareFunction('transmitLoraStub')}
${firmwareFunction('cycle','void')}
void cycleTests(){
    sequenceReady=false;sequenceStore.fail=false;sequenceStore.stored=1;
    fakeMs=1000;nowUtc=utc;cancelRequested=false;cancelOnPause=false;
    uint64_t lastLte=0;
    lastFix={true,519084900,-22587900,utc,8,9}; // Offline must not pass an old fix as current.
    cycle(true,false,PROFILE_NORMAL,1,1,lastLte);
    assert(loraTxCount==1 && lteSkippedCount==1 && modemCalls==0);
    assert(lastLte==11000 && !cellularFailure && uploadedPacket.empty());
    assert(pkt_tx_reason(sentPacket.data())==TX_BOOT && pkt_msg_seq(sentPacket.data())==1);
    assert(!(pkt_flags(sentPacket.data())&FLAG_GNSS_VALID) && pkt_lat_e7(sentPacket.data())==0);
    assert(console.log.find("TX_COMPLETE result=OK")!=std::string::npos);
    assert(console.log.find("Fallback due")!=std::string::npos);
    cycle(false,false,PROFILE_NORMAL,2,2,lastLte);
    assert(loraTxCount==2 && lteSkippedCount==1 && lastLte==11000 && modemCalls==0);
    nowUtc=0;running=true;cycle(false,true,PROFILE_DEBUG,1,1,lastLte);
    assert(loraTxCount==2 && !running && modemCalls==0);nowUtc=utc;
    cancelOnPause=true;cycle(false,true,PROFILE_DEBUG,1,1,lastLte);
    assert(loraTxCount==3 && lteSkippedCount==1 && modemCalls==0);
    cancelOnPause=false;cancelRequested=false;
    simulatedHome=true;cycle(false,false,PROFILE_NORMAL,3,3,lastLte);
    assert(pkt_tx_reason(sentPacket.data())==TX_WAKE_CHECKIN);
    assert(pkt_flags(sentPacket.data())==FLAG_HOME_BEACON_SEEN);
    simulatedHome=false;
    const auto txBefore=loraTxCount.load();
    sequenceNext=sequenceEnd;sequenceStore.fail=true;cycle(false,true,PROFILE_DEBUG,1,1,lastLte);
    assert(loraTxCount==txBefore && modemCalls==0);sequenceStore.fail=false;
    offlineBench=false;setupOk=false;
    cycle(false,true,PROFILE_NORMAL,1,1,lastLte); // Host UTC permits TX even when modem/TLS setup fails.
    assert(loraTxCount==txBefore+1 && cellularFailure && uploadedPacket.empty());
    setupOk=true;uploadOk=false;
    cycle(false,true,PROFILE_NORMAL,1,1,lastLte);
    assert(loraTxCount==txBefore+2 && sentPacket==uploadedPacket && cellularFailure);
    uploadOk=true;cycle(false,true,PROFILE_NORMAL,1,1,lastLte);
    assert(sentPacket==uploadedPacket && !cellularFailure);
    assert(!transmitLoraStub(nullptr,46));
    assert(!transmitLoraStub(sentPacket.data(),1));
    assert(!transmitLoraStub(sentPacket.data(),BP_MAX_PACKET_SIZE+1));
    cancelRequested=true;assert(!transmitLoraStub(sentPacket.data(),46));cancelRequested=false;
}
void lteOnlyTests(){
    WALTER_TLS_CA_PEM="-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----";
    modemCalls=0;uploadedPacket.clear();console.log.clear();
    const auto txBefore=loraTxCount.load();
    const auto skippedBefore=lteSkippedCount.load();
    offlineBench=true;nowUtc=0;assert(!testLteOnly() && modemCalls==0);
    nowUtc=utc;hmacKey[0]=0;assert(!testLteOnly() && modemCalls==0);hmacKey[0]=1;
    cancelRequested=true;assert(!testLteOnly() && modemCalls==0);cancelRequested=false;
    setupOk=false;assert(!testLteOnly() && modemCalls==1 && uploadedPacket.empty());
    setupOk=true;sequenceNext=sequenceEnd;sequenceStore.fail=true;
    assert(!testLteOnly() && modemCalls==2 && uploadedPacket.empty());sequenceStore.fail=false;
    // A retained real fix must not leak into this intentionally no-GNSS test.
    lastFix={true,519084900,-22587900,utc,8,9};
    uploadOk=false;assert(!testLteOnly() && modemCalls==4 && cellularFailure);
    assert(!(pkt_flags(uploadedPacket.data())&FLAG_GNSS_VALID));
    assert(pkt_flags(uploadedPacket.data())&FLAG_ERROR_PRESENT);
    assert(pkt_lat_e7(uploadedPacket.data())==0 && pkt_lon_e7(uploadedPacket.data())==0);
    assert(pkt_tx_reason(uploadedPacket.data())==TX_INTERRUPT);
    const auto seq=pkt_msg_seq(uploadedPacket.data());
    uploadOk=true;assert(testLteOnly() && modemCalls==6 && !cellularFailure);
    assert(pkt_msg_seq(uploadedPacket.data())==uint16_t(seq+1));
    assert(loraTxCount==txBefore && lteSkippedCount==skippedBefore && offlineBench);
    assert(console.log.find("[LORA-STUB]")==std::string::npos);
    assert(console.log.find("No-fix test packet")!=std::string::npos);
}
namespace modemSetupTests {
struct WalterModemRsp {struct {int rat=0;} data;};
using WalterModemRAT=int;
constexpr int WALTER_MODEM_OPSTATE_NO_RF=4;
constexpr int WALTER_MODEM_CEREG_REPORTS_ENABLED_UE_PSM_WITH_LOCATION_EMM_CAUSE=5;
struct SetupModem {
    int cached=0,active=0,pending=0,state=4;
    bool offOk=true,resetOk=true,stale=false,sendOk=true;
    std::vector<std::string> calls;
    bool getRAT(WalterModemRsp* r){calls.push_back("getRAT");r->data.rat=cached;return true;}
    bool setRAT(int r){assert(state==0);calls.push_back("setRAT");pending=r;return true;}
    bool softReset(){calls.push_back("reset");if(!resetOk)return false;active=pending;return true;}
    bool configCMEErrorReports(){calls.push_back("CME");return true;}
    bool configCEREGReports(int){calls.push_back("CEREG");return true;}
    bool sendCmd(const char* c){calls.push_back(c);if(!strcmp(c,"AT+SQNMODEACTIVE?")&&!stale)cached=active;return sendOk;}
    bool setOpState(int s){calls.push_back("CFUN4");state=s;return true;}
} modem;
bool radioOff(){modem.calls.push_back("CFUN0");if(!modem.offOk)return false;modem.state=0;return true;}
bool waitForSimReady(){modem.calls.push_back("SIM");return true;}
${firmwareFunction('configurePdpAuth')}
${firmwareFunction('selectRat')}
void run(){
    cancelRequested=false;
    assert(selectRat(0) && modem.calls==std::vector<std::string>{"getRAT"});
    modem={};assert(selectRat(1));
    const std::vector<std::string> expected={"getRAT","CFUN0","setRAT","reset","CME","CEREG","AT+SQNMODEACTIVE?","getRAT","CFUN4","SIM"};
    assert(modem.calls==expected && modem.cached==1);
    modem={};modem.offOk=false;assert(!selectRat(1) && modem.calls.size()==2);
    modem={};modem.resetOk=false;assert(!selectRat(1) && modem.calls.back()=="reset");
    modem={};modem.stale=true;assert(!selectRat(1) && modem.calls.back()=="getRAT");
    modem={};cancelRequested=true;assert(!selectRat(1) && modem.calls.empty());cancelRequested=false;
    assert(!selectRat(2) && modem.calls.empty());
    WALTER_APN_AUTH=1;assert(configurePdpAuth() && modem.calls.back()=="AT+CGAUTH=1,1");
    modem.sendOk=false;assert(!configurePdpAuth());
    WALTER_APN_AUTH=0;
}
}
namespace commandWindowTests {
char lastCloudCommandId[37]{};
uint16_t lastCloudCommandSequence=0;
uint64_t lostProfileStartedMs=0;
unsigned polls=0,acks=0;
uint64_t queuedAt=0;
bool failPoll=false,failAck=false;
std::vector<uint8_t> ackPacket;
const char* cloud=R"({"device_id":1010,"command_pending":true,"command":{"id":"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee","sequence_id":17,"type":"set_profile","payload":{"profile":"active"},"expires_unix":1787911300}})";
void response(JsonDocument& r){r.clear();r["format"]="device_commands";r["device_id"]=1010;r["command_pending"]=false;}
bool postJson(JsonDocument& q,JsonDocument& r){
    ++polls;assert(q["format"]=="device_commands" && q["device_id"]==1010);
    if(failPoll)return false;
    response(r);
    if(fakeMs>=queuedAt && !acks){assert(!deserializeJson(r,cloud));r["format"]="device_commands";}
    return true;
}
bool sendPacket(const uint8_t* packet,uint8_t length,JsonDocument& r){
    ++acks;ackPacket.assign(packet,packet+length);if(failAck)return false;
    r["acked_command"]["sequence_id"]=17;r["acked_command"]["status"]="acked";return true;
}
${firmwareFunction('restoreCommandState')}
${firmwareFunction('persistCommandState')}
${firmwareFunction('applyCloudCommand','void')}
${firmwareFunction('listenForLteCommands','void')}
void reset(){
    polls=acks=0;fakeMs=0;queuedAt=3000;failAck=failPoll=false;cancelRequested=false;
    cancelOnPause=false;lastCloudCommandId[0]=0;lastCloudCommandSequence=0;
    sequenceStore.fail=false;sequenceStore.bytes.clear();selectedProfile=PROFILE_NORMAL;console.log.clear();nowUtc=utc;
}
void run(){
    JsonDocument r;walter::ProfileCommand c;
    assert(!deserializeJson(r,cloud));assert(walter::parseProfileCommand(r,1010,utc,c));
    for(const char* field:{"id","sequence_id","expires_unix","type"}){
        assert(!deserializeJson(r,cloud));r["command"].remove(field);assert(!walter::parseProfileCommand(r,1010,utc,c));
    }
    assert(!deserializeJson(r,cloud));assert(!walter::parseProfileCommand(r,1001,utc,c));
    assert(!walter::parseProfileCommand(r,1010,0,c));
    for(unsigned seq:{0u,65536u}){r["command"]["sequence_id"]=seq;assert(!walter::parseProfileCommand(r,1010,utc,c));}
    assert(!deserializeJson(r,cloud));r["command"]["expires_unix"]=utc;assert(!walter::parseProfileCommand(r,1010,utc,c));
    for(const char* profile:{"normal","power_save","active","lost_alert","debug"}){
        assert(!deserializeJson(r,cloud));r["command"]["payload"]["profile"]=profile;assert(walter::parseProfileCommand(r,1010,utc,c));
    }
    assert(!deserializeJson(r,cloud));r["command"]["type"]="reboot";assert(!walter::parseProfileCommand(r,1010,utc,c));
    r["command"]["type"]="enter_lost_alert";assert(walter::parseProfileCommand(r,1010,utc,c)&&c.profile==PROFILE_LOST);
    r["command"]["type"]="exit_lost_alert";assert(walter::parseProfileCommand(r,1010,utc,c)&&c.profile==PROFILE_ACTIVE);
    r["command"]["payload"]["fallback_profile"]="lost_alert";assert(!walter::parseProfileCommand(r,1010,utc,c));
    reset();response(r);listenForLteCommands(r);
    assert(fakeMs==10000 && polls==10 && acks==1 && selectedProfile==PROFILE_ACTIVE);
    uint16_t acked=0;assert(pkt_tlv_get_u16(ackPacket.data(),TLV_ACKED_MSG_SEQ_ID,&acked)&&acked==17);
    assert(pkt_tx_reason(ackPacket.data())==TX_ACK && !(pkt_flags(ackPacket.data())&FLAG_GNSS_VALID));
    assert(pkt_power_profile(ackPacket.data())==PROFILE_ACTIVE);
    reset();queuedAt=9999;response(r);listenForLteCommands(r);assert(acks==1&&fakeMs==10000);
    reset();assert(!deserializeJson(r,cloud));applyCloudCommand(r);auto first=lostProfileStartedMs;
    fakeMs=5000;applyCloudCommand(r);assert(acks==2 && lostProfileStartedMs==first);
    // Simulate a reboot: durable identity/profile restores and the same command
    // is re-ACKed without applying it again.
    lastCloudCommandId[0]=0;lastCloudCommandSequence=0;selectedProfile=PROFILE_NORMAL;
    assert(restoreCommandState() && selectedProfile==PROFILE_ACTIVE && lastCloudCommandSequence==17);
    auto restoredAt=lostProfileStartedMs;applyCloudCommand(r);
    assert(acks==3 && selectedProfile==PROFILE_ACTIVE && lostProfileStartedMs==restoredAt);
    // A different UUID reusing the same sequence is an identity conflict.
    r["command"]["id"]="bbbbbbbb-cccc-dddd-eeee-ffffffffffff";applyCloudCommand(r);
    assert(acks==3 && console.log.find("identity conflict")!=std::string::npos);
    reset();sequenceNext=sequenceEnd;sequenceStore.fail=true;assert(!deserializeJson(r,cloud));applyCloudCommand(r);
    assert(!acks && selectedProfile==PROFILE_NORMAL);sequenceStore.fail=false;
    reset();failAck=true;assert(!deserializeJson(r,cloud));applyCloudCommand(r);assert(acks==1 && selectedProfile==PROFILE_ACTIVE);
    failAck=false;applyCloudCommand(r);assert(acks==2);
    reset();failPoll=true;response(r);listenForLteCommands(r);assert(fakeMs==10000 && polls==1 && !acks);
    reset();cancelOnPause=true;response(r);listenForLteCommands(r);assert(polls==0 && !acks && fakeMs<10000);
    reset();assert(!deserializeJson(r,cloud));r["command"]["type"]="reboot";applyCloudCommand(r);assert(!acks);
    const uint8_t body[]={123,125,0};
    assert(walter::parseHttpBody(r,body,sizeof(body),0)); // zero-length RING still has a body
    assert(walter::parseHttpBody(r,body,sizeof(body),2));
    assert(!walter::parseHttpBody(r,body,sizeof(body),1));
    assert(!walter::parseHttpBody(r,body,2,0)); // full buffer is potentially truncated
    const uint8_t empty[]={0};assert(!walter::parseHttpBody(r,empty,1,0));
    const uint8_t invalid[]="oops";assert(!walter::parseHttpBody(r,invalid,sizeof(invalid),0));
}
}
namespace assistanceTests {
using WMGNSSAssistanceType=unsigned;
constexpr int WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA=9;
struct Item {bool available=true;int timeToUpdate=3600,timeToExpire=7200;};
struct WalterModemRsp {int type=9;struct {Item gnssAssistance[2];}data;};
std::atomic<unsigned> assistanceEvents{0};
unsigned networks=0;
bool networkOk=true;
bool prepareModem(){return true;}
bool radioOff(){return true;}
bool networkOn(){++networks;return networkOk;}
struct Modem {
    Item items[2];bool events=true,queryOk=true;unsigned updates=0;int type=9;
    bool gnssSetUTCTime(uint32_t){return true;}
    bool gnssConfig(){return true;}
    bool gnssGetAssistanceStatus(WalterModemRsp* r){r->type=type;r->data.gnssAssistance[0]=items[0];r->data.gnssAssistance[1]=items[1];return queryOk;}
    bool gnssUpdateAssistance(unsigned i){++updates;if(events){items[i]=Item{};assistanceEvents.fetch_or(1u<<i);}return true;}
} modem;
${firmwareFunction('refreshGnssAssistance')}
void run(){
    cancelRequested=false;cancelOnPause=false;nowUtc=utc;fakeMs=0;
    assert(refreshGnssAssistance()&&networks==0&&modem.updates==0);
    modem.items[0].available=false;modem.items[1].timeToUpdate=0;
    assert(refreshGnssAssistance()&&networks==1&&modem.updates==2);
    modem.items[0].available=false;modem.events=false;fakeMs=0;
    assert(!refreshGnssAssistance()&&fakeMs==60000);
    modem.events=true;networkOk=false;assert(!refreshGnssAssistance());networkOk=true;
    modem.queryOk=false;assert(!refreshGnssAssistance());modem.queryOk=true;
    modem.type=0;assert(!refreshGnssAssistance());modem.type=9;
    cancelRequested=true;assert(!refreshGnssAssistance());cancelRequested=false;
    nowUtc=0;assert(!refreshGnssAssistance());nowUtc=utc;
}
}

namespace settlingTests {
constexpr unsigned WALTER_MODEM_GNSS_MAX_SATS=32, WALTER_MODEM_GNSS_FIX_STATUS_READY=0;
constexpr unsigned WALTER_MODEM_GNSS_SENS_MODE_HIGH=3, WALTER_MODEM_GNSS_ACQ_MODE_HOT_START=1;
constexpr unsigned WALTER_MODEM_GNSS_ACTION_CANCEL=1, BLUEPAWS_BUILD_UNIX_TIME=utc;
constexpr unsigned WALTER_GNSS_TIMEOUT_MS=180000;
constexpr unsigned pdTRUE=1;
unsigned pdMS_TO_TICKS(unsigned n){return n;}
struct Sat {uint8_t satNo=1,signalStrength=35;};
struct WMGNSSFixEvent {
    unsigned status=0;int64_t timestamp=utc;uint32_t timeToFix=2000;
    double estimatedConfidence=80,latitude=51.9,longitude=-2.2;
    uint8_t satCount=9;Sat sats[32];
};
std::vector<WMGNSSFixEvent> events;
size_t eventIndex=0;
bool pending=false,automaticTime=true,rfOk=true,configOk=true,clockOk=true,startOk=true,cancelOk=true,noClock=false;
unsigned cancellations=0,hotStarts=0,clockWrites=0;uint64_t readyAt=0,cancelAt=UINT64_MAX;
int gnssEvents=0;
uint32_t utcNow(){return noClock?0:utc+fakeMs/1000;}
void setUtc(int64_t){++clockWrites;}
void xQueueReset(int){}
unsigned xQueueReceive(int,WMGNSSFixEvent* f,unsigned wait){
    fakeMs+=wait;
    if(fakeMs>=cancelAt)cancelRequested=true;
    if(pending&&fakeMs>=readyAt&&eventIndex<events.size()){
        *f=events[eventIndex++];if(automaticTime)f->timestamp=utc+(readyAt-f->timeToFix)/1000;
        pending=false;return pdTRUE;
    }
    return 0;
}
bool radioOff(){return rfOk;}
struct Modem {
    bool gnssConfig(unsigned=3,unsigned hot=0){if(hot)++hotStarts;return configOk;}
    bool gnssSetUTCTime(uint32_t){return clockOk;}
    bool gnssPerformAction(unsigned action=0){
        if(action){++cancellations;pending=false;return cancelOk;}
        pending=true;readyAt=fakeMs+(eventIndex<events.size()?events[eventIndex].timeToFix:1000);
        return startOk;
    }
}modem;
${firmwareFunction('usableGnssSnapshot')}
${firmwareFunction('gnssSeparationM','double')}
${firmwareFunction('settleGnss')}
${firmwareFunction('acquireFix')}
void reset(){
    fakeMs=0;events.clear();eventIndex=0;pending=false;automaticTime=true;
    rfOk=configOk=clockOk=startOk=cancelOk=true;noClock=false;
    cancellations=hotStarts=clockWrites=0;cancelAt=UINT64_MAX;cancelRequested=false;cancelOnPause=false;
    lastFix={};running=false;
}
WMGNSSFixEvent fix(double uncertainty,double lat=51.9){WMGNSSFixEvent f;f.estimatedConfidence=uncertainty;f.latitude=lat;return f;}
void run(){
    reset();events={fix(57)};events[0].timeToFix=50000;
    assert(acquireFix()&&!clockWrites&&utcNow()==utc+50&&lastFix.utc==utc);
    uint8_t agedPacket[BP_MAX_PACKET_SIZE]{};
    assert(walter::buildPacket(agedPacket,1010,16,1,utcNow(),PROFILE_ACTIVE,false,TX_TELEMETRY,lastFix,false,false,50,hmacKey));
    assert(pkt_fix_age_s(agedPacket)==50); // A 50-second acquisition must not look age zero.
    reset();events={fix(20)};events[0].timeToFix=60000;
    assert(!acquireFix()&&!lastFix.valid&&utcNow()==utc+60);
    reset();automaticTime=false;events={fix(20)};events[0].timestamp=utc+100;
    assert(!acquireFix()&&!lastFix.valid); // Future sample cannot reset the clock either.
    reset();noClock=true;assert(!acquireFix()&&fakeMs==0);
    reset();cancelAt=100;assert(!acquireFix()&&cancellations==1);
    reset();assert(!acquireFix()&&fakeMs==180000&&cancellations==1);
    reset();auto sample=fix(20);assert(usableGnssSnapshot(sample));
    sample.status=1;assert(!usableGnssSnapshot(sample));sample.status=0;
    sample.latitude=NAN;assert(!usableGnssSnapshot(sample));sample.latitude=51.9;
    sample.longitude=181;assert(!usableGnssSnapshot(sample));sample.longitude=-2.2;
    sample.timestamp=utc-60;assert(!usableGnssSnapshot(sample));
    reset();events.assign(25,fix(80));for(auto& e:events)e.timeToFix=100;
    assert(settleGnss(false)&&eventIndex==20&&fakeMs<60000); // Explicit attempt cap.
    reset();events={fix(40),fix(30),fix(20)};
    assert(settleGnss(false)&&eventIndex==3&&lastFix.accuracyM==20&&fakeMs==8000&&!hotStarts);
    assert(lastFix.utc==utc+6); // Never relabel the capture time as selection time.
    reset();events={fix(40),fix(30),fix(20)};
    assert(settleGnss(true)&&hotStarts==2); // First shot is cold/warm, subsequent genuine fixes allow hot start.
    reset();events={fix(80),fix(70),fix(90)};
    assert(settleGnss(false)&&fakeMs==60000&&cancellations==1&&lastFix.accuracyM==70&&lastFix.utc==utc+3);
    reset();events={fix(10),fix(40),fix(30),fix(20)};events[0].timeToFix=20000;
    events[1].timeToFix=20000;events[2].timeToFix=15000;events[3].timeToFix=30000;
    events[0].estimatedConfidence=80;
    assert(settleGnss(false)&&fakeMs==60000&&cancellations==1&&lastFix.accuracyM==30);
    reset();events={fix(10)};
    assert(!settleGnss(false)&&fakeMs==60000&&!lastFix.valid); // The only fix aged to 60 s: no stale fallback.
    reset();events={fix(20),fix(20,52.9),fix(20,52.9)};
    assert(settleGnss(false)&&fakeMs==60000); // Geographic jump prevents early 'consistent' result.
    reset();events={fix(1500),fix(0),fix(NAN),fix(20)};events[3].satCount=3;
    assert(!settleGnss(false)&&fakeMs==60000);
    reset();events={fix(20)};events[0].satCount=255;assert(!settleGnss(false)); // Bound CN0 loop.
    reset();automaticTime=false;events={fix(20)};events[0].timestamp=utc+100;
    assert(!settleGnss(false)); // Future timestamp.
    reset();events={fix(40),fix(30)};cancelAt=3500;
    assert(!settleGnss(false)&&cancellations==1&&!lastFix.valid);
    reset();cancelOk=false;assert(!settleGnss(false)&&cancelRequested&&!running);
    reset();rfOk=false;assert(!settleGnss(false)&&fakeMs==0);
    reset();configOk=false;assert(!settleGnss(false)&&fakeMs==0);
    reset();clockOk=false;assert(!settleGnss(false)&&fakeMs==0);
    reset();startOk=false;assert(!settleGnss(false)&&fakeMs==0);
    reset();noClock=true;assert(!settleGnss(false)&&fakeMs==0);
    reset();cancelRequested=true;assert(!settleGnss(false)&&fakeMs==0);
    reset();
}
}

int main() {
    modemSetupTests::run();
    assert(registrationState("+CEREG: 5,2")==2);
    assert(registrationState("+CEREG: 5,0,,,,0,19")==0);
    assert(registrationState("+CEREG: 3,,,,0,15")==3);
    assert(registrationState("+CEREG: 80")==80);
    assert(registrationState("+CEREG: 5,3,\"1234\",\"00001234\",7,0,15")==3);
    assert(registrationState("+CEREG: 3,\"1234\",\"00001234\",7,0,15")==3);
    assert(registrationState("+CEREG: 5")==5);
    assert(registrationState("+CEREG: 5, 1")==1);
    assert(registrationState("+CEREG: ")==-1);
    assert(registrationState("+CESQ: 99,99")==-1);
    modem.states={-1,-1,0};assert(waitForSimReady() && modem.calls==3);
    modem.calls=0;modem.states={1};assert(!waitForSimReady() && modem.calls==1); // PIN required: no guessing/retry.
    modem.calls=0;modem.states={};fakeMs=0;assert(!waitForSimReady() && fakeMs==10000);
    modem.calls=0;cancelRequested=true;assert(!waitForSimReady() && modem.calls==0);cancelRequested=false;
    assert(walter::plausibleUtc(utc,utc));
    assert(!walter::plausibleUtc(3155760003LL,utc)); // Observed factory/default 2070 clock.
    assert(!walter::plausibleUtc(utc-86401,utc));
    assert(walter::signalQualityAvailable(-132,-195));
    assert(walter::signalQualityAvailable(-140,-195));
    assert(walter::signalQualityAvailable(-43,-25));
    assert(!walter::signalQualityAvailable(115,1080)); // Hardware CESQ unknown sentinel conversion.
    assert(!walter::signalQualityAvailable(115,-100));
    assert(!walter::signalQualityAvailable(-100,1080));
    assert(!walter::signalQualityAvailable(-141,-100));
    assert(!walter::signalQualityAvailable(-42,-100));
    assert(!walter::signalQualityAvailable(-100,-196));
    assert(!walter::signalQualityAvailable(-100,-24));
    uint16_t sequence;
    assert(nextSequence(sequence) && sequence==1 && sequenceStore.stored==257);
    assert(nextSequence(sequence) && sequence==2 && sequenceStore.stored==257);
    sequenceReady=false; // Simulated power loss: unused sequence numbers are skipped.
    assert(nextSequence(sequence) && sequence==257 && sequenceStore.stored==513);
    sequenceReady=false;sequenceStore.fail=true;
    assert(!nextSequence(sequence));
    sequenceStore.fail=false;assert(nextSequence(sequence) && sequence==513);
    sequenceNext=sequenceEnd;sequenceStore.fail=true;assert(!nextSequence(sequence));
    sequenceStore.fail=false;assert(nextSequence(sequence) && sequence==769);
    assert(credentialsReady());
    WALTER_APN="";assert(!credentialsReady() && packetCredentialsReady());WALTER_APN="test.apn";
    hmacKey[0]=0;assert(!credentialsReady());hmacKey[0]=1;
    WALTER_APN_AUTH=1;assert(credentialsReady()); // 1NCE PAP permits empty credentials.
    char authCommand[160]{};
    assert(buildPdpAuthCommand(authCommand,sizeof(authCommand)) && !strcmp(authCommand,"AT+CGAUTH=1,1"));
    WALTER_APN_USER="user";WALTER_APN_PASSWORD="pass";
    assert(buildPdpAuthCommand(authCommand,sizeof(authCommand)) && !strcmp(authCommand,"AT+CGAUTH=1,1,\"user\",\"pass\""));
    WALTER_APN_USER="";WALTER_APN_PASSWORD="";
    assert(!buildPdpAuthCommand(authCommand,8));
    WALTER_APN_USER="bad\"value";assert(!credentialsReady() && !buildPdpAuthCommand(authCommand,sizeof(authCommand)));WALTER_APN_USER="";
    WALTER_APN_PASSWORD="bad\r\nvalue";assert(!credentialsReady() && !buildPdpAuthCommand(authCommand,sizeof(authCommand)));WALTER_APN_PASSWORD="";
    WALTER_APN_AUTH=3;assert(!buildPdpAuthCommand(authCommand,sizeof(authCommand)));WALTER_APN_AUTH=0;
    WALTER_BEARER_TOKEN="bad\r\nAuthorization: injected-token-value";assert(!credentialsReady());
    WALTER_BEARER_TOKEN="synthetic-token-for-offline-tests-only";
    WALTER_TLS_CA_PEM="";assert(!credentialsReady());
    assert(!safeAtString("quote\"",99,true));
    assert(!safeAtString("back\\slash",99,true));
    assert(!safeAtString("toolong",3,true));
    for (const auto& p : BP_PROFILES) {
        for (unsigned cycle=1; cycle<=60; ++cycle) {
            const auto away=walter::decide(p.profile,cycle,cycle,false,false,false,0);
            assert(away.packet && away.gnss && away.lte == (cycle % p.cellular_ratio == 0));
            const auto home=walter::decide(p.profile,cycle,cycle,true,false,false,0);
            if (p.profile == PROFILE_LOST) { assert(home.gnss && home.lte==away.lte); continue; }
            assert(home.gnss == (cycle % p.home_gnss_refresh_ratio == 0));
            assert(home.packet == (home.gnss || cycle % p.wake_checkin_ratio == 0));
            assert(!home.lte);
            assert(!walter::decide(p.profile,cycle,cycle,true,false,false,p.lte_heartbeat_interval_s-1).lte);
            assert(walter::decide(p.profile,cycle,cycle,true,false,false,p.lte_heartbeat_interval_s).lte);
        }
        for (bool home : {false,true}) {
            const auto boot=walter::decide(p.profile,1,1,home,true,false,0);
            assert(boot.packet && boot.gnss && boot.lte && boot.reason==TX_BOOT);
            const auto forced=walter::decide(p.profile,1,1,home,false,true,0);
            assert(forced.packet && forced.gnss && forced.lte && forced.reason==TX_INTERRUPT);
        }
        assert(walter::sleepSeconds(p.profile) == (p.profile==PROFILE_LOST ? 30 : p.sleep_interval_s));
    }
    walter::Fix fix{true,519084900,-22587900,utc,8,9};
    uint8_t key[32]; for(int i=0;i<32;++i)key[i]=i;
    uint8_t packet[BP_MAX_PACKET_SIZE]{};
    auto build=[&](walter::Fix f, uint32_t time=utc, uint8_t reason=TX_TELEMETRY,
                   bool home=false, bool gpsFail=false, bool cellFail=false) {
        return walter::buildPacket(packet,1010,16,42,time,PROFILE_NORMAL,home,reason,f,gpsFail,cellFail,123,key);
    };
    const auto length=build(fix);
    assert(length==46 && pkt_flags(packet)==FLAG_GNSS_VALID && pkt_status(packet)==STATUS_OUT_AND_ABOUT);
    assert(pkt_batt_mV(packet)==0 && pkt_lat_e7(packet)==fix.latE7 && pkt_fix_age_s(packet)==0);
    for(int i=0;i<length;++i)printf("%02x",packet[i]);
    puts("");
    build(fix,utc+59); assert(pkt_flags(packet)&FLAG_GNSS_VALID);
    build(fix,utc+60); assert(!(pkt_flags(packet)&FLAG_GNSS_VALID));
    assert(pkt_flags(packet)&FLAG_STALE_FIX); assert(pkt_flags(packet)&FLAG_ERROR_PRESENT);
    assert(pkt_lat_e7(packet)==0 && pkt_status(packet)==STATUS_ERROR);
    build({},utc,TX_TELEMETRY,false,true); assert(pkt_flags(packet)==FLAG_ERROR_PRESENT);
    assert(pkt_fix_age_s(packet)==65535 && pkt_lat_e7(packet)==0);
    build(fix,utc,TX_WAKE_CHECKIN,true,true); assert(pkt_flags(packet)==FLAG_HOME_BEACON_SEEN);
    assert(pkt_status(packet)==STATUS_HOME && pkt_fix_age_s(packet)==65535 && pkt_lat_e7(packet)==0);
    build(fix,utc,TX_TELEMETRY,false,false,true); assert(pkt_flags(packet)&FLAG_ERROR_PRESENT);
    build(fix,utc-1); assert(!(pkt_flags(packet)&FLAG_GNSS_VALID));
    auto bad=fix;bad.latE7=1000000000;build(bad);assert(!(pkt_flags(packet)&FLAG_GNSS_VALID));
    assert(build(fix,0)==0);
    assert(walter::buildPacket(packet,16,16,1,utc,PROFILE_NORMAL,false,0,fix,false,false,0,key)==0);
    assert(walter::buildPacket(packet,1010,1011,1,utc,PROFILE_NORMAL,false,0,fix,false,false,0,key)==0);
    assert(walter::buildPacket(packet,1010,16,1,utc,PROFILE_UNKNOWN,false,0,fix,false,false,0,key)==0);
    assert(walter::buildPacket(packet,1010,16,65535,utc,PROFILE_LOST,true,0,fix,false,false,0,key));
    assert(pkt_msg_seq(packet)==65535 && pkt_status(packet)==STATUS_LOST);
    JsonDocument receipt;
    const char* valid=R"({"accepted":true,"device_id":1010,"message_id":42,"payload_hash":"test-hash","ingest_path":"cellular_direct","link_type":"lte"})";
    auto reset=[&](){receipt.clear();assert(!deserializeJson(receipt,valid));};
    auto accepted=[&](){return walter::acceptedReceipt(receipt,1010,42,"test-hash");};
    reset();assert(accepted());
    for(const auto field:{"accepted","device_id","message_id","payload_hash","ingest_path","link_type"}) {
        reset();receipt.remove(field);assert(!accepted());
    }
    reset();receipt["accepted"]="true";assert(!accepted());
    reset();receipt["accepted"]=false;assert(!accepted());
    reset();receipt["message_id"]=41;assert(!accepted());
    reset();receipt["device_id"]=1001;assert(!accepted());
    reset();receipt["payload_hash"]="another-packet";assert(!accepted());
    reset();receipt["link_type"]="lora";assert(!accepted());
    reset();receipt["ingest_path"]="lora_hub";assert(!accepted());
    reset();receipt["duplicate"]=true;assert(accepted());
    JsonDocument request;walter::fillRequest(request,"fixture");
    std::string json;serializeJson(request,json);puts(json.c_str());
    scheduleTests();
    cycleTests();
    lteOnlyTests();
    commandWindowTests::run();
    assistanceTests::run();
    settlingTests::run();
    const auto ackLength=walter::buildCommandAck(packet,1010,16,43,17,utc,PROFILE_ACTIVE,false,key);
    for(int i=0;i<ackLength;++i)printf("%02x",packet[i]);puts("");
}
`);
const compiler=process.env.CXX||(process.platform==='win32'?'C:/ProgramData/mingw64/mingw64/bin/g++.exe':'g++');
function run(command,args) {
    const result=spawnSync(command,args,{encoding:'utf8'});
    assert.equal(result.status,0,result.error?.message||result.stderr||result.stdout);
    return result.stdout;
}
run(compiler,['-std=c++17','-Wall','-Wextra','-Ishared/lib/BluepawsProtocol','-Icollar/walter/include',
    '-I.pio/libdeps/walter/ArduinoJson/src',source,'-o',executable]);
const [hex,json,ackHex]=run(executable,[]).trim().split(/\r?\n/);
const ack=decodeTlvPacket(Buffer.from(ackHex,'hex'),Buffer.from(Array.from({length:32},(_,i)=>i)));
assert.equal(ack.authentication.valid,true);
assert.equal(parseTlvRequest({format:'tlv',ingest_path:'cellular_direct',link_type:'lte',payload_b64:Buffer.from(ackHex,'hex').toString('base64')}).packet.tlvs.acked_msg_seq_id,17);
const packet=Buffer.from(hex,'hex');
const decoded=decodeTlvPacket(packet,Buffer.from(Array.from({length:32},(_,i)=>i)));
assert.equal(decoded.authentication.valid,true);
assert.equal(decoded.header.device_id,1010);
assert.equal(decoded.header.position.latitude,51.90849);
assert.deepEqual(decoded.header.flags.set,['GNSS_VALID']);
const wrapper=JSON.parse(json);wrapper.payload_b64=packet.toString('base64');
const parsed=parseTlvRequest(wrapper);
assert.equal(parsed.metadata.ingestPath,'cellular_direct');
assert.equal(parsed.packet.deviceGuid16,1010);
assert.deepEqual(Buffer.from(parsed.packet.rawBytes),packet);
assert.equal(decoded.packet.sha256,createHash('sha256').update(packet).digest('hex'));
writeFileSync(resolve(dir,'packet-fixture.json'),JSON.stringify({hex,wrapper,decoded},null,2));
console.log('Walter PASS: applied-profile sleep scheduling/cancellation, bounded GNSS settling/consistency/fresh selection/hot-start/cancellation tests, GNSS assistance cache/refresh/event timeout/cancellation gates, ten-second LTE polling (including final-deadline command), expiry/type/identity rejection, duplicate ACK handling, stop and failed-poll gates, signed ACK decoded by backend, zero-content-length bounded HTTP read, explicit CGAUTH/PAP validation, CFUN0 RAT switching/reset/readback, CEREG parsing, actual offline/online cycle, isolated LTE-only diagnostic, TX completion, immutable fallback bytes, stop/no-clock gates, credential gates, NVS reservations/reboots/write failures, five-profile cadence, home/away, boot/forced LTE, GNSS validity/staleness, fault flags, strict receipts, C++ HMAC -> web workbench -> Supabase parser. No network or serial traffic.');
