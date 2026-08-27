/* Hub data adapter. Card, marker, popup and navigation belong to app.js. */
(function (root) {
    'use strict';
    var latest = null, saveRequest = null;
    function view(s) {
        var guid = parseInt(s.gateway_guid16, 16);
        if (!Number.isInteger(guid) || guid <= 0 || guid > 65535) throw new Error('Invalid hub identity');
        var hasGps = Number.isFinite(s.latitude) && Number.isFinite(s.longitude);
        return {
            // Browser-only key prevents collisions; never sent to collar endpoints.
            id: -guid, entity: 'hub', hub: s, name: s.display_name || 'Home Hub',
            emoji: s.mode === 'home' ? s.home_emoji || '🏡' : s.portable_emoji || '📱',
            colour: /^#[0-9a-f]{6}$/i.test(s.marker_colour) ? s.marker_colour : '#38bdf8',
            hasGps: hasGps, lat: s.latitude, lon: s.longitude, age: 0,
            status: s.mode === 'home' ? 'Home' : s.mode === 'portable' ? 'Portable' : 'Off-Grid',
            rssi: s.wifi_rssi_dbm, snr: null
        };
    }
    root.HubPresencePanel = {
        view: view,
        toggleBluetooth: function () {
            if (latest && saveRequest) saveRequest({ble_enabled: !latest.ble_enabled});
        },
        edit: function () {
            if (!latest || !saveRequest) return;
            var name = window.prompt('Hub name (stored locally)', latest.display_name);
            if (!name) return;
            var home = window.prompt('Home emoji', latest.home_emoji || '🏡');
            if (!home) return;
            var portable = window.prompt('Portable / Off-Grid emoji', latest.portable_emoji || '📱');
            if (!portable) return;
            var colour = window.prompt('Marker colour as a hex value', latest.marker_colour);
            if (!colour) return;
            saveRequest({display_name: name, home_emoji: home, portable_emoji: portable, marker_colour: colour});
        },
        start: function (protectedFetch, onUpdate) {
            var busy = false;
            saveRequest = function (values) {
                return protectedFetch('/api/hub-preferences', {method: 'POST',
                    headers: {'Content-Type': 'application/json'}, body: JSON.stringify(values)})
                    .then(function (r) { if (!r.ok) throw new Error('Could not save hub preferences'); return load(); })
                    .catch(function (e) { window.alert(e.message); });
            };
            function load() {
                if (busy || document.hidden) return Promise.resolve();
                busy = true;
                return fetch('/api/hub-presence', {cache: 'no-store'})
                    .then(function (r) { if (!r.ok) throw new Error('Hub disconnected'); return r.json(); })
                    .then(function (s) { var device = view(s); latest = s; onUpdate(device); })
                    // Leave the last successful receive time untouched on failure.
                    .catch(function () {})
                    .finally(function () { busy = false; });
            }
            load(); setInterval(load, 5000);
            document.addEventListener('visibilitychange', load);
        }
    };
})(globalThis);
