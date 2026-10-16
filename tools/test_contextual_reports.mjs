import test from 'node:test';
import assert from 'node:assert/strict';
import { fixtureModule, hub, renderReport } from './shared_device_card_fixture.mjs';
const {buildDeviceReport,deviceReportsToCsv}=fixtureModule('@/lib/deviceReports');
const {buildHubReport,hubReportCsv}=fixtureModule('@/lib/hubReports');
const insight=fixtureModule('@/lib/reportInsights');
const observation={id:1,device_guid16:3001,msg_seq_id:12,time_unix:1770000000,recorded_at:'2026-10-16T12:00:00Z',received_at:'2026-10-16T12:00:01Z',status:0,power_profile:0,flags:0,tx_reason:0,gnss_valid:true,latitude:51,longitude:-2,batt_mv:3900,acc_m:6,fix_age_s:0,sat_count:8};
const path={observation_id:1,ingest_path:'lora_hub',link_type:'lora',gateway_guid16:30,link_rssi_dbm:-100,link_snr_db:5,receipt_count:1};
const row=(r,f)=>r.rows.find(r=>r.field===f);

test('Wi-Fi boundary and missing-data scenarios give calibrated, not alarmist advice',()=>{
  for(const value of [-30,-50,-60]) assert.match(insight.wifiMeaning(value),/Strong/);
  for(const value of [-61,-69,-70]) assert.match(insight.wifiMeaning(value),/Moderate/);
  for(const value of [-71,-80]) assert.match(insight.wifiMeaning(value),/Weak/);
  for(const value of [-81,-90]) assert.match(insight.wifiMeaning(value),/Very weak/);
  for(const value of [null,undefined,NaN,Infinity,0,10,-32768]) assert.match(insight.wifiMeaning(value),/No usable/);
});

test('radio matrix separates transport, RSSI and SNR, including zero and negative SNR',()=>{
  for(const type of ['lora','lte',null]) for(const rssi of [null,NaN,-80,-100,-110,-120,-130]) for(const snr of [null,NaN,-10,0,4,5,10]) {
    for(const metric of ['rssi','snr']) {
      const meaning=insight.radioMeaning(rssi,snr,type,metric);
      assert.ok(meaning.length>40);
      assert.doesNotMatch(meaning,/guaranteed delivery|definitely disconnected/);
      if(type!=='lora') assert.doesNotMatch(meaning,/Strong power with low SNR|spreading factor/);
    }
  }
  assert.match(insight.radioMeaning(-80,0,'lora'),/noise\/interference may matter more than distance/);
  assert.match(insight.radioMeaning(-120,10,'lora'),/weaker signal is nevertheless clear of noise/);
  assert.match(insight.radioMeaning(-110,-10,'lora','snr'),/LoRa can still decode negative SNR/);
  assert.match(insight.radioMeaning(-110,0,'lora','snr'),/equal power/);
});

test('battery, GNSS sentinels and estimation limits are explicit',()=>{
  for(const n of [0,-1,NaN,Infinity,65535]) assert.match(insight.batteryMeaning(n),/No usable/);
  assert.match(insight.batteryMeaning(3300),/Charge soon/);
  assert.match(insight.batteryMeaning(3600),/Charge soon/);
  assert.match(insight.batteryMeaning(3700),/getting low/);
  assert.match(insight.batteryMeaning(3900),/normal working/);
  assert.match(insight.batteryMeaning(5000),/calibration/);
  for(const n of [null,0,-1,NaN,65535]) assert.match(insight.accuracyMeaning(n),/not supplied/);
  assert.match(insight.accuracyMeaning(6),/true error can exceed/);
  assert.match(insight.accuracyMeaning(20),/surrounding area/);
  assert.match(insight.accuracyMeaning(100),/coarse/);
  for(const n of [null,-1,255,NaN,2.5]) assert.match(insight.satellitesMeaning(n),/No usable/);
  assert.match(insight.satellitesMeaning(3),/Few/);
  assert.match(insight.satellitesMeaning(8),/geometry/);
  assert.match(insight.fixAgeMeaning(65535),/unknown/);
  assert.match(insight.fixAgeMeaning(0),/not its age now/);
  assert.match(insight.fixAgeMeaning(100),/already old/);
});

