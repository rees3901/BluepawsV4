import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {renderHub,renderCollar} from './shared_device_card_fixture.mjs';
import {orderDeviceIds,pinDeviceFirst,moveDeviceBefore} from '../web/src/lib/deviceCardOrder.ts';
import {nextExpandedDeviceCards} from '../web/src/lib/expandedCards.ts';
import {buildHubReport,hubReportCsv} from '../web/src/lib/hubReports.ts';
import {hub} from './shared_device_card_fixture.mjs';
import {createRequire} from 'node:module';
import {normalizeDeviceName} from '../web/src/lib/deviceName.ts';
const require = createRequire(new URL('../web/package.json',import.meta.url));

function appearanceService({uploadError=null,saveError=null,affected=true}={}) {
  const calls=[];
  const chain={eq(field,value){calls.push(['eq',field,value]);return chain;},
    async select(){return {data:affected?[{gateway_guid16:16}]:[],error:saveError};}};
  const db={storage:{from(bucket){assert.equal(bucket,'hub-avatars');return {
    async upload(path,blob,options){calls.push(['upload',path,options]);return {error:uploadError};},
    async remove(paths){calls.push(['remove',...paths]);return {error:null};}
  };}},from(table){assert.equal(table,'hub_presence');return {update(values){calls.push(['update',values]);return chain;}};}};
  const source=readFileSync(new URL('../web/src/lib/hubAppearances.ts',import.meta.url),'utf8');
  const ts=require('typescript');
  const context=vm.createContext({exports:{},crypto:{randomUUID:()=> '00000000-0000-0000-0000-000000000099'},
    require(name){if(name==='./deviceName')return {normalizeDeviceName};assert.equal(name,'@/lib/supabase/client');return {createClient:()=>db};}});
  vm.runInContext(ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS}}).outputText,context);
  return {save:(input,mode)=>context.exports.saveHubAppearance({name:'Test Hub',...input},mode),calls};
}

test('hub photo upload saves only its Family-scoped preferences then removes the old photo',async()=>{
  const {save,calls}=appearanceService();
  await save({deviceId:-16,householdId:'family',kind:'photo',emoji:'🏡',color:'#38bdf8',preparedPhoto:{},previousStoragePath:'family/16/old.webp'},'home');
  const uploaded=calls.find(c=>c[0]==='upload');
  assert.match(uploaded[1],/^family\/16\/.+\.webp$/);
  assert.equal(uploaded[2].upsert,false);
  assert.equal(uploaded[2].contentType,'image/webp');
  const values=calls.find(c=>c[0]==='update')[1];
  assert.equal(values.display_name,'Test Hub');
  assert.equal(values.avatar_storage_path,uploaded[1]);
  assert.equal(values.home_emoji,'🏡');
  assert.ok(calls.some(c=>c[0]==='eq'&&c[1]==='gateway_guid16'&&c[2]===16));
  assert.ok(calls.some(c=>c[0]==='eq'&&c[1]==='household_id'&&c[2]==='family'));
  assert.equal(calls.at(-1)[1],'family/16/old.webp');
});

test('failed hub photo saves clean up only the new file and never report success',async()=>{
  for(const options of [{saveError:new Error('Denied')},{affected:false}]) {
    const {save,calls}=appearanceService(options);
    await assert.rejects(save({deviceId:-16,householdId:'family',kind:'photo',preparedPhoto:{},previousStoragePath:'old'},'home'));
    assert.equal(calls.at(-1)[0],'remove');
    assert.equal(calls.at(-1)[1],calls.find(c=>c[0]==='upload')[1]);
    assert.ok(!calls.some(c=>c[0]==='remove'&&c[1]==='old'));
  }
  const {save,calls}=appearanceService({uploadError:new Error('Upload failed')});
  await assert.rejects(save({deviceId:-16,householdId:'family',kind:'photo',preparedPhoto:{}},'home'));
  assert.equal(calls.length,1);
});

test('hub emoji changes remove the photo and invalid identities cannot write',async()=>{
  const {save,calls}=appearanceService();
  await save({deviceId:-16,householdId:'family',kind:'emoji',emoji:'📱',previousStoragePath:'old'},'portable');
  const values=calls.find(c=>c[0]==='update')[1];
  assert.equal(values.avatar_storage_path,null);
  assert.equal(values.portable_emoji,'📱');
  assert.equal(calls.at(-1)[1],'old');
  const before=calls.length;
  for(const deviceId of [16,-17,-65536]) await assert.rejects(save({deviceId},'home'),/Invalid hub identity/);
  assert.equal(calls.length,before);
});

