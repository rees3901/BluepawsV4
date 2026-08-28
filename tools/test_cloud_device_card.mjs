// Render the real TSX card; only unrelated icon/URL helpers are stubbed.
import {createRequire} from 'node:module';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
import * as hubControlFeedback from '../web/src/lib/hubControlFeedback.ts';
import * as hubReporting from '../web/src/lib/hubReporting.ts';
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
