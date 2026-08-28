// Run with node --test tools/test_hub_device_cards.mjs (Node + C++ compiler).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
import {collarFault} from '../web/src/lib/collarFault.ts';

// Execute the real renderer, not a reimplementation. The tiny DOM surface keeps
// this regression runnable without a browser or a powered-up hub.
const source = readFileSync(new URL('../hub/data/app.js', import.meta.url), 'utf8');
const renderer = source.slice(source.indexOf('    function renderDeviceCard(dev) {'),
    source.indexOf('    // Human-friendly time display'));

function fixture(roaming, bleResults = {}) {
    const cards = new Map();
    const context = vm.createContext({
        document: {
            getElementById: id => id === 'deviceCards'
                ? { appendChild: card => cards.set(card.id, card) } : cards.get(id),
            createElement: () => ({ innerHTML: '', classList: { add() {}, remove() {} },
                querySelector: selector => selector === '.card-avatar-edit' ? {} : null }),
        },
        Date, performance, expandedCardIds: [], followedDeviceId: null,
        hubPortableMode: roaming, bleResults, deviceLogs: {},
        getCollarStatus: () => ({ css: 'home', emoji: '', label: 'Home' }),
        formatDistFromHub: () => '1 km', formatLastSeen: () => 'just now', formatAge: () => 'just now',
        renderBatteryBars: () => 'Battery', renderSignalBars: () => 'LoRa',
        renderBleProximity: rssi => `BLE:${rssi}`, ICON_HOME_DIST: '', ICON_STOPWATCH: '', ICON_ANTENNA: '',
        buildActionButtons: () => '', wireActionButtons() {},
    });
    vm.runInContext(readFileSync(new URL('../hub/data/feedback.js', import.meta.url), 'utf8'), context);
    vm.runInContext(source.slice(source.indexOf('    function escapeHtml(value) {'),
        source.indexOf('    function hubDetailRows(data) {')), context);
    vm.runInContext(renderer, context);
    const device = id => ({ id, lastUpdate: Date.now(), avatar: {color:'#1d9bf0',emoji:'🐾'},
        data: {id, name:`Device ${id}`,profile:'Normal',status:'Home',hasGps:false,batt:3900,rssi:-94,snr:8,verification:'pending'} });
    return {context, cards, device};
}

for (const mode of ['home', 'portable', 'off_grid']) {
    test(`${mode}: first packet and timer refresh render a no-GPS card`, () => {
        const f = fixture(mode !== 'home');
        const dev = f.device(1001);
        f.context.renderDeviceCard(dev);
        f.context.renderDeviceCard(dev); // same path as five-second age refresh
        assert.equal(f.cards.size, 1);
        assert.match(f.cards.get('card-1001').innerHTML, /Device 1001/);
        assert.match(f.cards.get('card-1001').innerHTML, /verification pending/);
        assert.doesNotMatch(f.cards.get('card-1001').innerHTML, /BLE:/);
    });
}

test('local hub badge follows confirmed reporting profile, not pending target', () => {
    const f=fixture(false);
    const dev=f.device(-16);
    Object.assign(dev.data,{entity:'hub',verification:undefined,hub:{reporting_profile:'power_save',desired_reporting_profile:'active'}});
    for(const [profile,css,label] of [['power_save','power','Power Save'],['normal','normal','Normal'],['active','active','Active'],[undefined,'normal','Normal']]) {
        dev.data.hub.reporting_profile=profile;
        f.context.renderDeviceCard(dev);
        const html=f.cards.get('card--16').innerHTML;
        assert.ok(html.includes(`class="card-profile profile-${css}">${label}</span>`));
        assert.doesNotMatch(html,/collar-awake|💤/);
    }
});