test('1,920 collar combinations retain meaningful rows without inventing fresh fixes',()=>{
  let cases=0;
  for(const power_profile of [0,1,2,3,4]) for(const status of [0,1,2,3]) for(const tx_reason of [0,1,2,3,4,5,6,7]) for(const gnss_valid of [false,true]) for(const fix_age_s of [0,30,60,61,3600,65535]) {
    const report=buildDeviceReport({...observation,power_profile,status,tx_reason,gnss_valid,fix_age_s},path);
    assert.ok(report.rows.every(r=>r.field&&r.data&&r.description&&r.meaning));
    assert.ok(report.rows.every(r=>r.description!==r.meaning),JSON.stringify(report.rows.filter(r=>r.description===r.meaning)));
    assert.match(row(report,'Position').meaning,gnss_valid?/may already have been old/:tx_reason===7?/no new GPS position is expected/:/cannot confirm where/);
    if(!gnss_valid) assert.doesNotMatch(row(report,'GPS accuracy').meaning,/tight accuracy/);
    if(gnss_valid && ![0,3,4].includes(tx_reason)) assert.match(row(report,'Fix age at report').meaning,/not a full GPS telemetry message/);
    cases++;
  }
  assert.equal(cases,1920);
  const unknown=buildDeviceReport({...observation,status:99,power_profile:99},null);
  assert.match(row(unknown,'Status').meaning,/Unrecognised/);
  assert.match(row(unknown,'Power profile').meaning,/Unrecognised/);
  assert.match(row(unknown,'Radio RSSI').meaning,/No usable/);
});

test('hub permutations distinguish simulated data, missing charge, mode and beacon evidence',()=>{
  for(const mode of ['home','portable','off_grid']) for(const ble_enabled of [false,true]) for(const ble_advertising of [false,true]) for(const position_simulated of [false,true]) for(const battery_percent of [null,0,10,15,16,70,101]) {
    const report=buildHubReport({...hub,mode,ble_enabled,ble_advertising,position_simulated,battery_percent,wifi_rssi_dbm:-69,uptime_s:90});
    assert.ok(report.rows.every(r=>r.meaning&&r.description));
    assert.match(row(report,'Wi-Fi signal').meaning,/Moderate/);
    assert.match(row(report,'Battery').meaning,battery_percent===null||battery_percent>100?/No usable/:battery_percent<=15?/Low reported/:/above the low/);
    if(position_simulated) assert.match(row(report,'Coordinates').meaning,/simulated/i);
    if(ble_advertising&&ble_enabled) assert.match(row(report,'Home beacon').meaning,/does not prove/);
    if(ble_advertising&&!ble_enabled) assert.match(row(report,'Home beacon').meaning,/Inconsistent/);
  }
  const simulated=buildHubReport({...hub,battery_simulated:true,battery_percent:0});
  assert.match(row(simulated,'Battery').meaning,/simulated/i);
  const unknown=buildHubReport({...hub,latitude:null,longitude:null,fix_at:null,uptime_s:NaN});
  assert.match(row(unknown,'GPS fix').meaning,/No GPS measurement time/);
  assert.match(row(unknown,'Uptime').meaning,/No usable uptime/);
});

test('exports preserve contextual advice and safely quote hub cells',()=>{
  const csv=hubReportCsv({...hub,display_name:'=formula',wifi_rssi_dbm:-69});
  assert.match(csv,/"Field","Data","Description","What this means"/);
  assert.match(csv,/"'=formula/);
  assert.match(csv,/Moderate Wi-Fi/);
  const collar=deviceReportsToCsv([buildDeviceReport(observation,path)]);
  assert.match(collar,/field_descriptions/); assert.match(collar,/reading_meanings/);
});

test('four-column modal renders descriptions and interpretations as separate cells',()=>{
  const report=buildHubReport({...hub,wifi_rssi_dbm:-69});
  const html=renderReport(report);
  assert.match(html,/<th scope="col">Field<\/th><th scope="col">Data<\/th><th scope="col">Description<\/th><th scope="col">What this means<\/th>/);
  assert.match(html,/<th scope="row">Wi-Fi signal<\/th><td>-69 dBm<\/td><td>Received Wi-Fi power.*?<\/td><td>Moderate Wi-Fi/s);
  for(const coordinates of [{latitude:null,longitude:null},{latitude:100,longitude:0},{latitude:NaN,longitude:0}]) {
    const report=buildDeviceReport({...observation,...coordinates},path);
    assert.match(row(report,'Position').meaning,/No usable GPS position/);
    assert.match(row(report,'GPS accuracy').meaning,/No usable position/);
  }
});
