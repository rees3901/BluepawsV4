// Run with node --test tools/test_hub_device_cards.mjs (no dependencies).
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
import assert from 'node:assert/strict';

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
    assert.match(f.cards.get('card-1001').innerHTML,/Collar reported a fault/);
});
