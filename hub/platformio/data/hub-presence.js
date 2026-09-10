/* Hub data adapter. Card, marker, popup and navigation belong to app.js. */
(function (root) {
    'use strict';
    var latest = null, saveRequest = null, feedback = null;
    var profiles = {power_save: 'Power Save — every 180 seconds', normal: 'Normal — every 60 seconds', active: 'Active — every 30 seconds'};
    function timedFetch(send, url, options) {
        var controller = new AbortController();
        var timer = setTimeout(function () { controller.abort(); }, 4000);
        return send(url, Object.assign({}, options, {signal: controller.signal}))
            .finally(function () { clearTimeout(timer); });
    }
    function reportRows(s) {
        return [
            ['Report type','Hub local status','Read directly from this Home Hub; no cloud connection is needed.'],
            ['Hub ID',s.gateway_guid16,'The identity of this Home Hub.'],
            ['Mode',s.mode,'Home, Portable or Off-Grid communications mode.'],
            ['Reporting profile',profiles[s.reporting_profile] || profiles.normal,'Self-report cadence only; LoRa reception and commands remain always on.'],
            ['Coordinates',Number.isFinite(s.latitude) && Number.isFinite(s.longitude) ? s.latitude.toFixed(5)+', '+s.longitude.toFixed(5) : 'Waiting for GPS fix','The hub’s own GPS position, never a collar’s position.'],
            ['GPS fix',s.fix_age_s == null ? 'Not acquired' : s.fix_age_s+' seconds old','Position age is separate from last contact.'],
            ['Battery','No data','Battery telemetry is not supplied yet; this does not mean the battery is empty.'],
            ['Wi-Fi signal',Number.isFinite(s.wifi_rssi_dbm) ? s.wifi_rssi_dbm+' dBm' : 'Not connected','Less negative values indicate a stronger Wi-Fi connection.'],
            ['Home beacon',s.ble_advertising ? 'Advertising' : 'Off','Actual BLE Home beacon activity.'],
            ['Uptime',s.uptime_s+' seconds','Time since the hub last restarted.']
        ];
    }
    function downloadReport(rows) {
        var cell=function(value) { var s=String(value); return '"'+(/^[\s]*[=+\-@]/.test(s) ? "'"+s : s).replace(/"/g,'""')+'"'; };
        var csv=[['Field','Data','Description']].concat(rows).map(function(row){return row.map(cell).join(',');}).join('\r\n');
        var url=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'}));
        var a=document.createElement('a');a.href=url;a.download='bluepaws_hub_report.csv';a.click();URL.revokeObjectURL(url);
    }
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
        feedback: function () { return feedback; },
        report: function(exportOnly) {
            fetch('/api/hub-presence',{cache:'no-store'}).then(function(r){if(!r.ok) throw new Error('Unable to load hub report');return r.json();})
                .then(function(s){
                    var rows=reportRows(s);
                    if(exportOnly) {downloadReport(rows);return;}
                    var old=document.getElementById('hub-report-modal');if(old)old.remove();
                    var modal=document.createElement('div');modal.id='hub-report-modal';modal.className='modal';modal.setAttribute('role','dialog');modal.setAttribute('aria-modal','true');modal.setAttribute('aria-label','Latest Home Hub report');
                    var body=document.createElement('div');body.className='modal-content hub-report';modal.appendChild(body);
                    var title=document.createElement('h2');title.textContent='Latest Home Hub report';body.appendChild(title);
                    var summary=document.createElement('p');summary.textContent=(s.display_name || 'Home Hub')+' responded in '+s.mode+' mode at '+new Date().toLocaleTimeString()+'.';body.appendChild(summary);
                    var table=document.createElement('table');table.className='hub-report-table';body.appendChild(table);
                    [['Field','Data','Description']].concat(rows).forEach(function(row,index){var tr=document.createElement('tr');table.appendChild(tr);row.forEach(function(value){var td=document.createElement(index===0 ? 'th' : 'td');td.textContent=value;tr.appendChild(td);});});
                    var actions=document.createElement('div');actions.className='modal-actions';body.appendChild(actions);
                    var download=document.createElement('button');download.className='btn-primary';download.textContent='Download CSV';download.onclick=function(){downloadReport(rows);};actions.appendChild(download);
                    var close=document.createElement('button');close.className='btn-secondary';close.textContent='Close';close.onclick=function(){modal.remove();};actions.appendChild(close);
                    document.body.appendChild(modal);close.focus();
                }).catch(function(e){window.alert(e.message);});
        },
        toggleBluetooth: function () {
            if (latest && saveRequest) saveRequest({ble_enabled: !latest.ble_enabled});
        },
        configureProfile: function () {
            if (!latest || !saveRequest || (feedback && feedback.state === 'pending')) return;
            var old=document.getElementById('hub-profile-modal'); if(old) old.remove();
            var modal=document.createElement('div'); modal.id='hub-profile-modal'; modal.className='modal';
            modal.setAttribute('role','dialog'); modal.setAttribute('aria-modal','true');
            modal.setAttribute('aria-label','Hub power profile');
            var form=document.createElement('form'); form.className='modal-content'; modal.appendChild(form);
            var title=document.createElement('h2'); title.textContent='Change Hub Power Profile'; form.appendChild(title);
            var group=document.createElement('div'); group.className='form-group'; form.appendChild(group);
            var label=document.createElement('label'); label.textContent='Reporting profile'; label.htmlFor='hub-reporting-profile'; group.appendChild(label);
            var select=document.createElement('select'); select.id='hub-reporting-profile';
            Object.keys(profiles).forEach(function(key) {var option=document.createElement('option');option.value=key;option.textContent=profiles[key];select.appendChild(option);});
            select.value=latest.reporting_profile || 'normal'; group.appendChild(select);
            var note=document.createElement('p'); note.textContent='Only this hub’s own reports change. LoRa reception, Bluetooth and command handling stay on.'; form.appendChild(note);
            var actions=document.createElement('div'); actions.className='modal-actions'; form.appendChild(actions);
            var apply=document.createElement('button'); apply.className='btn-primary'; apply.type='submit'; apply.textContent='Apply profile'; actions.appendChild(apply);
            if (!latest.control_poll_s) {
                apply.disabled=true;
                note.textContent='Update this hub’s firmware before changing its reporting profile.';
            }
            var close=document.createElement('button'); close.className='btn-secondary'; close.type='button'; close.textContent='Close'; close.onclick=function(){modal.remove();}; actions.appendChild(close);
            modal.onkeydown=function(e) {
                if(e.key==='Escape'){e.preventDefault();modal.remove();}
                if(e.key==='Tab' && e.shiftKey && document.activeElement===select){e.preventDefault();close.focus();}
                else if(e.key==='Tab' && !e.shiftKey && document.activeElement===close){e.preventDefault();select.focus();}
            };
            form.onsubmit=function(e){e.preventDefault();saveRequest({reporting_profile:select.value});modal.remove();};
            document.body.appendChild(modal); select.focus();
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
        start: function (protectedFetch, onUpdate, onFeedback) {
            var busy = false, pending = null, confirmationTimer = null;
            function notify(state, text) {
                feedback = {state: state, text: text};
                if (onFeedback && latest) onFeedback(-parseInt(latest.gateway_guid16, 16));
            }
            saveRequest = function (values) {
                if (feedback && feedback.state === 'pending') return Promise.resolve();
                notify('pending', 'Updating hub settings…');
                return timedFetch(protectedFetch, '/api/hub-preferences', {method: 'POST',
                    headers: {'Content-Type': 'application/json'}, body: JSON.stringify(values)})
                    .then(function (r) {
                        if (!r.ok) throw new Error('Could not save hub preferences');
                        if (typeof values.ble_enabled !== 'boolean' && !values.reporting_profile) {
                            notify('confirmed', 'Settings saved on this hub.'); return load();
                        }
                        pending = values;
                        confirmationTimer = setTimeout(function () {
                            pending = null;
                            notify('failed', 'Hub setting not confirmed. Check your connection to the hub before retrying.');
                        }, 8000);
                        return load();
                    })
                    .catch(function () {
                        pending = null;
                        notify('failed', 'Could not confirm the change. Check your connection to the hub before retrying.');
                    });
            };
            function load() {
                if (busy || document.hidden) return Promise.resolve();
                busy = true;
                return timedFetch(fetch, '/api/hub-presence', {cache: 'no-store'})
                    .then(function (r) { if (!r.ok) throw new Error('Hub disconnected'); return r.json(); })
                    .then(function (s) {
                        var device = view(s); latest = s; onUpdate(device);
                        if (pending !== null &&
                            (typeof pending.ble_enabled !== 'boolean' || (s.ble_enabled === pending.ble_enabled && s.ble_settled === true)) &&
                            (!pending.reporting_profile || s.reporting_profile === pending.reporting_profile)) {
                            clearTimeout(confirmationTimer);
                            notify('confirmed', pending.reporting_profile ? 'Reporting profile: ' + profiles[pending.reporting_profile] + ' — confirmed by hub.' :
                                'Bluetooth ' + (pending.ble_enabled ? 'enabled' : 'disabled') + ' — confirmed by hub.');
                            pending = null;
                        }
                    })
                    // Leave the last successful receive time untouched on failure.
                    .catch(function () {})
                    .finally(function () {
                        busy = false;
                        if (pending !== null) setTimeout(load, 250);
                    });
            }
            load(); setInterval(load, 5000);
            document.addEventListener('visibilitychange', load);
        }
    };
})(globalThis);
