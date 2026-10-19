// Real React card components with synthetic data. Never connects to Supabase.
import {createRequire} from 'node:module';
import {readFileSync, existsSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {posix} from 'node:path';
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
  const context = vm.createContext({exports:{},require(child) {
    return load(child.startsWith('.') ? posix.join(posix.dirname(name),child).replace(/\.tsx?$/,'') : child);
  }});
  vm.runInContext(output,context); modules.set(name,context.exports);
  return context.exports;
}
export { load as fixtureModule };
export const hub = {gateway_guid16:16, household_id:'synthetic-family', mode:'home',
  display_name:'Bluepaws Test Hub',home_emoji:'🏡',portable_emoji:'📱',marker_colour:'#38bdf8',
  received_at:'2026-08-27T12:00:00Z',latitude:51.907,longitude:-2.24,fix_at:'2026-08-27T11:59:57Z',
  wifi_rssi_dbm:-40,ble_enabled:true,ble_advertising:true,desired_ble_enabled:true,settings_revision:1,applied_revision:1};
const noop = () => {};
export const cardProps = {expanded:true, dragging:false,dragOver:false,followed:false,trailVisible:false,
  portableMode:false,distance:'122 m',ageSeconds:3,onExpand:noop,onAction:noop,onAvatarEdit:noop,
  onDragStart:noop,onDragOver:noop,onDrop:noop,onDragEnd:noop,onPinToggle:noop,onReportLog:noop,onReportExport:noop};
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
export function renderReport(report, entityLabel='hub', props={}) {
  // The real modal, with only the portal target substituted for static fixture QA.
  const source=readFileSync(new URL('../web/src/components/DeviceReportModal.tsx',import.meta.url),'utf8');
  const context=vm.createContext({exports:{},document:{body:{}},require(name){
    return name==='react-dom' ? {createPortal:node=>node} : load(name);
  }});
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,jsx:ts.JsxEmit.ReactJSX}}).outputText,context);
  return renderToStaticMarkup(React.createElement(context.exports.DeviceReportModal,{deviceName:'Synthetic report',reports:[report],entityLabel,loading:false,error:null,onClose:noop,onDownload:noop,...props}));
}
if (process.argv.includes('--serve')) {
  createServer((req,res) => {
    if (['/parity.css','/web.css','/hub-presence.css'].includes(req.url)) {
      res.setHeader('Content-Type','text/css');
      return res.end(readFileSync(new URL('../web/src/app'+req.url,import.meta.url)));
    }
    res.setHeader('Content-Type','text/html; charset=utf-8');
    if(req.url === '/metric-colours') {
      const now=Date.now();
      const cards=[['Recent GPS',300,30,350],['Amber metrics',720,1861,700],['Red metrics',10800,7201,1200]].map(([name,gpsAge,ageSeconds,metres])=>{
        const at=new Date(now-gpsAge*1000).toISOString();
        const device={id:3001,name,seq:1,time:now/1000,status:'Out',profile:'PowerSave',error:'None',hasGps:true,lat:51+metres/111195,lon:-2,batt:3900,rssi:-90,snr:8,ingestPath:'lora_hub',bleHome:false,lastUpdate:now-ageSeconds*1000,positionRecordedAt:at,gnss:{accuracyM:6,satellites:8,fixAgeS:0,recordedAt:at},homeHub:{lat:51,lon:-2,fixAt:new Date(now).toISOString()}};
        return '<section style="width:560px;max-width:100%"><h3>'+name+'</h3>'+renderCollar({device,now,ageSeconds,distance:load('@/lib/mapLocation').formatHomeDistance(load('@/lib/mapLocation').homeDistanceMetres(device))})+'</section>';
      });
      return res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Metric colour fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css"><main id="deviceCards" style="padding:12px;display:flex;gap:12px;flex-wrap:wrap">'+cards.join('')+'</main>');
    }
    if(req.url === '/packet-log') {
      const report=load('@/lib/deviceReports').buildDeviceReport({id:1,device_guid16:3001,msg_seq_id:12,time_unix:1770000000,recorded_at:new Date().toISOString(),received_at:new Date().toISOString(),status:0,power_profile:0,flags:200,tx_reason:4,gnss_valid:false,latitude:null,longitude:null,batt_mv:0,acc_m:0,fix_age_s:65535,sat_count:255,tlv_data:{fw_ver:258,reset_reason:4,uptime_s:0}},null);
      return res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Packet diagnostics fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css">'+renderReport(report,'collar'));
    }
    if(req.url === '/report-log') {
      const report=load('@/lib/hubReports').buildHubReport({...hub,received_at:new Date().toISOString(),wifi_rssi_dbm:-69,uptime_s:90});
      return res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Contextual report fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css">'+renderReport(report));
    }
    if (req.url === '/gps-freshness') {
      const now=Date.now();
      const cases=[['Fresh GPS',15],['Ageing GPS',2400],['Old GPS, new contact',8049],['Home, GPS last known',8049,true],['Unknown fix time',null]];
      res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">' +
        '<title>GPS age — synthetic fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css">' +
        '<main id="deviceCards" style="width:100%;min-width:0;height:100vh;overflow:auto;padding:12px;display:flex;flex-direction:row;flex-wrap:wrap;gap:16px;align-items:flex-start">' +
        cases.map(([name,age,home])=>{
          const at=new Date(now-(age??0)*1000).toISOString();
          const device={id:3001,name,profile:'PowerSave',status:home?'Home':'Out',bleHome:!!home,homeBeaconSeenAt:home?now:null,lastReportReason:home?7:0,error:'None',lat:51,lon:-2,hasGps:true,
            batt:3900,rssi:-90,snr:8,ingestPath:'lora_hub',lastUpdate:now,positionRecordedAt:at,
            gnss:age===null?null:{accuracyM:6,satellites:8,fixAgeS:0,recordedAt:at}};
          return '<section style="width:490px;max-width:100%;min-width:0"><h3>'+name+'</h3>'+[false,true].map(expanded=>renderCollar({device,now,expanded,ageSeconds:0})).join('')+'</section>';
        }).join('')+'</main>');
      return;
    }
    if (req.url === '/faults') {
      const cases=[['Stale GPS',0xc1,0],['GPS unavailable',0x80,0],['Low battery',0x85,0],
        ['Multiple indicators',0xc4,4],['Wake check-in',0x88,7],['Healthy / Lost Alert',0x09,7]];
      res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">' +
        '<title>Collar fault reasons — synthetic fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css">' +
        '<main style="padding:12px;display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,360px));gap:16px">' +
        cases.map(([name,flags,txReason])=>'<section><h3>'+name+'</h3>'+[false,true].map(expanded=>renderCollar({expanded,
          reportedFlags:flags,reportedFaultReport:{flags,txReason,resetReason:txReason===4?2:null}})).join('')+'</section>').join('')+'</main>');
      return;
    }
    res.end('<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">' +
      '<title>Shared cloud cards — synthetic fixture</title><link rel="stylesheet" href="/parity.css"><link rel="stylesheet" href="/web.css"><link rel="stylesheet" href="/hub-presence.css">' +
      '<main style="width:460px;max-width:100%;padding:12px"><h3>Shared cloud cards · UI fixture</h3>' +
      renderHub() + renderCollar() + renderCollar({ageSeconds:4*60*60}) + renderHub({latitude:null,longitude:null,fix_at:null,mode:'portable'}) + '</main>');
  }).listen(8793,'127.0.0.1',()=>console.log('Synthetic cloud cards: http://127.0.0.1:8793/'));
}