test('BLE proximity belongs to the card device, never a different row', () => {
    const f = fixture(true, {1001:{rssi:-44},1002:{rssi:-80}});
    for (const id of [1002,1001]) f.context.renderDeviceCard(f.device(id));
    assert.match(f.cards.get('card-1001').innerHTML, /BLE:-44/);
    assert.doesNotMatch(f.cards.get('card-1001').innerHTML, /BLE:-80/);
    assert.match(f.cards.get('card-1002').innerHTML, /BLE:-80/);
    delete f.context.bleResults[1001];
    f.context.renderDeviceCard(f.device(1001));
    assert.doesNotMatch(f.cards.get('card-1001').innerHTML, /BLE:/);
});

test('expanded GPS card can be refreshed and collapsed while Off-Grid', () => {
    const f = fixture(true, {1001:{rssi:-51}});
    const dev = f.device(1001);
    Object.assign(dev.data, {hasGps:true,lat:51.9,lon:-2.2});
    f.context.expandedCardIds = [1001];
    f.context.renderDeviceCard(dev);
    assert.match(f.cards.get('card-1001').innerHTML, /51\.90000, -2\.20000/);
    assert.match(f.cards.get('card-1001').innerHTML, /Message Log/);
    f.context.renderDeviceCard(dev);
    f.context.expandedCardIds = [];
    f.context.renderDeviceCard(dev);
    assert.doesNotMatch(f.cards.get('card-1001').className, /expanded/);
});

test('reset diagnostics do not become faults, including in Lost Alert', () => {
    const f=fixture(true); const dev=f.device(1001);
    Object.assign(dev.data,{profile:'Lost Alert',status:'Lost',resetReason:3,errorPresent:false});
    f.context.renderDeviceCard(dev);
    let html=f.cards.get('card-1001').innerHTML;
    assert.match(html,/profile-lost/);
    assert.doesNotMatch(html,/error-badge/);
    dev.data.errorPresent=true;
    f.context.renderDeviceCard(dev);
    assert.match(f.cards.get('card-1001').innerHTML,/Reported fault — cause unspecified/);
});

test('local fault labels match cloud rules for every flags byte and report type',()=>{
    const f=fixture(false);
    for(let flags=0;flags<256;flags++) for(const txReason of [0,1,2,3,4,5,6,7,undefined]) {
        const report={flags,txReason,resetReason:2};
        assert.equal(JSON.stringify(f.context.HubFeedback.fault(report)),JSON.stringify(collarFault(report)));
    }
});

test('local fault reasons refresh and clear independently of the retained GPS position',()=>{
    const f=fixture(false); const dev=f.device(1001);
    Object.assign(dev.data,{errorPresent:true,flags:0xc4,txReasonCode:4,resetReason:2,resetReasonPresent:true});
    f.context.renderDeviceCard(dev);
    assert.match(f.cards.get('card-1001').innerHTML,/>Reported fault — stale GPS \+1<\/span>/);
    assert.match(f.cards.get('card-1001').innerHTML,/stale GPS; low battery/);
    assert.match(f.cards.get('card-1001').innerHTML,/Reset diagnostic 0x02/);
    Object.assign(dev.data,{flags:0x88,txReasonCode:7,resetReasonPresent:false});
    f.context.renderDeviceCard(dev);
    assert.match(f.cards.get('card-1001').innerHTML,/>Reported fault — cause unspecified<\/span>/);
    assert.doesNotMatch(f.cards.get('card-1001').innerHTML,/Reset diagnostic/);
    Object.assign(dev.data,{flags:0,resetReason:3,rssi:-140,snr:-25});
    f.context.renderDeviceCard(dev);
    assert.doesNotMatch(f.cards.get('card-1001').innerHTML,/error-badge/);
});

