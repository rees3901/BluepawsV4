// Render the real TSX card; only unrelated icon/URL helpers are stubbed.
import {createRequire} from 'node:module';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
import * as hubControlFeedback from '../web/src/lib/hubControlFeedback.ts';
import * as hubReporting from '../web/src/lib/hubReporting.ts';
import * as collarFault from '../web/src/lib/collarFault.ts';
import * as collarFeedback from '../web/src/lib/collarFeedback.ts';
import {buildDiagnosticPacket,defaultDeviceSettings,generateDeviceCredential} from './tlv-web-console/lib/tlv-core.mjs';
import {parseTlvPacket} from '../supabase/functions/ingest-position/tlv.ts';
const require = createRequire(new URL('../web/package.json',import.meta.url));
const ts = require('typescript');
const React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const source=readFileSync(new URL('../web/src/components/DeviceCard.tsx',import.meta.url),'utf8');
const output=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,jsx:ts.JsxEmit.ReactJSX}}).outputText;
const icons=Object.fromEntries(['BatteryIndicator','BleProximity','HomeDistance','LastSeen','SignalIndicator'].map(name=>[name,({children})=>React.createElement('span',null,children)]));
const context=vm.createContext({exports:{},require(name){
  if(name==='@/components/Indicators')return icons;
  if(name==='@/lib/hubControlFeedback')return hubControlFeedback;
  if(name==='@/lib/hubReporting')return hubReporting;
  if(name==='@/lib/collarFault')return collarFault;
  if(name==='@/lib/emoji')return {emojiImageUrl:()=>'/favicon.svg'};
  if(name==='@/lib/mapLocation')return {googleMapsUrl:()=> '#',formatMapCoordinates:()=> '51.9, -2.2'};
  return require(name);
}});
vm.runInContext(output,context);
const device={id:1001,name:'Podge',profile:'Emergency Lost',status:'Lost',error:'None',lat:51.9,lon:-2.2,batt:3900};
const props={device,avatar:{kind:'emoji',emoji:'🐾',color:'#0099ff'},expanded:true,ageSeconds:3,distance:'1 km'};
const render=extra=>renderToStaticMarkup(React.createElement(context.exports.DeviceCard,{...props,...extra}));
test('Lost Alert is not a fault in the cloud card',()=>{
  const html=render({reportedFlags:0,awakeSeconds:7,commandFeedback:{text:'Command pending: profile → Active',pending:true,status:'pending'}});
  assert.match(html,/status-lost/); assert.match(html,/Emergency Lost/);
  assert.doesNotMatch(html,/class="error-badge"/);
  assert.match(html,/💡/); assert.match(html,/command-feedback pending/);
});
test('fresh presence flags clear old position faults; real faults remain visible',()=>{
  assert.doesNotMatch(render({device:{...device,error:'Module'},reportedFlags:0}),/class="error-badge"/);
  assert.match(render({reportedFlags:128}),/Reported fault/);
  assert.doesNotMatch(render({awakeSeconds:0}),/💡/);
  assert.match(render({awakeSeconds:0}),/💤/);
  assert.doesNotMatch(render({commandFeedback:null}),/role="status"/);
});