test('hub and collar share card shell, avatar, expansion and navigation',()=>{
  for (const html of [renderHub(),renderCollar()]) {
    for (const cls of ['device-card expanded','card-summary','card-avatar-wrap','card-reorder-handle',
      'card-pin-button','card-avatar-edit','card-detail-reveal','card-grid','btn-jump','btn-follow','btn-trail'])
      assert.ok(html.includes(cls),cls);
  }
  const html=renderHub();
  assert.match(html,/Wi-Fi -40 dBm/);
  assert.match(html,/Bluetooth On/);
  for (const part of ['battery-indicator','No data','sig-bar filled','>Wi-Fi</span>','icon-stopwatch','Message Log','btn-log-export']) assert.ok(html.includes(part),part);
  assert.doesNotMatch(html,/Power Profile|Dist From Hub|btn-find|Reported fault|hub-summary|Uptime|Free memory/);
  assert.match(html,/btn-cmd/); assert.match(html,/Reporting profile/);
  assert.match(renderCollar(),/Power Profile/);
  assert.match(renderCollar(),/btn-cmd/);
});

test('hub profile badge uses confirmed state and collar styling in both card sizes',()=>{
  for(const [profile,css,label] of [['power_save','power','Power Save'],['normal','normal','Normal'],['active','active','Active']]) {
    for(const expanded of [false,true]) {
      const html=renderHub({reporting_profile:profile,desired_reporting_profile:profile==='active'?'power_save':'active'},{expanded});
      assert.ok(html.includes(`class="card-profile profile-${css}">${label}</span>`));
    }
  }
  assert.match(renderHub(),/card-profile profile-normal">Normal<\/span>/);
  assert.match(renderCollar(),/card-profile profile-active">Active<\/span>/);
});

test('collar stopwatch advances through the ten-second bulb/sleep transition; hubs never sleep',()=>{
  for (const age of [0,1,9,10,11,59,60]) {
    const html=renderCollar({ageSeconds:age,awakeSeconds:Math.max(0,10-age)});
    assert.ok(html.includes(age<10 ? '💡' : '💤'));
    assert.ok(html.includes('class="card-lastseen-value">'+(age<60 ? age+'s' : '1m 0s')+'</span>'));
  }
  assert.doesNotMatch(renderHub(),/💡|💤/);
});

test('hub report and CSV use hub data, not invented collar fields',()=>{
  const report=buildHubReport({...hub,uptime_s:90});
  assert.match(report.summary,/Bluepaws Test Hub checked in/);
  assert.equal(report.rows.find(r=>r.field==='Battery').data,'No data');
  assert.equal(report.rows.find(r=>r.field==='Wi-Fi signal').data,'-40 dBm');
  assert.ok(!report.rows.some(r=>/Power Profile|Sequence|TLV/.test(r.field)));
  assert.match(hubReportCsv({...hub,display_name:'=formula',uptime_s:90}),/"'=formula/);
  assert.match(buildHubReport({...hub,latitude:null,longitude:null,fix_at:null}).rows.find(r=>r.field==='Coordinates').data,/Waiting/);
});

test('hub photo uses the same avatar image as collars and retains edit control',()=>{
  const html=renderHub({avatar_kind:'photo'}, {avatar:{kind:'photo',emoji:'🏡',color:'#38bdf8',photoUrl:'blob:synthetic-photo'}});
  assert.match(html,/card-avatar has-photo/);
  assert.match(html,/background-image:url\(&quot;blob:synthetic-photo/);
  assert.match(html,/card-avatar-edit/);
});
test('hub no-fix, collapsed, stale and mode states remain honest',()=>{
  const html=renderHub({latitude:null,longitude:null,fix_at:null});
  assert.match(html,/Waiting for GPS fix/);
  assert.doesNotMatch(html,/google.com\/maps/);
  assert.match(html,/btn-jump" disabled/);
  assert.match(renderHub({}, {expanded:false}),/aria-hidden="true"/);
  assert.match(renderHub({}, {ageSeconds:601}),/device-card stale expanded/);
  assert.match(renderHub({mode:'off_grid'}),/Off-Grid/);
  assert.match(renderHub({home_emoji:'🐈',display_name:'Custom Hub'}),/Custom Hub/);
});
test('hub uses same ordering, pin and four-card expansion rules without ID collisions',()=>{
  const ids=orderDeviceIds([1001,16,-16],[1001,-16]);
  assert.deepEqual(pinDeviceFirst(ids,-16),[-16,1001,16]);
  assert.deepEqual(moveDeviceBefore(ids,-16,1001),[-16,1001,16]);
  assert.deepEqual(nextExpandedDeviceCards([1001,1002,1003,1004],-16),[1002,1003,1004,-16]);
});
test('offline adapter shares renderer but never invents collar telemetry',()=>{
  const context=vm.createContext({});
  vm.runInContext(readFileSync(new URL('../hub/data/hub-presence.js',import.meta.url),'utf8'),context);
  const h=context.HubPresencePanel.view({gateway_guid16:'0010',mode:'portable',latitude:null,longitude:null});
  assert.equal(h.id,-16);assert.equal(h.emoji,'📱');assert.equal(h.hasGps,false);
  for (const key of ['profile','batt','rxWindowMs','errorPresent','verification']) assert.equal(h[key],undefined);
  const js=readFileSync(new URL('../hub/data/app.js',import.meta.url),'utf8');
  assert.match(js,/HubPresencePanel.start\(protectedFetch/);
  assert.match(js,/updateDevice\(data\); \/\/ Shared cards/);
  assert.doesNotMatch(js,/hub-map-icon|HubPresencePanel.point/);
});