test('real firmware JSON serializers preserve diagnostics for live, reconnect and API paths',()=>{
    const root=fileURLToPath(new URL('../',import.meta.url));
    const firmware=readFileSync(resolve(root,'hub/src/main.cpp'),'utf8').replaceAll('\r\n','\n');
    const live=firmware.match(/static void buildDeviceJson\(const uint8_t \*buf[^;{]*\) \{[\s\S]*?\n}/)[0];
    const state=firmware.match(/struct device_state_t \{[\s\S]*?\n};/)[0];
    const snapshots=['handleEvents','handleApiDevices'].map(name=>{
        const body=firmware.match(new RegExp('static void '+name+'\\(\\) \\{[\\s\\S]*?\\n}'))[0];
        return body.match(/snprintf\((?:json|buf),[\s\S]*?\n            \);/)[0].replaceAll('sizeof(json)','outLen').replaceAll('sizeof(buf)','outLen').replace(/^snprintf\((json|buf),/,'snprintf(out,');
    });
    const code=`
#include <initializer_list>
#include "bp_protocol.h"
${state}
const char* deviceDisplayName(uint16_t) { return "Fixture"; }
const char* deviceEmoji(uint16_t) { return "cat"; }
const char* deviceColour(uint16_t) { return "#0099ff"; }
const char* journalSyncName(uint8_t) { return "pending"; }
unsigned long deviceRxWindowMs(const device_state_t&) { return 0; }
unsigned long deviceAgeSeconds(const device_state_t&) { return 0; }
${live}
void reconnect(device_state_t* d,char* out,size_t outLen) { ${snapshots[0]} }
void api(device_state_t* d,char* out,size_t outLen) { ${snapshots[1]} }
int main() {
  for(uint8_t flags : {0,128,192,196,133}) for(uint8_t reason : {0,4,7}) {
    uint8_t packet[BP_MAX_PACKET_SIZE]; char out[768];
    pkt_init(packet,1001,16,1,1787920000,STATUS_HOME,PROFILE_ACTIVE,flags,reason);
    if(reason==4) pkt_add_tlv_u8(packet,TLV_RESET_REASON,2);
    pkt_finalize(packet);
    buildDeviceJson(packet,-140,-25,1,1787920000,0,out,sizeof(out)); puts(out);
    device_state_t d{}; d.device_id=1001; d.flags=flags; d.tx_reason=reason;
    d.error_present=flags & FLAG_ERROR_PRESENT; d.reset_reason=reason==4?2:0;
    d.reset_reason_present=reason==4;
    reconnect(&d,out,sizeof(out)); puts(out); api(&d,out,sizeof(out)); puts(out);
  }
}`;
    const dir=resolve(root,'.pio/fault-tests'); mkdirSync(dir,{recursive:true});
    const cpp=resolve(dir,'serialization.cpp'), exe=resolve(dir,process.platform==='win32'?'serialization.exe':'serialization');
    writeFileSync(cpp,code);
    const compiler=process.env.CXX || (process.platform==='win32'?'C:/ProgramData/mingw64/mingw64/bin/g++.exe':'g++');
    const compiled=spawnSync(compiler,['-std=c++17','-I'+resolve(root,'shared/lib/BluepawsProtocol'),cpp,'-o',exe],{encoding:'utf8'});
    assert.equal(compiled.status,0,compiled.stderr);
    const result=spawnSync(exe,[],{encoding:'utf8'}); assert.equal(result.status,0,result.stderr);
    const rows=result.stdout.trim().split(/\r?\n/).map(JSON.parse);
    assert.equal(rows.length,45);
    const f=fixture(false), dev=f.device(1001);
    for(let index=0;index<rows.length;index+=3) {
        const expected=rows[index];
        assert.equal(typeof expected.txReason,'string','retain live SSE display-string compatibility');
        for(const data of rows.slice(index,index+3)) {
            for(const key of ['flags','txReasonCode','errorPresent','resetReason','resetReasonPresent']) assert.equal(data[key],expected[key],key);
            dev.data=data; f.context.renderDeviceCard(dev);
            const label=collarFault({flags:data.flags,txReason:data.txReasonCode})?.label;
            const html=f.cards.get('card-1001').innerHTML;
            if(label) assert(html.includes('>'+label+'</span>'));
            else assert.doesNotMatch(html,/error-badge/);
        }
    }
});
