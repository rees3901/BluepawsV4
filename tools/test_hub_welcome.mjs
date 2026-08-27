import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import test from 'node:test';

const source = readFileSync(new URL('../hub/data/welcome.js', import.meta.url), 'utf8');
const fixture = {hub_id:'0010', recent_collars:2, known_collars:5, last_report_age_s:90, time_synced:true};
const flush = () => new Promise(resolve => setImmediate(resolve));
function boot({hidden=false, response=fixture, fail=false, pending=false}={}) {
    const elements = new Map();
    const events = {};
    const timers = new Map();
    const calls = [];
    let nextTimer = 0;
    const document = {hidden, getElementById(id) {
        if (!elements.has(id)) elements.set(id, {textContent:''});
        return elements.get(id);
    }, addEventListener(name, handler) { events[name] = handler; }};
    vm.runInNewContext(source, {document, window:{addEventListener(name, handler) {events[name]=handler;}},
        AbortController, setTimeout(fn, ms) {timers.set(++nextTimer, {fn, ms}); return nextTimer;},
        clearTimeout(id) {timers.delete(id);}, async fetch(url, options) {
            calls.push({url, options});
            if (pending) return new Promise((resolve, reject) => options.signal.addEventListener('abort', () => reject(new Error('aborted'))));
            return {ok:!fail, json:async () => response};
        }});
    return {elements,events,timers,calls,document};
}

test('welcome renders actual hub metadata and schedules one slow refresh', async () => {
    const app = boot(); await flush();
    assert.equal(app.elements.get('hub-id').textContent, '0010');
    assert.equal(app.elements.get('last-report').textContent, '1 min ago');
    assert.equal(app.elements.get('recent-collars').textContent, '2');
    assert.equal(app.calls[0].url, '/api/welcome');
    assert.equal(app.calls[0].options.cache, 'no-store');
    assert.deepEqual([...app.timers.values()].map(t => t.ms), [15000]);
});
test('empty history and approximate time do not invent report freshness', async () => {
    const app = boot({response:{...fixture,last_report_age_s:null,recent_collars:0,time_synced:false}}); await flush();
    assert.equal(app.elements.get('last-report').textContent, 'No reports yet');
    assert.equal(app.elements.get('clock-state').textContent, 'Approximate');
});
test('failed or malformed snapshots show a non-blocking error and retry slowly', async () => {
    for (const options of [{fail:true},{response:{...fixture,hub_id:'<img>'}},{response:{...fixture,last_report_age_s:-1}}]) {
        const app = boot(options); await flush();
        assert.match(app.elements.get('connection').textContent, /unavailable/);
        assert.equal(app.elements.get('hub-id').textContent, '—');
        assert.deepEqual([...app.timers.values()].map(t => t.ms), [15000]);
    }
});
test('hidden page does not poll; showing resumes and pagehide stops it', async () => {
    const app = boot({hidden:true}); await flush();
    assert.equal(app.calls.length, 0);
    app.document.hidden = false; app.events.visibilitychange(); await flush();
    assert.equal(app.calls.length, 1);
    app.events.pagehide();
    assert.equal(app.timers.size, 0);
    app.events.pageshow(); await flush();
    assert.equal(app.calls.length, 2);
});
test('slow requests cannot overlap and are aborted after five seconds', async () => {
    const app = boot({pending:true}); await flush();
    app.events.pageshow(); app.events.visibilitychange();
    assert.equal(app.calls.length, 1);
    const timeout = [...app.timers.values()].find(t => t.ms === 5000);
    timeout.fn(); await flush();
    assert.equal(app.calls[0].options.signal.aborted, true);
    assert.match(app.elements.get('connection').textContent, /unavailable/);
    assert.deepEqual([...app.timers.values()].map(t => t.ms), [15000]);
});
