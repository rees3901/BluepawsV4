// Real React card components with synthetic data. Never connects to Supabase.
import {createRequire} from 'node:module';
import {readFileSync, existsSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {createServer} from 'node:http';
import vm from 'node:vm';
const require = createRequire(new URL('../web/package.json', import.meta.url));
const ts = require('typescript'), React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const modules = new Map();
function load(name) {
  if (name === '@/lib/supabase/client') return {createClient() {throw Error('Fixture must not contact cloud');}};
  if (!name.startsWith('@/')) return require(name);
  if (modules.has(name)) return modules.get(name);
  const base = new URL('../web/src/' + name.slice(2), import.meta.url);
  const path = ['.tsx','.ts'].map(ext => fileURLToPath(base) + ext).find(existsSync);
  const source = readFileSync(path,'utf8');
  const output = ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,jsx:ts.JsxEmit.ReactJSX}}).outputText;
  const context = vm.createContext({exports:{},require:load});
  vm.runInContext(output,context); modules.set(name,context.exports);
  return context.exports;
}
export const hub = {gateway_guid16:16, household_id:'synthetic-family', mode:'home',
  display_name:'Bluepaws Test Hub',home_emoji:'🏡',portable_emoji:'📱',marker_colour:'#38bdf8',
  received_at:'2026-08-27T12:00:00Z',latitude:51.907,longitude:-2.24,fix_at:'2026-08-27T11:59:57Z',
  wifi_rssi_dbm:-40,ble_advertising:true,desired_ble_enabled:true,settings_revision:1,applied_revision:1};
const noop = () => {};
export const cardProps = {expanded:true, dragging:false,dragOver:false,followed:false,trailVisible:false,
  portableMode:false,distance:'122 m',ageSeconds:3,onExpand:noop,onAction:noop,onAvatarEdit:noop,
  onDragStart:noop,onDragOver:noop,onDrop:noop,onDragEnd:noop,onPinTop:noop,onReportLog:noop,onReportExport:noop};
export function renderHub(overrides={}, props={}) {
  const h={...hub,...overrides};
  const device=load('@/lib/hubPresence').hubMapDevice(h);
  return renderToStaticMarkup(React.createElement(load('@/components/HubCard').HubCard,
    {hub:h,onSaved:noop,cardProps:{...cardProps,device,avatar:load('@/lib/hubPresence').hubAvatar(h),...props}}));
}
export function renderCollar(props={}) {
  const device={id:1001,name:'Podge',profile:'Active',status:'Home',error:'None',lat:51.906,lon:-2.239,
    hasGps:true,batt:3900,rssi:-94,snr:8,ingestPath:'lora_hub'};
  return renderToStaticMarkup(React.createElement(load('@/components/DeviceCard').DeviceCard,
    {...cardProps,device,avatar:{kind:'emoji',emoji:'🐱',color:'#0099ff'},...props}));
}
if (process.argv.includes('--serve')) {
  createServer((req,res) => {
    if (['/parity.css','/web.css','/hub-presence.css'].includes(req.url)) {
      res.setHeader('Content-Type','text/css');
      return res.end(readFileSync(new URL('../web/src/app'+req.url,import.meta.url)));
    }
    res.setHeader('Content-Type','text/html; charset=utf-8');
    res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">' +
      '<title>Shared cloud cards — synthetic fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css"><link rel="stylesheet" href="/hub-presence.css">' +
      '<main style="width:460px;max-width:100%;padding:12px"><h3>Shared cloud cards · UI fixture</h3>' +
      renderHub() + renderCollar() + renderHub({latitude:null,longitude:null,fix_at:null,mode:'portable'}) + '</main>');
  }).listen(8793,'127.0.0.1',()=>console.log('Synthetic cloud cards: http://127.0.0.1:8793/'));
}
