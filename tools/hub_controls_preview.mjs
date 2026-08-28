// Interactive, loopback-only real React components with a synthetic settings service.
// npm install --prefix .pio/ui-tests --no-package-lock --no-save --ignore-scripts esbuild
// node tools/hub_controls_preview.mjs
import {build} from '../.pio/ui-tests/node_modules/esbuild/lib/main.js';
import {createServer} from 'node:http';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
import {hub} from './shared_device_card_fixture.mjs';
const bundle=await build({
  stdin:{contents:`
    import React,{useState,useEffect} from 'react';
    import {createRoot} from 'react-dom/client';
    import {HubCard} from '@/components/HubCard';
    import {DeviceCard} from '@/components/DeviceCard';
    import {hubMapDevice,hubAvatar} from '@/lib/hubPresence';
    import {service} from '@/lib/supabase/client';
    const noop=()=>{};
    const base={expanded:true,dragging:false,dragOver:false,followed:false,trailVisible:false,
      portableMode:false,distance:'122 m',onExpand:noop,onAction:noop,onAvatarEdit:noop,onDragStart:noop,
      onDragOver:noop,onDrop:noop,onDragEnd:noop,onPinTop:noop,onReportLog:noop,onReportExport:noop};
    function App() {
      const [h,setHub]=useState(service.hub),[now,setNow]=useState(Date.now()),[receipt,setReceipt]=useState(Date.now());
      useEffect(()=>{const id=setInterval(()=>{setNow(Date.now());setHub({...service.hub});},1000);return()=>clearInterval(id)},[]);
      const age=Math.floor((now-receipt)/1000);
      return <><h3>Synthetic controls — no hardware/cloud</h3>
        <button onClick={()=>{setReceipt(Date.now());setNow(Date.now());}}>Simulate collar report</button>
        <HubCard hub={h} onSaved={()=>setHub({...service.hub})}
          cardProps={{...base,device:hubMapDevice(h),avatar:hubAvatar(h),ageSeconds:Math.floor((now-Date.parse(h.received_at))/1000)}}/>
        <DeviceCard {...base} device={{id:1001,name:'Podge · UI test',profile:'Active',status:'Home',error:'None',
          lat:51.9,lon:-2.2,hasGps:true,batt:3900,rssi:-94,snr:8,ingestPath:'lora_hub'}}
          avatar={{kind:'emoji',emoji:'🐱',color:'#0099ff'}} ageSeconds={age} awakeSeconds={Math.max(0,10-age)}/>
      </>;
    }
    createRoot(document.getElementById('root')).render(<App/>);
  `,loader:'tsx',resolveDir:resolve('web')},
  bundle:true,write:false,jsx:'automatic',platform:'browser',
  tsconfig:resolve('web/tsconfig.json'),nodePaths:[resolve('web/node_modules')],
  define:{'process.env.NODE_ENV':'"development"'},
  plugins:[{name:'synthetic-service',setup(b){
    b.onResolve({filter:/^@\/lib\/supabase\/client$/},()=>({path:'service',namespace:'test'}));
    b.onLoad({filter:/.*/,namespace:'test'},()=>({contents:`
      export const service={hub:{...${JSON.stringify(hub)},received_at:new Date().toISOString(),
        control_poll_s:5,reporting_profile:'power_save',desired_reporting_profile:'power_save'}};
      export function createClient(){return {from(table){
        if(table!=='hub_presence')throw Error('Unexpected table');
        return {update(values){
          Object.assign(service.hub,values,{settings_revision:service.hub.settings_revision+1});
          const revision=service.hub.settings_revision;
          const q={eq(){return q},select(){return q},async abortSignal(){
            setTimeout(()=>{Object.assign(service.hub,{ble_enabled:service.hub.desired_ble_enabled,
              reporting_profile:service.hub.desired_reporting_profile,applied_revision:revision,received_at:new Date().toISOString()})},2000);
            return {data:[{settings_revision:revision}],error:null};
          }};return q;
        }};
      }}}
    `}));
  }}],
});
createServer((req,res)=>{
  if(req.url==='/app.js'){res.setHeader('Content-Type','text/javascript');return res.end(bundle.outputFiles[0].text);}
  if(['/parity.css','/web.css','/hub-presence.css'].includes(req.url)){
    res.setHeader('Content-Type','text/css');return res.end(readFileSync(resolve('web/src/app'+req.url)));
  }
  res.setHeader('Content-Type','text/html;charset=utf-8');
  res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Synthetic hub controls</title>'+
    '<link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css"><link rel="stylesheet" href="/hub-presence.css">'+
    '<main id="root" style="width:460px;max-width:100%;padding:12px"></main><script src="/app.js"></script>');
}).listen(8794,'127.0.0.1',()=>console.log('Synthetic interactive cloud cards: http://127.0.0.1:8794/'));
