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
 const state=[]; let cursor=0;
 const context=vm.createContext({exports:{}, Error, require(name){
  if(name==='react')return {...React,useState(initial){const i=cursor++;if(!(i in state))state[i]=initial;return [state[i],value=>{state[i]=value;}];}};
  if(name==='@/lib/powerProfiles')return powerProfiles;
  if(name.startsWith('@/'))return {};
  return require(name);
 }});vm.runInContext(code,context);
 let profile='Normal';
 const render=()=>{cursor=0;return context.exports.InlineProfileControl({device:{id:3001,name:'Podge',profile},onSend});};
 const flatten=node=>!node?[]:Array.isArray(node)?node.flatMap(flatten):typeof node==='object'?[node,...flatten(node.props?.children)]:[];
 const get=(type,text)=>flatten(render()).find(n=>n.type===type&&(!text||n.props.children===text));
 const change=value=>get('select').props.onChange({target:{value}});
 return {render,get,change,profile(value){profile=value;}};
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
 h.profile('Debug');assert.equal(h.get('select').props.value,'');assert.equal(h.get('button','Send command'),undefined);
 h.change('normal');assert.ok(h.get('button','Send command'));
});
