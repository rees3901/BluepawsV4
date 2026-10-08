import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';
import * as powerProfiles from '../web/src/lib/powerProfiles.ts';
const require = createRequire(new URL('../web/package.json', import.meta.url));
const ts = require('typescript'), React = require('react');
const code = ts.transpileModule(readFileSync(new URL('../web/src/components/DeviceCard.tsx', import.meta.url),'utf8'), {compilerOptions:{module:ts.ModuleKind.CommonJS,jsx:ts.JsxEmit.ReactJSX}}).outputText;
function harness(onSend) {
 const state=[], effects=[]; let cursor=0, effectCursor=0, now=0, nextTimer=0; const timers=new Map();
 const context=vm.createContext({exports:{}, Error, setTimeout(fn,ms){const id=++nextTimer;timers.set(id,{fn,at:now+ms});return id;}, clearTimeout(id){timers.delete(id);}, require(name){
  if(name==='react')return {...React,useState(initial){const i=cursor++;if(!(i in state))state[i]=initial;return [state[i],value=>{state[i]=value;}];},useEffect(fn,deps){const i=effectCursor++;const old=effects[i];if(!old||deps.some((v,j)=>!Object.is(v,old.deps[j]))){old?.cleanup?.();effects[i]={deps,cleanup:fn()};}}};
  if(name==='@/lib/powerProfiles')return powerProfiles;
  if(name.startsWith('@/'))return {};
  return require(name);
 }});vm.runInContext(code,context);
 let profile='Normal';
 const render=()=>{cursor=0;effectCursor=0;return context.exports.InlineProfileControl({device:{id:3001,name:'Podge',profile},onSend});};
 const flatten=node=>!node?[]:Array.isArray(node)?node.flatMap(flatten):typeof node==='object'?[node,...flatten(node.props?.children)]:[];
 const get=(type,text)=>flatten(render()).find(n=>n.type===type&&(!text||n.props.children===text));
 const change=value=>get('select').props.onChange({target:{value}});
 return {render,get,change,profile(value){profile=value;},advance(ms){render();now+=ms;for(const [id,timer] of timers){if(timer.at<=now){timers.delete(id);timer.fn();}}}};
}
test('selection is a draft; cancel sends nothing; confirmed queue preserves reported profile',async()=>{
 const calls=[];const h=harness(async profile=>calls.push(profile));
 assert.equal(h.get('select').props.value,'normal');assert.equal(h.get('button','Send command'),undefined);
 h.change('active');assert.equal(calls.length,0);assert.ok(h.get('button','Send command'));
 h.get('button','Cancel').props.onClick();assert.equal(h.get('select').props.value,'normal');assert.equal(calls.length,0);
 h.change('power_save');await h.get('button','Send command').props.onClick();assert.deepEqual(calls,['power_save']);
 assert.equal(h.get('select').props.value,'normal');assert.equal(h.get('button','Send command'),undefined);
 h.profile('PowerSave');assert.equal(h.get('select').props.value,'power_save');
});
test('queue errors preserve draft and allow retry; duplicate sends disabled while pending',async()=>{
 let reject, calls=0;const h=harness(()=>{calls++;return new Promise((_,r)=>{reject=r;});});
 h.change('active');const pending=h.get('button','Send command').props.onClick();
 assert.equal(h.get('select').props.disabled,true);assert.equal(h.get('button','Cancel').props.disabled,true);
 await h.get('button','Queueing…').props.onClick();assert.equal(calls,1);
 reject(new Error('Queue unavailable'));await pending;assert.equal(h.get('select').props.value,'active');
 assert.equal(h.get('p','Queue unavailable').props.role,'alert');assert.equal(h.get('select').props.disabled,false);
});
test('new reported profile invalidates an unsent draft; unknown Debug is preserved until selection',()=>{
 const h=harness(async()=>{});h.change('active');h.profile('PowerSave');assert.equal(h.get('select').props.value,'power_save');
 assert.equal(h.get('button','Send command'),undefined);
 h.profile('Normal');assert.equal(h.get('select').props.value,'normal');assert.equal(h.get('button','Send command'),undefined);
 h.profile('Debug');assert.equal(h.get('select').props.value,'');assert.equal(h.get('button','Send command'),undefined);
 h.change('normal');assert.ok(h.get('button','Send command'));
});

 test('unsent draft cancels at ten seconds; changing selection restarts timeout',()=>{
 const calls=[];const h=harness(async p=>calls.push(p));
 h.change('active');h.advance(9999);assert.equal(h.get('select').props.value,'active');
 h.advance(1);assert.equal(h.get('select').props.value,'normal');assert.equal(h.get('button','Send command'),undefined);assert.deepEqual(calls,[]);
 h.change('active');h.advance(9000);h.change('power_save');h.advance(1000);assert.equal(h.get('select').props.value,'power_save');
 h.advance(9000);assert.equal(h.get('select').props.value,'normal');
 });
 test('timeout never cancels an in-flight send and clears queue errors after inactivity',async()=>{
 let reject;const h=harness(()=>new Promise((_,r)=>{reject=r;}));
 h.change('active');const pending=h.get('button','Send command').props.onClick();h.advance(20000);
 assert.equal(h.get('select').props.value,'active');assert.equal(h.get('select').props.disabled,true);
 reject(new Error('Queue unavailable'));await pending;h.advance(10000);
 assert.equal(h.get('select').props.value,'normal');assert.equal(h.get('p','Queue unavailable'),undefined);
 });