test('cloud fault badge stays compact and uses only diagnostics belonging to the current report',()=>{
  for(const expanded of [false,true]) {
    const html=render({expanded,reportedFlags:0xc4,reportedFaultReport:{flags:0xc4,txReason:4,resetReason:2}});
    assert.match(html,/class="card-fault-row"/);
    assert.match(html,/>Reported fault — stale GPS \+1<\/span>/);
    assert.match(html,/title="Reported fault — stale GPS; low battery/);
    assert.match(html,/aria-label="Reported fault/);
  }
  const oldPosition={...device,error:'Module',faultReport:{flags:0xc0,txReason:0}};
  assert.match(render({device:oldPosition,reportedFlags:0x88}),/>Reported fault — cause unspecified<\/span>/);
  assert.doesNotMatch(render({device:oldPosition,reportedFlags:0}),/error-badge/);
  assert.match(render({device:oldPosition}),/>Reported fault — stale GPS<\/span>/);
  assert.match(render({device:oldPosition,reportedFlags:0x80,reportedFaultReport:{flags:0xc0,txReason:0}}),/>Reported fault — cause unspecified<\/span>/);
  assert.doesNotMatch(render({device:{...device,rssi:-140,snr:-25}}),/error-badge/);
});

test('simulator binary TLV decodes through the ingestion parser into the rendered fault summary',async()=>{
  const credential=generateDeviceCredential(1001);
  for(const [flags,txReason,label] of [[0xc4,4,'stale GPS +1'],[0x80,0,'GPS fix unavailable'],[0x88,7,'cause unspecified'],[0x85,0,'low battery']]) {
    const built=buildDiagnosticPacket({...defaultDeviceSettings(1001),flags,txReason,includeTlvs:true},credential);
    const packet=parseTlvPacket(built.packet);
    const reports=await collarFault.loadFaultReports([{device_id:1001,observation_id:42,flags:packet.flags}],async()=>({error:null,
      data:[{id:42,device_guid16:packet.deviceGuid16,flags:packet.flags,tx_reason:packet.txReason,reset_reason:packet.tlvs.reset_reason}]}));
    assert(render({reportedFlags:packet.flags,reportedFaultReport:reports[1001]}).includes('>Reported fault — '+label+'</span>'));
  }
});

test('real feedback hook publishes flags before optional Family-scoped diagnostics and ignores late unmounted reads',async()=>{
  for(const unmount of [false,true]) {
    let state, resolveDetails, now=100000;
    const effects=[], calls=[];
    const details=new Promise(resolve=>{resolveDetails=resolve;});
    const client={rpc:async()=>({error:null,data:[{device_id:1001,observation_id:42,flags:128,rx_window_remaining_ms:8000,command:null}]}),
      from(table){calls.push(['from',table]);return query;}};
    const query={select(columns){calls.push(['select',columns]);return query;},
      eq(key,value){calls.push(['eq',key,value]);return query;},in(key,ids){calls.push(['in',key,ids]);return query;},
      abortSignal(signal){assert(signal instanceof AbortSignal);return details;}};
    const hooks={useState(initial){state=initial;return [state,next=>{state=next(state);}];},
      useRef:value=>({current:value}),useCallback:fn=>fn,useEffect:fn=>effects.push(fn)};
    const hooksSource=readFileSync(new URL('../web/src/lib/useCollarFeedback.ts',import.meta.url),'utf8');
    const compiled=ts.transpileModule(hooksSource,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText;
    const noop=()=>{};
    const ctx=vm.createContext({exports:{},Date:{now:()=>now},AbortSignal,setTimeout,clearTimeout,
      document:{hidden:false,addEventListener:noop,removeEventListener:noop},window:{addEventListener:noop,removeEventListener:noop},
      require(name){return {'react':hooks,'@/lib/supabase/client':{createClient:()=>client},'@/lib/collarFeedback':collarFeedback,'@/lib/collarFault':collarFault}[name];}});
    vm.runInContext(compiled,ctx);ctx.exports.useCollarFeedback('family-a','v1');
    const cleanup=effects[0]();
    await new Promise(setImmediate);
    assert.equal(state.rows[1001].flags,128,'fault flag appears before diagnostics finish');
    assert.equal(state.rows[1001].rxWindowUntil,108000);
    assert.equal(state.rows[1001].faultReport,null);
    assert.deepEqual(calls.slice(0,3),[['from','observations'],['select','id,device_guid16,flags,tx_reason,reset_reason:tlv_data->reset_reason'],['eq','household_id','family-a']]);
    assert.equal(JSON.stringify(calls[3]),JSON.stringify(['in','id',[42]]));
    if(unmount)cleanup();
    now+=2000;
    resolveDetails({error:null,data:[{id:42,device_guid16:1001,flags:128,tx_reason:7,reset_reason:null}]});
    await new Promise(setImmediate);
    assert.equal(state.rows[1001].faultReport?.txReason,unmount?undefined:7);
    assert.equal(state.rows[1001].rxWindowUntil,108000,'enrichment never restarts the receive timer');
    if(!unmount)cleanup();
  }
});
