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
function firmwareFunction(name) {
    const start=firmware.indexOf(`bool ${name}(`);
    assert.notEqual(start,-1,`Missing firmware function ${name}`);
    return firmware.slice(start,firmware.indexOf('\n}',start)+2);
}
writeFileSync(source,String.raw`
#include <cassert>
#include <cstdio>
#include <string>
#include <atomic>
#include <vector>
#include "walter_policy.h"
#include "walter_http.h"
constexpr uint32_t utc=1787911200;
struct FakePreferences {
    uint32_t stored=1;
    bool fail=false;
    bool begin(const char*,bool) {return !fail;}
    uint32_t getUInt(const char*,uint32_t) {return stored;}
    size_t putUInt(const char*,uint32_t value) {if(fail)return 0;stored=value;return sizeof(value);}
} sequenceStore;
bool sequenceReady=false;
uint32_t sequenceNext=0,sequenceEnd=0;
${firmwareFunction('nextSequence')}
const char* WALTER_APN="test.apn";
const char* WALTER_APN_USER="";
const char* WALTER_APN_PASSWORD="";
int WALTER_APN_AUTH=0;
const char* WALTER_BEARER_TOKEN="synthetic-token-for-offline-tests-only";
const char* WALTER_TLS_CA_PEM="-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----";
uint8_t hmacKey[32]={1};
${firmwareFunction('safeAtString')}
${firmwareFunction('credentialsReady')}
constexpr unsigned WALTER_MODEM_SIM_STATE_READY=0, WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR=1;
struct WalterModemRsp {unsigned result=0,type=0;struct {unsigned simState=0,cmeError=0;} data;};
std::atomic<bool> cancelRequested{false};
uint64_t fakeMs=0;
uint64_t monotonicMs(){return fakeMs;}
bool pauseMs(uint32_t ms){fakeMs+=ms;return !cancelRequested.load();}
struct QuietSerial {template<typename... T>void printf(const char*,T...) {}} Serial;
struct SimModem {
    std::vector<int> states;size_t calls=0;
    bool getSIMState(WalterModemRsp* rsp) {
        const int state=calls<states.size()?states[calls]:-1;++calls;
        if(state<0){rsp->result=1;rsp->type=1;rsp->data.cmeError=14;return false;}
        rsp->data.simState=state;return true;
    }
} modem;
${firmwareFunction('waitForSimReady')}
int main() {
    modem.states={-1,-1,0};assert(waitForSimReady() && modem.calls==3);
    modem.calls=0;modem.states={1};assert(!waitForSimReady() && modem.calls==1); // PIN required: no guessing/retry.
    modem.calls=0;modem.states={};fakeMs=0;assert(!waitForSimReady() && fakeMs==10000);
    modem.calls=0;cancelRequested=true;assert(!waitForSimReady() && modem.calls==0);cancelRequested=false;
    assert(walter::plausibleUtc(utc,utc));
    assert(!walter::plausibleUtc(3155760003LL,utc)); // Observed factory/default 2070 clock.
    assert(!walter::plausibleUtc(utc-86401,utc));
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
    WALTER_APN="";assert(!credentialsReady());WALTER_APN="test.apn";
    hmacKey[0]=0;assert(!credentialsReady());hmacKey[0]=1;
    WALTER_APN_AUTH=1;assert(!credentialsReady());WALTER_APN_AUTH=0;
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
const [hex,json]=run(executable,[]).trim().split(/\r?\n/);
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
console.log('Walter PASS: credential gates, NVS reservations/reboots/write failures, five-profile cadence, home/away, boot/forced LTE, GNSS validity/staleness, fault flags, strict receipts, C++ HMAC -> web workbench -> Supabase parser. No network or serial traffic.');
