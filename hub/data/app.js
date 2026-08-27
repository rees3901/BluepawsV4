/*
  Bluepaws V4 — Hub Web GUI
  ══════════════════════════════════════════════════════════════
  Single-page app that runs in the browser, served from the hub's
  on-chip LittleFS flash storage.

  Key features:
   - Leaflet.js map with Street/Satellite/Topo layers
   - Real-time telemetry via SSE (Server-Sent Events) from GET /events
   - Device cards in a collapsible sidebar showing telemetry + action buttons
   - Follow mode: auto-center map on a specific device
   - Trail mode: breadcrumb polyline showing movement history
   - Measure tool: click-to-measure distance on the map
   - Find modal: trigger collar buzzer + LED flash
   - Command modal: change collar operating mode
   - Settings modal: configure WiFi SSID/password + cloud endpoint
   - Dark/light theme toggle with localStorage persistence
  ══════════════════════════════════════════════════════════════
*/

(function () {
    'use strict';

    // ═══════════════════════════════════════════════
    // Application State
    // ═══════════════════════════════════════════════
    const devices = {};            // Map of device_id → device object (marker, trail, data)
    let map = null;                // Leaflet map instance
    let evtSource = null;          // EventSource for SSE connection
    let measuring = false;         // true when measure tool is active
    let measurePoints = [];        // Array of L.LatLng clicked during measurement
    let measureLine = null;        // Leaflet polyline connecting measure points
    let measureLabels = [];        // Distance labels at each measure point
    let measureMarkers = [];       // Circle markers at each measure point
    let darkMode = true;           // Current theme (persisted to localStorage)
    let followedDeviceId = null;   // Device ID being auto-followed on map (null = none)
    var hubMode = 'home';          // home | portable | off_grid
    var hubPortableMode = false;   // true when hub scans for BLE find beacons
    var fallbackPollingTimer = null;
    var localSessionToken = sessionStorage.getItem('bluepawsLocalSession') || '';
    var bleResults = {};           // Map of device_id → { rssi, age_ms }
    var blePollingTimer = null;    // Interval ID for BLE result polling
    var consoleLog = [];           // Ring buffer of display strings (max 200)
    var consoleLogData = [];       // Structured entries for CSV export
    var MAX_LOG_ENTRIES = 200;
    var deviceLogs = {};           // Per-device display strings: id → string[]
    var deviceLogData = {};        // Per-device structured entries: id → object[]
    var MAX_DEVICE_LOG = 50;       // Keep last 50 messages per collar

    // Each new device gets assigned an emoji avatar and a trail color
    // from these palettes. Cycles if more than 8 devices are tracked.
    const AVATARS = [
        { emoji: '\u{1F431}', color: '#1d9bf0', label: 'Cat'     },
        { emoji: '\u{1F436}', color: '#ff6b35', label: 'Dog'     },
        { emoji: '\u{1F430}', color: '#a855f7', label: 'Rabbit'  },
        { emoji: '\u{1F43E}', color: '#22c55e', label: 'Paw'     },
        { emoji: '\u{1F98A}', color: '#f97316', label: 'Fox'     },
        { emoji: '\u{1F426}', color: '#06b6d4', label: 'Bird'    },
        { emoji: '\u{1F422}', color: '#84cc16', label: 'Turtle'  },
        { emoji: '\u{1F439}', color: '#ec4899', label: 'Hamster' }
    ];

    const TRAIL_COLORS = [
        '#1d9bf0', '#ff6b35', '#a855f7', '#22c55e',
        '#f97316', '#06b6d4', '#84cc16', '#ec4899'
    ];

    let avatarIndex = 0;  // Increments as new devices are discovered

    // ═══════════════════════════════════════════════
    // Signal Quality — 5-Stage Colour-Coded Indicator
    //
    // Combines RSSI (dBm) and SNR (dB) into a single quality score
    // using LoRa best-practice thresholds.
    //
    // RSSI thresholds (LoRa SX1262):
    //   > -80 dBm  = Excellent     -80 to -100 = Good
    //   -100 to -110 = Fair        -110 to -120 = Poor
    //   < -120 dBm = Very Poor
    //
    // SNR thresholds (LoRa):
    //   > 7 dB = Excellent         5 to 7 = Good
    //   0 to 5 = Fair              -5 to 0 = Poor
    //   < -5 dB = Very Poor
    //
    // The combined score is a weighted average: 60% RSSI + 40% SNR,
    // each normalised to a 0–4 scale. The result maps to 5 stages.
    // ═══════════════════════════════════════════════
    function getSignalQuality(rssi, snr) {
        // Score RSSI on 0–4 scale
        var rssiScore;
        if (rssi > -80) rssiScore = 4;
        else if (rssi > -100) rssiScore = 3;
        else if (rssi > -110) rssiScore = 2;
        else if (rssi > -120) rssiScore = 1;
        else rssiScore = 0;

        // Score SNR on 0–4 scale
        var snrScore;
        if (snr > 7) snrScore = 4;
        else if (snr > 5) snrScore = 3;
        else if (snr > 0) snrScore = 2;
        else if (snr > -5) snrScore = 1;
        else snrScore = 0;

        // Weighted average (RSSI 60%, SNR 40%)
        var combined = (rssiScore * 0.6) + (snrScore * 0.4);

        // Map to 5 stages
        if (combined >= 3.5) return { level: 5, label: 'Excellent', color: '#22c55e' };  // Green
        if (combined >= 2.5) return { level: 4, label: 'Good',      color: '#84cc16' };  // Light green
        if (combined >= 1.5) return { level: 3, label: 'Average',   color: '#f59e0b' };  // Amber
        if (combined >= 0.8) return { level: 2, label: 'Poor',      color: '#f97316' };  // Orange
        return                       { level: 1, label: 'Very Poor', color: '#ef4444' };  // Red
    }

    // Render signal quality as 5 bars with colour coding
    // Inline SVG icons for card indicators — styled to match reference graphics
    // Antenna: classic Y-shaped broadcast tower (inverted triangle + vertical mast)
    var ICON_ANTENNA = '<svg class="indicator-icon icon-antenna" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">' +
        '<line x1="12" y1="24" x2="12" y2="10"/>' +
        '<line x1="12" y1="10" x2="3" y2="2"/>' +
        '<line x1="12" y1="10" x2="21" y2="2"/>' +
        '<line x1="3" y1="2" x2="21" y2="2"/>' +
        '</svg>';
    var ICON_HOME_DIST = '<svg class="indicator-icon icon-home" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
        '<path d="M3 12l9-9 9 9"/><path d="M5 10v10a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V10"/>' +
        '</svg>';

    function renderSignalBars(rssi, snr) {
        var sig = getSignalQuality(rssi, snr);
        var bars = '';
        for (var i = 1; i <= 5; i++) {
            var filled = i <= sig.level;
            var height = 4 + (i * 3);  // Bars grow taller: 7, 10, 13, 16, 19px
            bars += '<span class="sig-bar' + (filled ? ' filled' : '') + '" style="' +
                'height:' + height + 'px;' +
                (filled ? 'background:' + sig.color + ';' : '') +
                '"></span>';
        }
        return '<span class="signal-indicator" title="RSSI: ' + rssi + ' dBm / SNR: ' + snr + ' dB — ' + sig.label + '">' +
            ICON_ANTENNA +
            bars +
            '<span class="sig-label" style="color:' + sig.color + '">' + sig.label + '</span>' +
            '</span>';
    }

    // ═══════════════════════════════════════════════
    // Battery Level — 5-Stage Indicator
    //
    // LiPo battery thresholds (single-cell 3.7V nominal):
    //   >= 4.10V = Full (Level 5)
    //   3.95–4.09V = Very Good (Level 4)
    //   3.80–3.94V = Medium (Level 3)
    //   3.65–3.79V = Low (Level 2)
    //   < 3.65V = Nearly Empty (Level 1)
    // ═══════════════════════════════════════════════
    function getBatteryLevel(millivolts) {
        var v = millivolts / 1000;  // Convert mV → V
        if (v >= 4.10) return { level: 5, label: 'Full',         color: '#22c55e' };
        if (v >= 3.95) return { level: 4, label: 'Very Good',    color: '#84cc16' };
        if (v >= 3.80) return { level: 3, label: 'Medium',       color: '#f59e0b' };
        if (v >= 3.65) return { level: 2, label: 'Low',          color: '#f97316' };
        return                { level: 1, label: 'Nearly Empty', color: '#ef4444' };
    }

    function renderBatteryBars(millivolts) {
        var batt = getBatteryLevel(millivolts);
        // Build 5 bars inside the battery body — each bar is a rect
        // Battery body inner area: x 4–21, y 4.5–13.5 → 17px wide, 9px tall
        // 5 bars with gaps: each bar 2.6px wide, gap 0.8px
        var barRects = '';
        for (var i = 0; i < 5; i++) {
            var x = 4 + i * 3.4;
            var fill = (i < batt.level) ? batt.color : '#2f3e4e';
            barRects += '<rect x="' + x + '" y="4.5" width="2.6" height="9" rx="0.6" fill="' + fill + '"/>';
        }
        var svg = '<svg class="indicator-icon icon-battery" viewBox="0 0 28 18" fill="none">' +
            '<rect x="1" y="1" width="23" height="16" rx="3" ry="3" stroke="#607d8b" stroke-width="2"/>' +
            '<rect x="24" y="5.5" width="3" height="7" rx="1.2" fill="#607d8b"/>' +
            barRects +
            '</svg>';
        return '<span class="battery-indicator" title="' + (millivolts / 1000).toFixed(2) + ' V — ' + batt.label + '">' +
            svg +
            '<span class="sig-label" style="color:' + batt.color + '">' + batt.label + '</span>' +
            '</span>';
    }

    // ═══════════════════════════════════════════════
    // BLE Proximity Bars (Portable Mode)
    //
    // When hub is in portable mode, shows signal strength bars
    // based on BLE RSSI from collar find beacons.
    //   >= -50 dBm → 4 bars (very close)
    //   >= -65 dBm → 3 bars (close)
    //   >= -80 dBm → 2 bars (medium)
    //   <  -80 dBm → 1 bar (far)
    // ═══════════════════════════════════════════════
    function renderBleProximity(rssi) {
        var level;
        var label;
        if (rssi >= -50) { level = 4; label = 'Very Close'; }
        else if (rssi >= -65) { level = 3; label = 'Close'; }
        else if (rssi >= -80) { level = 2; label = 'Medium'; }
        else { level = 1; label = 'Far'; }

        var bars = '';
        for (var i = 1; i <= 4; i++) {
            var h = 4 + (i * 3);
            bars += '<span class="ble-bar' + (i <= level ? ' filled' : '') + '" style="height:' + h + 'px"></span>';
        }
        return '<span class="ble-proximity" title="BLE RSSI: ' + rssi + ' dBm — ' + label + '">' +
            bars +
            '<span class="ble-proximity-label">' + label + '</span>' +
            '</span>';
    }

    // ═══════════════════════════════════════════════
    // Hub Home Position — Distance Calculation
    //
    // The hub's home position is set from the first GPS fix received,
    // since the hub is stationary and the collar starts near it.
    // Persisted to localStorage so it survives page refreshes.
    // ═══════════════════════════════════════════════
    var hubHomeLat = null;
    var hubHomeLon = null;

    // Haversine distance between two lat/lon points (returns meters)
    function haversineDistance(lat1, lon1, lat2, lon2) {
        var R = 6371000;  // Earth radius in meters
        var dLat = (lat2 - lat1) * Math.PI / 180;
        var dLon = (lon2 - lon1) * Math.PI / 180;
        var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
                Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
                Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
    }

    // Convert decimal degrees to DMS format (e.g. 51°30'18.0"N)
    function toDMS(dd, posChar, negChar) {
        var dir = dd >= 0 ? posChar : negChar;
        dd = Math.abs(dd);
        var d = Math.floor(dd);
        var m = Math.floor((dd - d) * 60);
        var s = ((dd - d) * 60 - m) * 60;
        return d + '\u00B0' + m.toString().padStart(2, '0') + '\'' + s.toFixed(1).padStart(4, '0') + '"' + dir;
    }

    function formatDistFromHub(lat, lon) {
        if (hubHomeLat === null) return '--';
        var d = haversineDistance(hubHomeLat, hubHomeLon, lat, lon);
        if (d >= 2000) return (d / 1000).toFixed(1) + ' km';
        return Math.round(d) + ' m';
    }

    // ═══════════════════════════════════════════════
    // Console Log — captures SSE events for debugging
    // ═══════════════════════════════════════════════
    function logEvent(type, msg, structured) {
        var ts = new Date().toLocaleTimeString();
        consoleLog.push('[' + ts + '] ' + type + ': ' + msg);
        if (consoleLog.length > MAX_LOG_ENTRIES) consoleLog.shift();
        if (structured) {
            consoleLogData.push(structured);
            if (consoleLogData.length > MAX_LOG_ENTRIES) consoleLogData.shift();
        }
        updateConsoleDisplay();
    }

    // Per-device log — stores formatted lines + structured data keyed by device ID
    function logDeviceEvent(deviceId, line, structured) {
        if (!deviceLogs[deviceId]) deviceLogs[deviceId] = [];
        deviceLogs[deviceId].push(line);
        if (deviceLogs[deviceId].length > MAX_DEVICE_LOG) deviceLogs[deviceId].shift();
        if (structured) {
            if (!deviceLogData[deviceId]) deviceLogData[deviceId] = [];
            deviceLogData[deviceId].push(structured);
            if (deviceLogData[deviceId].length > MAX_DEVICE_LOG) deviceLogData[deviceId].shift();
        }
        updateDeviceLogDisplay(deviceId);
    }

    // ── CSV Export helpers ──
    function toCsvRow(values) {
        return values.map(function (v) {
            var s = String(v == null ? '' : v);
            if (s.indexOf(',') !== -1 || s.indexOf('"') !== -1) {
                return '"' + s.replace(/"/g, '""') + '"';
            }
            return s;
        }).join(',');
    }

    var CSV_HEADER = ['timestamp', 'device_id', 'name', 'lat', 'lon', 'rssi', 'snr', 'batt_mV', 'status', 'profile'];

    function exportLogCsv(entries, filename) {
        if (!entries || !entries.length) return;
        var lines = [toCsvRow(CSV_HEADER)];
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i];
            lines.push(toCsvRow([e.ts, e.id, e.name, e.lat, e.lon, e.rssi, e.snr, e.batt, e.status, e.profile]));
        }
        var blob = new Blob([lines.join('\n')], { type: 'text/csv' });
        var a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = filename;
        a.click();
        URL.revokeObjectURL(a.href);
    }

    function updateDeviceLogDisplay(deviceId) {
        var el = document.getElementById('deviceLog-' + deviceId);
        if (el && !el.parentElement.classList.contains('hidden')) {
            el.textContent = (deviceLogs[deviceId] || []).join('\n');
            el.scrollTop = el.scrollHeight;
        }
    }

    function updateConsoleDisplay() {
        var el = document.getElementById('consoleLogContent');
        if (el && !el.parentElement.classList.contains('hidden')) {
            el.textContent = consoleLog.join('\n');
            el.scrollTop = el.scrollHeight;
        }
    }

    function toggleConsoleLog() {
        var panel = document.getElementById('consoleLogPanel');
        panel.classList.toggle('hidden');
        if (!panel.classList.contains('hidden')) {
            updateConsoleDisplay();
        }
    }

    // ═══════════════════════════════════════════════
    // Map Initialisation
    // Creates a Leaflet map with 3 tile layer options.
    // Default center is London — will auto-recenter when first device data arrives.
    // ═══════════════════════════════════════════════
    function initMap() {
        map = L.map('map', {
            center: [54.5, -3.2],  // UK overview until cached/live collars are available
            zoom: 6,
            zoomControl: false        // We add our own zoom control below
        });

        // Local map-source abstraction. The first implementation is a bundled
        // vector skeleton and coordinate grid; a future SD source can replace
        // it without changing marker/trail code.
        var SkeletonGrid = L.GridLayer.extend({
            createTile: function () {
                var tile = document.createElement('canvas');
                tile.width = tile.height = 256;
                var ctx = tile.getContext('2d');
                ctx.fillStyle = document.body.classList.contains('dark') ? '#071522' : '#dcecf3';
                ctx.fillRect(0, 0, 256, 256);
                ctx.strokeStyle = document.body.classList.contains('dark') ? '#173a52' : '#bad3df';
                ctx.lineWidth = 1;
                for (var p = 0; p <= 256; p += 64) {
                    ctx.beginPath(); ctx.moveTo(p, 0); ctx.lineTo(p, 256); ctx.stroke();
                    ctx.beginPath(); ctx.moveTo(0, p); ctx.lineTo(256, p); ctx.stroke();
                }
                return tile;
            }
        });
        var mapSources = {
            skeleton: new SkeletonGrid({ attribution: 'Bluepaws offline map', maxZoom: 19 })
        };
        mapSources.skeleton.addTo(map);
        fetch('/basemap.json').then(function (response) { return response.json(); }).then(function (data) {
            L.geoJSON(data, {
                style: function () {
                    return { color: '#5f8498', weight: 2, fillColor: '#b8cfad', fillOpacity: 0.52 };
                }
            }).addTo(map);
        }).catch(function () { addConsoleLog('Offline coastline unavailable'); });

        // Zoom control (bottom-left to avoid hamburger overlap)
        L.control.zoom({ position: 'bottomleft' }).addTo(map);

        // Fit All Markers button (map overlay)
        var FitAllControl = L.Control.extend({
            options: { position: 'topleft' },
            onAdd: function () {
                var btn = L.DomUtil.create('div', 'leaflet-map-btn');
                btn.id = 'btnFitAllMap';
                btn.title = 'Fit all markers into view';
                btn.innerHTML = '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5">' +
                    '<circle cx="8" cy="8" r="5"/>' +
                    '<path d="M8 1v3m0 8v3M1 8h3m8 0h3"/>' +
                    '<circle cx="8" cy="8" r="1.5" fill="currentColor" stroke="none"/>' +
                    '</svg>';
                L.DomEvent.disableClickPropagation(btn);
                L.DomEvent.on(btn, 'click', function () { fitAllMarkers(); });
                return btn;
            }
        });
        new FitAllControl().addTo(map);

        // Measure tool (ruler icon)
        var MeasureControl = L.Control.extend({
            options: { position: 'topleft' },
            onAdd: function () {
                var btn = L.DomUtil.create('div', 'leaflet-map-btn');
                btn.id = 'btnMeasure';
                btn.title = 'Measure distance (click points on map)';
                btn.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round">' +
                    '<rect x="1" y="7" width="22" height="10" rx="1"/>' +
                    '<line x1="5" y1="7" x2="5" y2="12"/>' +
                    '<line x1="9" y1="7" x2="9" y2="10"/>' +
                    '<line x1="13" y1="7" x2="13" y2="12"/>' +
                    '<line x1="17" y1="7" x2="17" y2="10"/>' +
                    '<line x1="21" y1="7" x2="21" y2="12"/>' +
                    '</svg>';
                L.DomEvent.disableClickPropagation(btn);
                L.DomEvent.on(btn, 'click', function () { toggleMeasure(); });
                return btn;
            }
        });
        new MeasureControl().addTo(map);

        // Scale bar (bottom-right, km + miles)
        L.control.scale({ position: 'bottomright', imperial: true, metric: true }).addTo(map);

        // Live cursor coordinate display (bottom-right)
        var CoordsControl = L.Control.extend({
            options: { position: 'bottomright' },
            onAdd: function () {
                var div = L.DomUtil.create('div', 'leaflet-cursor-coords');
                div.id = 'cursorCoords';
                div.innerHTML = '--';
                return div;
            }
        });
        new CoordsControl().addTo(map);

        // Update cursor coords on mouse move — shows Lat/Lon (decimal) + DMS
        map.on('mousemove', function (e) {
            var el = document.getElementById('cursorCoords');
            if (!el) return;
            var lat = e.latlng.lat;
            var lon = e.latlng.lng;
            el.innerHTML = lat.toFixed(6) + ', ' + lon.toFixed(6) +
                '<br>' + toDMS(lat, 'N', 'S') + ' ' + toDMS(lon, 'E', 'W');
        });

        // Click handler for measurement mode
        map.on('click', onMapClick);

        // Wire action buttons inside popups when they open
        map.on('popupopen', function (e) {
            var container = e.popup.getElement();
            if (container) wireActionButtons(container);
        });
    }

    // ═══════════════════════════════════════════════
    // Theme Toggle
    // ═══════════════════════════════════════════════
    function toggleTheme() {
        darkMode = !darkMode;
        document.body.classList.toggle('light', !darkMode);

        // Update sidebar theme button icon (moon = dark, sun = light)
        var btn = document.getElementById('btnTheme');
        if (btn) {
            btn.innerHTML = darkMode
                ? '<svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.92-.1-1.36a5.389 5.389 0 0 1-4.4 2.26 5.403 5.403 0 0 1-3.14-9.8c-.44-.06-.9-.1-1.36-.1z"/></svg>'
                : '<svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="5"/><path d="M12 1v2m0 18v2M4.22 4.22l1.42 1.42m12.72 12.72l1.42 1.42M1 12h2m18 0h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>';
        }

        try { localStorage.setItem('bp_theme', darkMode ? 'dark' : 'light'); } catch (e) {}
    }

    function loadTheme() {
        try {
            var saved = localStorage.getItem('bp_theme');
            if (saved === 'light') {
                darkMode = false;
                document.body.classList.add('light');
            }
        } catch (e) {}
    }

    // ═══════════════════════════════════════════════
    // Sidebar Toggle (hamburger)
    // ═══════════════════════════════════════════════
    function toggleSidebar() {
        var panel = document.getElementById('panel');
        var isOpen = panel.classList.toggle('open');
        document.body.classList.toggle('panel-open', isOpen);
        setTimeout(function () { map.invalidateSize(); }, 300);
    }

    // ═══════════════════════════════════════════════
    // SSE Connection with Heartbeat Watchdog
    //
    // The hub sends a "heartbeat" event every 5 seconds.
    // If we don't receive ANY event within 10 seconds, we
    // show a "No heartbeat" warning in the status banner.
    // The EventSource API automatically reconnects on disconnect.
    // ═══════════════════════════════════════════════
    var heartbeatTimer = null;
    var HEARTBEAT_TIMEOUT_MS = 10000;  // Show "No heartbeat" after 10s of silence

    // Reset the watchdog timer — called on every SSE event
    function resetHeartbeatWatchdog() {
        clearTimeout(heartbeatTimer);
        setStatus('connected', 'Connected');
        heartbeatTimer = setTimeout(function () {
            setStatus('disconnected', 'No heartbeat');
        }, HEARTBEAT_TIMEOUT_MS);
    }

    // Open SSE connection to the hub's /events endpoint
    function connectSSE() {
        if (evtSource) evtSource.close();  // Close any existing connection

        evtSource = new EventSource('/events');
        evtSource.addEventListener('cmd_ack', function (e) {
            resetHeartbeatWatchdog();
            try { acceptCommandFeedback(JSON.parse(e.data)); }
            catch (err) { logEvent('ERR', 'Command feedback unavailable: ' + err.message); }
        });

        // "telemetry" events carry device data as JSON
        evtSource.addEventListener('telemetry', function (e) {
            resetHeartbeatWatchdog();
            try {
                var data = JSON.parse(e.data);
                var ts = new Date().toISOString();
                var tsShort = new Date().toLocaleTimeString();
                var structured = { ts: ts, id: data.id, name: data.name, lat: (data.lat||0).toFixed(5), lon: (data.lon||0).toFixed(5), rssi: data.rssi, snr: data.snr, batt: data.batt, status: data.status, profile: data.profile, error: data.errorPresent ? 'Reported fault' : 'None' };
                var logLine = data.name + ' id=' + data.id + ' lat=' + structured.lat + ' lon=' + structured.lon + ' rssi=' + data.rssi + ' batt=' + data.batt + 'mV';
                logEvent('RX', logLine, structured);
                logDeviceEvent(data.id, '[' + tsShort + '] ' + logLine + ' snr=' + data.snr + ' status=' + data.status + ' profile=' + data.profile, structured);
                updateDevice(data);
            } catch (err) {
                logEvent('ERR', 'SSE device update: ' + err.message);
                console.error('SSE device update failed:', err);
            }
        });

        // Heartbeats also carry automatic Wi-Fi/mode transitions.
        evtSource.addEventListener('heartbeat', function (e) {
            resetHeartbeatWatchdog();
            try { syncHubModeState(JSON.parse(e.data)); } catch (err) { /* older firmware */ }
            if (!document.getElementById('settingsModal').classList.contains('hidden')) refreshHubStatus();
        });

        evtSource.addEventListener('verification', function (e) {
            resetHeartbeatWatchdog();
            try {
                var result = JSON.parse(e.data);
                var dev = devices[result.device_id];
                if (dev && dev.data.localId === result.local_id) {
                    dev.data.verification = result.verification;
                    renderDeviceCard(dev);
                }
            } catch (err) {
                logEvent('ERR', 'Verification update: ' + err.message);
            }
        });

        evtSource.addEventListener('appearance', function (e) {
            resetHeartbeatWatchdog();
            try {
                var appearance = JSON.parse(e.data);
                var dev = devices[appearance.id];
                if (!dev) return;
                dev.data.name = appearance.name;
                dev.data.emoji = appearance.emoji;
                dev.data.colour = appearance.colour;
                dev.data.age = Math.max(0, (Date.now() - dev.lastUpdate) / 1000);
                dev.data.rxWindowMs = Math.max(0, (dev.rxUntil || 0) - performance.now());
                updateDevice(dev.data);
            } catch (err) {
                logEvent('ERR', 'Appearance update: ' + err.message);
            }
        });

        evtSource.onopen = function () {
            resetCommandFeedback();
            fetchCommandFeedback();
            if (fallbackPollingTimer) {
                clearInterval(fallbackPollingTimer);
                fallbackPollingTimer = null;
            }
            logEvent('SYS', 'SSE connected');
            resetHeartbeatWatchdog();
        };

        evtSource.onerror = function () {
            resetCommandFeedback();
            logEvent('SYS', 'SSE disconnected');
            clearTimeout(heartbeatTimer);
            setStatus('disconnected', 'Disconnected');
            if (!fallbackPollingTimer) {
                fallbackPollingTimer = setInterval(fetchDeviceSnapshot, 10000);
            }
        };
    }

    function fetchDeviceSnapshot() {
        refreshHubStatus(); // also recover mode state for clients using polling
        fetchCommandFeedback();
        return fetch('/api/devices').then(function (r) { return r.json(); })
            .then(function (items) { items.forEach(updateDevice); });
    }

    function protectedFetch(url, options) {
        options = options || {};
        options.headers = options.headers || {};
        if (localSessionToken) options.headers['X-Bluepaws-Local-Session'] = localSessionToken;
        return fetch(url, options).then(function (response) {
            if (response.status !== 403) return response;
            var pin = window.prompt('Enter the four-digit Off-Grid command PIN');
            if (!pin) return response;
            return fetch('/api/security/unlock', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'pin=' + encodeURIComponent(pin)
            }).then(function (unlockResponse) {
                if (!unlockResponse.ok) throw new Error('PIN unlock failed');
                return unlockResponse.json();
            }).then(function (data) {
                localSessionToken = data.session_token;
                sessionStorage.setItem('bluepawsLocalSession', localSessionToken);
                options.headers['X-Bluepaws-Local-Session'] = localSessionToken;
                return fetch(url, options);
            });
        });
    }

    function editLocalAppearance(deviceId) {
        var dev = devices[deviceId];
        if (!dev) return;
        var name = window.prompt('Local collar name (stored only on this Home Hub)', dev.data.name || ('Device ' + deviceId));
        if (!name) return;
        var emoji = window.prompt('Emoji or short symbol', dev.avatar.emoji || '🐾');
        if (!emoji) return;
        var colour = window.prompt('Marker colour as a hex value', dev.avatar.color || '#1d9bf0');
        if (!colour) return;

        var body = new URLSearchParams({
            device: String(deviceId),
            name: name,
            emoji: emoji,
            colour: colour
        }).toString();
        protectedFetch('/api/device-meta', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (response) {
            if (!response.ok) throw new Error('Appearance was not saved (' + response.status + ')');
            return response.json();
        }).then(function (appearance) {
            dev.data.name = appearance.name;
            dev.data.emoji = appearance.emoji;
            dev.data.colour = appearance.colour;
            updateDevice(dev.data);
        }).catch(function (error) {
            window.alert(error.message);
        });
    }

    // Update the connection status indicator (in sidebar header).
    // Fades out after 20 seconds when connected; stays visible when disconnected.
    var statusFadeTimer = null;
    var lastStatusState = null;

    function setStatus(state, text) {
        var banner = document.getElementById('statusBanner');
        var textEl = document.getElementById('statusText');
        banner.className = state;
        textEl.textContent = text;

        // Show on any state change
        banner.classList.remove('faded');
        clearTimeout(statusFadeTimer);

        if (state === 'connected') {
            // Auto-fade after 20 seconds when connected
            statusFadeTimer = setTimeout(function () {
                banner.classList.add('faded');
            }, 20000);
        }
        // Disconnected stays visible (no fade)

        lastStatusState = state;
    }

    // ═══════════════════════════════════════════════
    // Device Updates
    //
    // Called for every "telemetry" SSE event AND for the initial
    // /api/devices fetch. Creates or updates the device's map marker,
    // trail breadcrumb, and sidebar card.
    // ═══════════════════════════════════════════════
    function updateDevice(data) {
        var id = data.id;
        var dev = devices[id];

        // First time seeing this device — create a new entry with
        // an assigned avatar emoji and trail color
        if (!dev) {
            var av = AVATARS[avatarIndex % AVATARS.length];
            if (data.emoji) av = { emoji: data.emoji, color: data.colour || av.color, label: 'Local' };
            else if (data.colour) av = { emoji: av.emoji, color: data.colour, label: av.label };
            var tc = TRAIL_COLORS[avatarIndex % TRAIL_COLORS.length];
            dev = {
                id: id,
                name: data.name,
                marker: null,       // Leaflet marker (created on first GPS fix)
                trail: [],          // Array of [lat, lon] for breadcrumb trail
                trailLine: null,    // Leaflet polyline for the trail
                showTrail: true,    // Trail visible by default
                avatar: av,         // Assigned emoji + color
                trailColor: tc      // Trail line color
            };
            avatarIndex++;
            devices[id] = dev;
        }

        dev.name = data.name || dev.name;
        if (data.emoji) dev.avatar.emoji = data.emoji;
        if (data.colour) {
            dev.avatar.color = data.colour;
            dev.trailColor = data.colour;
        }

        dev.data = data;               // Store latest telemetry payload
        dev.lastUpdate = Date.now() - Math.max(0, Number(data.age || 0)) * 1000;
        HubFeedback.receiveWindow(dev, data, performance.now());

        // Only update map if we have valid GPS coordinates
        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            var latlng = [data.lat, data.lon];

            if (!dev.marker) {
                // First GPS fix for this device — create a map marker
                // using a custom div icon with the device's emoji avatar
                var icon = L.divIcon({
                    className: '',
                    html: '<div class="bp-marker" id="marker-' + id + '" style="border-color:' + dev.avatar.color + '">' + dev.avatar.emoji + '</div>',
                    iconSize: [32, 32],
                    iconAnchor: [16, 16]  // Center the icon on the position
                });
                dev.marker = L.marker(latlng, { icon: icon }).addTo(map);
                dev.marker.bindPopup('', { minWidth: 240, autoPanPadding: [20, 20] });

                // If this is the first device ever, auto-zoom to it
                if (Object.keys(devices).length === 1) {
                    map.setView(latlng, 16);
                }
            } else {
                // Existing marker — just move it to the new position
                dev.marker.setLatLng(latlng);

                // Trigger the "pop" animation on the marker (scale up then back)
                var el = document.getElementById('marker-' + id);
                if (el) {
                    el.classList.remove('updated');
                    void el.offsetWidth;  // Force reflow to restart CSS animation
                    el.classList.add('updated');
                }
            }

            // Update the popup content with latest telemetry
            dev.marker.setPopupContent(buildPopup(dev));

            // ── GPS accuracy radius circle ──
            // Shows a translucent ring around the marker when GPS accuracy > 15m,
            // similar to Google Maps' blue accuracy circle.
            if (data.acc && data.acc > 15) {
                if (!dev.accCircle) {
                    dev.accCircle = L.circle(latlng, {
                        radius: data.acc,
                        color: dev.avatar.color,
                        fillColor: dev.avatar.color,
                        fillOpacity: 0.08,
                        weight: 1,
                        opacity: 0.3,
                        interactive: false
                    }).addTo(map);
                } else {
                    dev.accCircle.setLatLng(latlng);
                    dev.accCircle.setRadius(data.acc);
                }
            } else if (dev.accCircle) {
                map.removeLayer(dev.accCircle);
                dev.accCircle = null;
            }

            // Apply status-based marker styles (green border for home, red pulse for lost)
            var markerEl = document.getElementById('marker-' + id);
            if (markerEl) {
                markerEl.className = 'bp-marker';
                markerEl.style.borderColor = dev.avatar.color;
                if (data.status === 'Home') markerEl.classList.add('status-home');
                if (data.status === 'Lost' || data.status === 'LostTimeout' || data.status === 'LostAlert') markerEl.classList.add('status-lost');
            }

            // ── Trail breadcrumb line ──
            // Each GPS update adds a point. The local journal retains at most
            // 100 points per collar, matching the off-grid history contract.
            // Renders as a dashed polyline in the device's trail color.
            if (dev.showTrail) {
                dev.trail.push(latlng);
                while (dev.trail.length > 100) dev.trail.shift();
                if (dev.trailLine) {
                    dev.trailLine.setLatLngs(dev.trail);  // Update existing polyline
                } else {
                    // Create new trail polyline on first point
                    dev.trailLine = L.polyline(dev.trail, {
                        color: dev.trailColor,
                        weight: 2,
                        opacity: 0.6,
                        dashArray: '4 4'  // Dashed line
                    }).addTo(map);
                }
            }

            // ── Follow mode ──
            // If this device is being followed, keep the map centered on it
            if (followedDeviceId === id) {
                map.setView(latlng);
            }
        }

        // Update or create the sidebar device card
        renderDeviceCard(dev);
    }

    function loadDeviceHistory(deviceId) {
        return fetch('/api/history?device=' + encodeURIComponent(deviceId) + '&limit=100')
            .then(function (response) { return response.json(); })
            .then(function (payload) {
                var items = payload.items || [];
                deviceLogs[deviceId] = items.map(function (item) {
                    var time = item.gateway_rx_time_unix
                        ? new Date(item.gateway_rx_time_unix * 1000).toLocaleString()
                        : 'Time unavailable';
                    return '[' + time + '] ' + item.tx_reason + ' · ' + item.status +
                        ' · ' + item.profile + ' · ' + item.battery_mv + 'mV · ' +
                        item.rssi_dbm + 'dBm · ' + item.verification;
                });
                deviceLogData[deviceId] = items;

                var dev = devices[deviceId];
                if (!dev) return;
                dev.trail = items.filter(function (item) {
                    return item.has_gps && item.verification !== 'rejected';
                }).map(function (item) { return [item.latitude, item.longitude]; });
                if (dev.trailLine) map.removeLayer(dev.trailLine);
                dev.trailLine = dev.trail.length ? L.polyline(dev.trail, {
                    color: dev.trailColor, weight: 2, opacity: 0.6, dashArray: '4 4'
                }).addTo(map) : null;
                updateDeviceLogDisplay(deviceId);
            });
    }

    // ═══════════════════════════════════════════════
    // Collar Status — emoji + label + offline detection
    // ═══════════════════════════════════════════════
    var STATUS_MAP = {
        'home':  { emoji: '\u{1F3E0}', label: 'Home',    css: 'status-home'  },
        'out':   { emoji: '\u{1F43E}', label: 'Out',     css: 'status-out'   },
        'error': { emoji: '\u2753',    label: 'Error',   css: 'status-error' },
        'lost':  { emoji: '\u2757\u2757', label: 'Lost', css: 'status-lost'  }
    };
    var STATUS_OFFLINE = { emoji: '\u26AB', label: 'Offline', css: 'status-offline' };
    var OFFLINE_THRESHOLD_MS = 600000;  // Fixed 10-minute local stale threshold

    function getCollarStatus(dev) {
        var age = Date.now() - dev.lastUpdate;
        if (age >= OFFLINE_THRESHOLD_MS) return STATUS_OFFLINE;
        var key = (dev.data.status || '').toLowerCase();
        if (key === 'losttimeout' || key === 'lost alert' || key === 'lost_alert') key = 'lost';
        return STATUS_MAP[key] || { emoji: '?', label: 'Unknown', css: 'status-unknown' };
    }

    // Format elapsed time compactly for the stopwatch: 23s, 1m 10s, 2h 5m
    function formatLastSeen(seconds) {
        if (seconds < 60) return seconds + 's';
        if (seconds < 3600) {
            var m = Math.floor(seconds / 60);
            var s = seconds % 60;
            return m + 'm ' + s + 's';
        }
        var h = Math.floor(seconds / 3600);
        var rm = Math.floor((seconds % 3600) / 60);
        return h + 'h ' + rm + 'm';
    }

    // Stopwatch SVG icon (small, inline)
    var ICON_STOPWATCH = '<svg class="indicator-icon icon-stopwatch" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
        '<circle cx="12" cy="13" r="8"/>' +
        '<line x1="12" y1="9" x2="12" y2="13"/>' +
        '<line x1="9" y1="1" x2="15" y2="1"/>' +
        '<line x1="12" y1="1" x2="12" y2="5"/>' +
        '</svg>';

    function buildPopup(dev) {
        var data = dev.data;
        var isFollowed = (followedDeviceId === dev.id);
        var distStr = (data.hasGps && data.lat !== 0 && data.lon !== 0)
            ? formatDistFromHub(data.lat, data.lon) : '--';
        var st = getCollarStatus(dev);
        return '<div class="popup-content">' +
            '<div class="popup-header">' +
                '<span style="font-size:20px">' + dev.avatar.emoji + '</span> ' +
                '<strong>' + data.name + '</strong>' +
                '<span class="card-status ' + st.css + '" style="margin-left:6px;font-size:10px">' + st.emoji + ' ' + st.label + '</span>' +
            '</div>' +
            '<div class="popup-grid">' +
                '<span class="label">Signal</span><span class="value">' + renderSignalBars(data.rssi, data.snr) + '</span>' +
                '<span class="label">Battery</span><span class="value">' + renderBatteryBars(data.batt) + '</span>' +
                '<span class="label">Dist From Hub</span><span class="value">' + distStr + '</span>' +
            '</div>' +
            '<div class="card-actions popup-actions">' +
                buildActionButtons(dev, isFollowed) +
            '</div>' +
            '</div>';
    }

    // ═══════════════════════════════════════════════
    // Device Cards — Sidebar UI (Collapsible)
    //
    // Cards have two states:
    //  Collapsed (default): compact summary — avatar, name, status badge,
    //                       signal indicator, and coordinates
    //  Expanded (on click):  full telemetry grid + action buttons (Jump,
    //                        Follow, Trail, Find, Cmd)
    //
    // Clicking the compact header area toggles expand/collapse.
    // ═══════════════════════════════════════════════
    // Shared action buttons HTML used in both card detail and map popup
    function buildActionButtons(dev, isFollowed) {
        return '<button class="btn-action btn-jump" data-action="jump" data-id="' + dev.id + '" title="Jump to location">' +
                '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M1 8h11M8 4l4 4-4 4"/></svg>' +
                ' Jump To' +
            '</button>' +
            '<button class="btn-action btn-follow' + (isFollowed ? ' active' : '') + '" data-action="follow" data-id="' + dev.id + '" title="Auto-follow on map">' +
                '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0a5.5 5.5 0 00-5.5 5.5C2.5 10 8 16 8 16s5.5-6 5.5-10.5A5.5 5.5 0 008 0zm0 8a2.5 2.5 0 110-5 2.5 2.5 0 010 5z"/></svg>' +
                (isFollowed ? ' Following' : ' Follow') +
            '</button>' +
            '<button class="btn-action btn-trail' + (dev.showTrail ? ' active' : '') + '" data-action="trail" data-id="' + dev.id + '" title="Toggle breadcrumb trail">' +
                '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M2 14l4-4 3 3 5-7v2l-5 7-3-3-4 4v-2z"/></svg>' +
                ' Trail' +
            '</button>' +
            '<button class="btn-action btn-find" data-action="find" data-id="' + dev.id + '" title="Find Alert — trigger buzzer + LED">' +
                '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M8 1a4 4 0 00-4 4c0 1.2.4 2 1 3l-2 5h10l-2-5c.6-1 1-1.8 1-3a4 4 0 00-4-4zm0 13a2 2 0 01-2-2h4a2 2 0 01-2 2z"/></svg>' +
                ' Find Alert' +
            '</button>' +
            '<button class="btn-action btn-cmd" data-action="cmd" data-id="' + dev.id + '" title="Command & Control">' +
                '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M6 1h4v2h3v4h-2V5H5v2H3V3h3V1zm4 14H6v-2H3v-4h2v2h6v-2h2v4h-3v2z"/></svg>' +
                ' Cmd' +
            '</button>';
    }

    // Wire action button clicks (works for both card and popup contexts)
    function wireActionButtons(container) {
        var buttons = container.querySelectorAll('.btn-action');
        buttons.forEach(function (btn) {
            btn.addEventListener('click', function (e) {
                e.stopPropagation();
                var action = btn.getAttribute('data-action');
                var devId = parseInt(btn.getAttribute('data-id'), 10);
                var dev = devices[devId];
                if (!dev) return;
                if (action === 'jump') focusDevice(devId);
                if (action === 'follow') toggleFollow(devId);
                if (action === 'trail') toggleTrail(devId);
                if (action === 'find') openFindModal(devId, dev.data.name);
                if (action === 'cmd') sendModeCmd(devId, dev.data.name);
            });
        });
    }

    var expandedCardId = null;  // Only one card expanded at a time

    function toggleCardExpand(deviceId) {
        if (expandedCardId === deviceId) {
            expandedCardId = null;  // Collapse
        } else {
            expandedCardId = deviceId;  // Expand this one
        }
        // Re-render all cards to update expanded state
        for (var id in devices) {
            renderDeviceCard(devices[id]);
        }
    }

    function renderDeviceCard(dev) {
        var data = dev.data;
        var container = document.getElementById('deviceCards');
        var card = document.getElementById('card-' + dev.id);
        var isNew = false;

        // Create card element if this is a new device
        if (!card) {
            card = document.createElement('div');
            card.id = 'card-' + dev.id;
            card.className = 'device-card';
            container.appendChild(card);
            isNew = true;
        }

        // Calculate time since last update — cards older than 10 minutes get dimmed
        var age = Math.floor((Date.now() - dev.lastUpdate) / 1000);
        var stale = age >= 600;  // 10 minutes
        var isExpanded = (expandedCardId === dev.id);
        card.className = 'device-card' + (stale ? ' stale' : '') + (isExpanded ? ' expanded' : '');

        var st = getCollarStatus(dev);
        var isFollowed = (followedDeviceId === dev.id);

        // Distance from hub (used in both collapsed and expanded views)
        var distStr = (data.hasGps && data.lat !== 0 && data.lon !== 0)
            ? formatDistFromHub(data.lat, data.lon) : '--';

        // Last seen compact time
        var lastSeenStr = formatLastSeen(age);

        // Profile badge — colour-coded with optional emoji prefix
        var profileLabel = HubFeedback.profileLabel(data.profile);
        var profileLower = profileLabel.toLowerCase().replace(/ /g, '');
        var profileClass = 'profile-' + (profileLower === 'lostalert' ? 'lost' : profileLower.replace('save', ''));
        if (profileLower === 'powersave') profileLabel = '\u{1F4A4} PowerSave';
        if (profileLower === 'debug') profileLabel = '\u{1F9EA} Debug';

        // ── Compact summary (always visible) ──
        // Row 1: avatar, name, status badge, profile badge, chevron
        // Row 2: battery | signal
        // Row 3: distance from home | last seen
        var html =
            '<div class="card-summary">' +
                '<div class="card-avatar" style="border-color:' + dev.avatar.color + '">' + dev.avatar.emoji + '</div>' +
                '<div class="card-identity">' +
                    '<div class="card-name-row">' +
                        '<span class="card-name">' + data.name + '</span>' +
                        '<span class="card-status ' + st.css + '">' + st.emoji + ' ' + st.label + '</span>' +
                        '<span class="card-profile ' + profileClass + '">' + profileLabel + '</span>' +
                        (data.verification === 'pending' ? '<span class="verification-badge pending">Locally received — verification pending</span>' : '') +
                        (data.verification === 'rejected' ? '<span class="verification-badge rejected">Rejected by cloud</span>' : '') +
                        (data.errorPresent === true ? '<span class="error-badge" title="The collar set its ERROR_PRESENT flag. Lost Alert alone is not a fault.">Collar reported a fault</span>' : '') +
                    '</div>' +
                    '<div class="card-indicators">' +
                        '<span class="card-indicator-group">' + renderBatteryBars(data.batt) + '</span>' +
                        '<span class="card-indicator-group">' + renderSignalBars(data.rssi, data.snr) + '</span>' +
                        '<span class="collar-awake" data-awake="' + dev.id + '" hidden></span>' +
                        (hubPortableMode && bleResults[dev.id] ? '<span class="card-indicator-group">' + renderBleProximity(bleResults[dev.id].rssi) + '</span>' : '') +
                    '</div>' +
                    '<div class="card-indicators card-indicators-row3">' +
                        '<span class="card-indicator-group card-dist-group" title="Distance from home">' +
                            ICON_HOME_DIST +
                            '<span class="card-dist-value">' + distStr + '</span>' +
                        '</span>' +
                        '<span class="card-indicator-group card-lastseen-group" title="Last seen">' +
                            ICON_STOPWATCH +
                            '<span class="card-lastseen-value">' + lastSeenStr + '</span>' +
                        '</span>' +
                    '</div>' +
                '</div>' +
                '<span class="card-chevron">' + (isExpanded ? '&#9650;' : '&#9660;') + '</span>' +
            '</div>';

        // ── Expanded detail (shown only when card is expanded) ──
        if (isExpanded) {
            // Coordinate display — hyperlinked to Google Maps
            var coordHtml = '<span class="card-coords">---, ---</span>';
            if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
                var coordStr = data.lat.toFixed(5) + ', ' + data.lon.toFixed(5);
                var gmapsUrl = 'https://www.google.com/maps?q=' + data.lat.toFixed(6) + ',' + data.lon.toFixed(6);
                coordHtml =
                    '<a href="' + gmapsUrl + '" target="_blank" rel="noopener" class="card-coords card-coords-link" title="Open in Google Maps">' + coordStr + '</a>';
            }

            var logEntries = deviceLogs[dev.id] || [];
            var logContent = logEntries.length ? logEntries.join('\n') : 'No messages yet.';

            html +=
                '<div class="card-detail">' +
                    '<div class="card-grid">' +
                        '<span class="label">Coordinates</span><span class="value">' + coordHtml + '</span>' +
                        '<span class="label">Power Profile</span><span class="value">' + data.profile + '</span>' +
                        '<span class="label">Dist From Hub</span><span class="value">' + distStr + '</span>' +
                        '<span class="label">Last seen</span><span class="value" data-detail-age>' + formatAge(age) + '</span>' +
                    '</div>' +

                    '<div class="card-actions">' +
                        buildActionButtons(dev, isFollowed) +
                    '</div>' +
                    '<div class="command-feedback" data-command-feedback="' + dev.id + '" role="status" hidden></div>' +

                    '<div class="log-btn-row">' +
                        '<button class="btn-device-log btn-secondary" data-logid="' + dev.id + '">Message Log</button>' +
                        '<button class="btn-device-appearance btn-secondary" data-deviceid="' + dev.id + '">Local appearance</button>' +
                        '<button class="btn-log-export btn-export-device" data-logid="' + dev.id + '" data-name="' + data.name + '" title="Export log as CSV"><svg class="icon-download" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M12 4v12m0 0l-4-4m4 4l4-4"/><path d="M5 20h14"/></svg></button>' +
                    '</div>' +
                    '<div id="deviceLogPanel-' + dev.id + '" class="device-log-panel hidden">' +
                        '<pre id="deviceLog-' + dev.id + '" class="console-log device-log">' + logContent + '</pre>' +
                    '</div>' +
                '</div>';
        }

        card.innerHTML = html;
        updateCardFeedback(dev);

        // Wire up action buttons and message log toggle (only present when expanded)
        if (isExpanded) {
            wireActionButtons(card);
            var logBtn = card.querySelector('.btn-device-log');
            if (logBtn) {
                logBtn.addEventListener('click', function (e) {
                    e.stopPropagation();
                    var did = logBtn.getAttribute('data-logid');
                    var panel = document.getElementById('deviceLogPanel-' + did);
                    panel.classList.toggle('hidden');
                    if (!panel.classList.contains('hidden')) {
                        loadDeviceHistory(parseInt(did, 10)).catch(function () {
                            updateDeviceLogDisplay(parseInt(did, 10));
                        });
                    }
                });
            }
            var exportBtn = card.querySelector('.btn-export-device');
            if (exportBtn) {
                exportBtn.addEventListener('click', function (e) {
                    e.stopPropagation();
                    var did = parseInt(exportBtn.getAttribute('data-logid'), 10);
                    window.location.href = '/api/history.csv?device=' + encodeURIComponent(did);
                });
            }
            var appearanceBtn = card.querySelector('.btn-device-appearance');
            if (appearanceBtn) {
                appearanceBtn.addEventListener('click', function (e) {
                    e.stopPropagation();
                    editLocalAppearance(dev.id);
                });
            }
        }

        // Sheen animation on update
        if (!isNew) {
            card.classList.remove('sheen');
            void card.offsetWidth;
            card.classList.add('sheen');
        }

        // Click card summary to toggle expand/collapse (ignore link clicks)
        var summary = card.querySelector('.card-summary');
        if (summary) {
            summary.addEventListener('click', function (e) {
                if (e.target.closest('.btn-action') || e.target.closest('a')) return;
                toggleCardExpand(dev.id);
            });
        }
    }

    // Feedback changes in place; timers must not rebuild controls or animations.
    var commandFeedback = HubFeedback.createStore();
    var commandFetchPending = false;
    var commandFetchAgain = false;
    var commandFeedbackGeneration = 0;
    function resetCommandFeedback() {
        commandFeedbackGeneration++;
        commandFeedback.reset();
        for (var id in devices) updateCardFeedback(devices[id]);
    }
    function acceptCommandFeedback(item) {
        if (commandFeedback.accept(item) && devices[item.device]) updateCardFeedback(devices[item.device]);
    }
    function fetchCommandFeedback() {
        if (commandFetchPending) { commandFetchAgain = true; return; }
        commandFetchPending = true;
        var generation = commandFeedbackGeneration;
        return fetch('/api/commands', {cache:'no-store'})
            .then(function (r) { if (!r.ok) throw new Error('Command snapshot unavailable'); return r.json(); })
            .then(function (items) {
                if (generation === commandFeedbackGeneration && Array.isArray(items)) items.forEach(acceptCommandFeedback);
            })
            .catch(function () { /* SSE or next recovery poll retries; never claim an ACK. */ })
            .finally(function () {
                commandFetchPending = false;
                if (commandFetchAgain) { commandFetchAgain = false; fetchCommandFeedback(); }
            });
    }
    function updateCardFeedback(dev) {
        var card = document.getElementById('card-' + dev.id);
        if (!card) return;
        var awake = card.querySelector('[data-awake]');
        if (awake) {
            var seconds = Math.max(0, Math.ceil(((dev.rxUntil || 0) - performance.now()) / 1000));
            awake.hidden = seconds === 0;
            awake.textContent = seconds ? '💡' : '';
            awake.title = 'Recently heard — expected command receive window, not a guarantee of delivery';
            awake.setAttribute('aria-label', 'Recently heard; expected receive window ' + seconds + ' seconds');
        }
        var line = card.querySelector('[data-command-feedback]');
        if (line) {
            var feedback = commandFeedback.latest(dev.id);
            line.hidden = !feedback;
            line.textContent = feedback ? feedback.text : '';
            var className = 'command-feedback' + (feedback ? ' ' + (feedback.pending ? 'pending' : feedback.status) : '');
            if (line.className !== className) line.className = className;
        }
    }

    // Human-friendly time display (e.g. "just now", "5s ago", "3m ago", "2h ago")
    function formatAge(seconds) {
        if (seconds < 5) return 'just now';
        if (seconds < 60) return seconds + 's ago';
        if (seconds < 3600) return Math.floor(seconds / 60) + 'm ago';
        return Math.floor(seconds / 3600) + 'h ago';
    }

    // Update text in place; no radio/network polling and no form reset.
    setInterval(function () {
        for (var id in devices) {
            var dev = devices[id], card = document.getElementById('card-' + dev.id);
            if (!card) continue;
            var age = Math.max(0, Math.floor((Date.now() - dev.lastUpdate) / 1000));
            card.classList.toggle('stale', age >= 600);
            var seen = card.querySelector('.card-lastseen-value');
            if (seen) seen.textContent = formatLastSeen(age);
            var detailAge = card.querySelector('[data-detail-age]');
            if (detailAge) detailAge.textContent = formatAge(age);
            var status = card.querySelector('.card-status');
            if (status) {
                var st = getCollarStatus(dev);
                status.className = 'card-status ' + st.css;
                status.textContent = st.emoji + ' ' + st.label;
            }
            updateCardFeedback(dev);
        }
    }, 250);

    // ═══════════════════════════════════════════════
    // Follow Mode
    // ═══════════════════════════════════════════════
    function toggleFollow(deviceId) {
        if (followedDeviceId === deviceId) {
            followedDeviceId = null;
        } else {
            followedDeviceId = deviceId;
            var dev = devices[deviceId];
            if (dev && dev.marker) {
                map.setView(dev.marker.getLatLng());
            }
        }
        // Re-render all cards to update follow button state
        for (var id in devices) {
            renderDeviceCard(devices[id]);
        }
    }

    // ═══════════════════════════════════════════════
    // Trail Toggle
    // ═══════════════════════════════════════════════
    function toggleTrail(deviceId) {
        var dev = devices[deviceId];
        if (!dev) return;

        dev.showTrail = !dev.showTrail;

        if (!dev.showTrail && dev.trailLine) {
            map.removeLayer(dev.trailLine);
            dev.trailLine = null;
        } else if (dev.showTrail && dev.trail.length > 1) {
            dev.trailLine = L.polyline(dev.trail, {
                color: dev.trailColor,
                weight: 2,
                opacity: 0.6,
                dashArray: '4 4'
            }).addTo(map);
        }

        renderDeviceCard(dev);
    }

    // ═══════════════════════════════════════════════
    // Measurement Tool
    //
    // Click the ruler button, then click points on the map to
    // measure distance. Each click adds a point; a dashed line
    // connects them and a label shows cumulative distance.
    // Click the ruler again to clear and exit measure mode.
    // ═══════════════════════════════════════════════
    function toggleMeasure() {
        measuring = !measuring;
        var btn = document.getElementById('btnMeasure');
        if (btn) btn.classList.toggle('active', measuring);

        if (!measuring) {
            clearMeasure();  // Remove all measure markers/lines when deactivating
        }

        // Change cursor to crosshair while measuring
        map.getContainer().style.cursor = measuring ? 'crosshair' : '';
    }

    // Handle map clicks — only active when measure tool is on
    function onMapClick(e) {
        if (!measuring) return;

        measurePoints.push(e.latlng);

        // Add a small blue dot at the clicked point
        var cm = L.circleMarker(e.latlng, {
            radius: 4,
            color: '#1d9bf0',
            fillOpacity: 1
        }).addTo(map);
        measureMarkers.push(cm);

        // After 2+ points, draw/extend the line and show cumulative distance
        if (measurePoints.length > 1) {
            var total = totalMeasureDistance();

            if (measureLine) {
                measureLine.addLatLng(e.latlng);  // Extend existing line
            } else {
                measureLine = L.polyline(measurePoints, {
                    color: '#1d9bf0',
                    weight: 2,
                    dashArray: '6 4'
                }).addTo(map);
            }

            // Show distance label at this point
            var label = L.marker(e.latlng, {
                icon: L.divIcon({
                    className: 'measure-label',
                    html: formatDistance(total),
                    iconSize: null
                })
            }).addTo(map);
            measureLabels.push(label);
        }
    }

    // Sum up distances between all consecutive measure points
    function totalMeasureDistance() {
        var total = 0;
        for (var i = 1; i < measurePoints.length; i++) {
            total += measurePoints[i - 1].distanceTo(measurePoints[i]);  // Leaflet's distanceTo uses Haversine
        }
        return total;
    }

    // Format distance for display (meters or km)
    function formatDistance(meters) {
        if (meters < 1000) return Math.round(meters) + ' m';
        return (meters / 1000).toFixed(2) + ' km';
    }

    // Remove all measurement artifacts from the map
    function clearMeasure() {
        measurePoints = [];
        if (measureLine) {
            map.removeLayer(measureLine);
            measureLine = null;
        }
        measureLabels.forEach(function (l) { map.removeLayer(l); });
        measureLabels = [];
        measureMarkers.forEach(function (m) { map.removeLayer(m); });
        measureMarkers = [];
    }

    // ═══════════════════════════════════════════════
    // Fit All Markers
    // Zooms the map to show all tracked devices at once.
    // Adds 20% padding so markers aren't right at the edge.
    // ═══════════════════════════════════════════════
    function fitAllMarkers() {
        var bounds = [];
        if (typeof HubPresencePanel !== 'undefined' && HubPresencePanel.point()) bounds.push(HubPresencePanel.point());
        for (var id in devices) {
            if (devices[id].marker) {
                bounds.push(devices[id].marker.getLatLng());
            }
        }
        if (bounds.length > 0) {
            map.fitBounds(L.latLngBounds(bounds).pad(0.2));
        }
    }

    // ═══════════════════════════════════════════════
    // Settings Modal
    // Opens a dialog to configure WiFi SSID/password and cloud endpoint.
    // Also fetches and displays hub diagnostics (uptime, memory, etc.).
    // ═══════════════════════════════════════════════
    // Validate SSID + password and enable/disable Save button
    function validateConfigForm() {
        var ssid = document.getElementById('cfgSSID').value.trim();
        var pass = document.getElementById('cfgPass').value;
        var btn = document.getElementById('btnSaveConfig');
        // SSID: 1-32 chars, printable ASCII. Password: 8-63 chars (WPA2 spec) or empty (open network).
        var ssidValid = !ssid.length || (ssid.length <= 32 && /^[\x20-\x7E]+$/.test(ssid));
        var passValid = pass.length === 0 || (pass.length >= 8 && pass.length <= 63);
        var secondary = document.getElementById('cfgSecondarySSID').value;
        var secondaryPass = document.getElementById('cfgSecondaryPass').value;
        var secondaryValid = secondary.length <= 32 && (!secondaryPass.length || (secondaryPass.length >= 8 && secondaryPass.length <= 63));
        btn.disabled = !(ssidValid && passValid && secondaryValid);
    }

    function openSettings() {
        document.getElementById('settingsModal').classList.remove('hidden');
        validateConfigForm();  // Set initial button state
        refreshHubStatus();
    }

    function syncHubModeState(s) {
        if (['home', 'portable', 'off_grid'].indexOf(s.hubMode) === -1) return;
        if (hubMode === 'off_grid' && s.hubMode !== 'off_grid') {
            localSessionToken = '';
            sessionStorage.removeItem('bluepawsLocalSession');
        }
        hubMode = s.hubMode;
        hubPortableMode = hubMode !== 'home';
        updateHubModeUI();
        if (hubPortableMode && !blePollingTimer) startBlePolling();
        if (!hubPortableMode && blePollingTimer) stopBlePolling();
        document.getElementById('connectionAvailable').classList.toggle(
            'hidden', !(hubMode === 'off_grid' && s.known_wifi_available));
    }

    function refreshHubStatus() {
        // Fetch hub status to display diagnostics in the modal
        return fetch('/api/status')
            .then(function (r) { return r.json(); })
            .then(function (s) {
                var provisioning = s.provisioning_mode === true && s.hubMode !== 'off_grid';
                document.getElementById('provisioningFields').classList.toggle('hidden', !provisioning);
                document.getElementById('btnSaveConfig').classList.toggle('hidden', !provisioning);
                document.getElementById('hubStatus').innerHTML =
                    'Uptime: ' + formatAge(s.uptime) + '<br>' +
                    'Packets RX: ' + s.rxCount + '<br>' +
                    'Commands TX: ' + s.txCount + '<br>' +
                    'CRC Fails: ' + s.crcFails + '<br>' +
                    'Devices: ' + s.devices + '<br>' +
                    'Log entries: ' + s.logEntries + '<br>' +
                    'Free heap: ' + (s.freeHeap / 1024).toFixed(1) + ' KB<br>' +
                    'WiFi STA: ' + (s.staConnected ? s.staIP : 'Not connected') + '<br>' +
                    'Network: ' + (s.network_phase || 'Unknown') + '<br>' +
                    'Recovery remaining: ' + Math.ceil((s.recovery_remaining_ms || 0) / 1000) + ' s<br>' +
                    'AP IP: ' + s.apIP + '<br>' +
                    'AP clients: ' + (s.ap_clients || 0) + ' / 8 · channel ' + (s.ap_channel || '—') + '<br>' +
                    'AP start failures: ' + (s.ap_start_failures || 0) + '<br>' +
                    'Network / web stack spare: ' + (s.network_stack_free || 0) + ' / ' + (s.web_stack_free || 0) + ' bytes';
                // Sync hub mode state from server
                syncHubModeState(s);
            })
            .catch(function () {
                document.getElementById('hubStatus').textContent = 'Failed to load status';
            });
    }

    function closeSettings() {
        document.getElementById('settingsModal').classList.add('hidden');
    }

    // POST new WiFi/cloud config to the hub. The hub saves to flash and restarts.
    function saveConfig() {
        var ssid = document.getElementById('cfgSSID').value;
        var pass = document.getElementById('cfgPass').value;
        var cloud = document.getElementById('cfgCloud').value;

        var body = 'ssid=' + encodeURIComponent(ssid) +
                   '&pass=' + encodeURIComponent(pass) +
                   '&secondary_ssid=' + encodeURIComponent(document.getElementById('cfgSecondarySSID').value) +
                   '&secondary_pass=' + encodeURIComponent(document.getElementById('cfgSecondaryPass').value) +
                   '&clear_secondary=' + document.getElementById('cfgClearSecondary').checked +
                   '&cloud_url=' + encodeURIComponent(cloud);

        fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (response) {
            if (!response.ok) throw new Error('Configuration rejected');
            alert('Configuration saved. Hub will restart.');
        }).catch(function () {
            alert('Failed to save configuration.');
        });
    }

    // ═══════════════════════════════════════════════
    // Hub Mode Toggle (Home / Portable)
    //
    // In Portable mode, the hub stops its BLE home beacon and starts
    // scanning for collar BLE find beacons. We poll GET /api/ble every
    // 2 seconds to get RSSI proximity data for the device cards.
    // ═══════════════════════════════════════════════
    function setHubMode(mode) {
        var leavingOffGrid = hubMode === 'off_grid' && mode !== 'off_grid';
        if (leavingOffGrid && !window.confirm('Leave Off-Grid mode? Collar states, including Lost Alert, will not be changed.')) return;
        protectedFetch('/api/hub-mode', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'mode=' + mode + (leavingOffGrid ? '&confirm=true' : '')
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              if (d.pending) {
                  document.getElementById('hubStatus').textContent =
                      'Switching network. Leaving Off-Grid disconnects this hotspot; join the selected Wi-Fi. If neither network connects, the hotspot returns after 30 seconds.';
                  setTimeout(refreshHubStatus, 1000);
                  return;
              }
              if (!d.mode) throw new Error(d.error || 'Mode change failed');
              hubMode = d.mode;
              hubPortableMode = (hubMode === 'portable' || hubMode === 'off_grid');
              if (hubMode !== 'off_grid') {
                  localSessionToken = '';
                  sessionStorage.removeItem('bluepawsLocalSession');
              }
              updateHubModeUI();
              if (hubPortableMode) {
                  startBlePolling();
              } else {
                  stopBlePolling();
              }
          })
          .catch(function () {
              logEvent('ERR', 'Failed to set hub mode');
          });
    }

    function updateHubModeUI() {
        var btnHome = document.getElementById('btnHomeMode');
        var btnPortable = document.getElementById('btnPortableMode');
        var btnOffGrid = document.getElementById('btnOffGridMode');
        if (btnHome && btnPortable && btnOffGrid) {
            btnHome.classList.toggle('active', hubMode === 'home');
            btnPortable.classList.toggle('active', hubMode === 'portable');
            btnOffGrid.classList.toggle('active', hubMode === 'off_grid');
        }

        // Show/hide portable banner in sidebar header
        var banner = document.getElementById('portableBanner');
        if (!banner && hubPortableMode) {
            banner = document.createElement('div');
            banner.id = 'portableBanner';
            banner.className = 'portable-banner';
            banner.textContent = hubMode === 'off_grid' ? 'OFF-GRID MODE' : 'PORTABLE MODE';
            var panel = document.getElementById('panelHeader');
            if (panel) panel.after(banner);
        }
        if (banner) {
            banner.style.display = hubPortableMode ? '' : 'none';
            banner.textContent = hubMode === 'off_grid' ? 'OFF-GRID MODE' : 'PORTABLE MODE';
        }
    }

    function setLocalPin(enabled) {
        var pin = document.getElementById('localCommandPin').value.trim();
        var body = enabled ? 'pin=' + encodeURIComponent(pin) : 'enabled=false';
        protectedFetch('/api/security/pin', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) {
            if (!r.ok) return r.json().then(function (d) { throw new Error(d.error); });
            return r.json();
        }).then(function (d) {
            alert(d.pin_enabled ? 'Command PIN enabled for this Off-Grid session.' : 'Command PIN disabled.');
            document.getElementById('localCommandPin').value = '';
        }).catch(function (error) { alert('PIN change failed: ' + error.message); });
    }

    function startBlePolling() {
        stopBlePolling();
        pollBle();
        blePollingTimer = setInterval(pollBle, 2000);
    }

    function stopBlePolling() {
        if (blePollingTimer) {
            clearInterval(blePollingTimer);
            blePollingTimer = null;
        }
        bleResults = {};
    }

    function pollBle() {
        fetch('/api/ble')
            .then(function (r) { return r.json(); })
            .then(function (results) {
                bleResults = {};
                results.forEach(function (r) {
                    bleResults[r.id] = { rssi: r.rssi, age_ms: r.age_ms };
                });
                // Re-render cards to show BLE proximity
                for (var id in devices) {
                    renderDeviceCard(devices[id]);
                }
            })
            .catch(function () { /* silently ignore polling failures */ });
    }

    // ═══════════════════════════════════════════════
    // Command Modal — Change Collar Power Profile
    //
    // Opens a dialog where the user selects a profile (Normal, Power Save,
    // Active, Emergency Lost) and sends it to the collar via
    // POST /api/command. The hub builds a LoRa PKT_CMD_MODE packet.
    // ═══════════════════════════════════════════════
    var cmdTargetId = 0;  // Device ID for the command modal

    function sendModeCmd(deviceId, deviceName) {
        cmdTargetId = deviceId;
        document.getElementById('cmdDeviceName').textContent = deviceName;
        document.getElementById('commandModal').classList.remove('hidden');
    }

    function closeCommand() {
        document.getElementById('commandModal').classList.add('hidden');
    }

    // Send the selected mode command to the hub's API
    function sendCommand() {
        var mode = document.getElementById('cmdMode').value;
        // Device ID is sent as 4-digit hex (e.g. "0001")
        var body = 'device=' + cmdTargetId.toString(16).padStart(4, '0') +
                   '&mode=' + mode;

        protectedFetch('/api/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              if (d.ok) {
                  fetchCommandFeedback();
                  closeCommand();
              } else {
                  alert('Command failed');
              }
          })
          .catch(function () {
              alert('Failed to send command');
          });
    }

    // ═══════════════════════════════════════════════
    // Find Modal — "Find My Pet" Feature
    //
    // Opens a dialog where the user picks a buzzer pattern
    // (chirp, trill, siren, melody A/B) and LED flash count,
    // then sends it via POST /api/find. The collar will beep
    // and flash its LED so you can locate it.
    // ═══════════════════════════════════════════════
    var findTargetId = 0;      // Device ID for the find modal
    var findDuration = 5;      // Alert duration in minutes (1–60)

    function openFindModal(deviceId, deviceName) {
        findTargetId = deviceId;
        document.getElementById('findDeviceName').textContent = deviceName;
        document.getElementById('findModal').classList.remove('hidden');
        updateFindToggles();
        updateFindDurDisplay();
    }

    function closeFind() {
        document.getElementById('findModal').classList.add('hidden');
    }

    // Show/hide pattern selectors based on toggle state
    function updateFindToggles() {
        var buzzerOn = document.getElementById('findBuzzerEnabled').checked;
        var ledOn = document.getElementById('findLedEnabled').checked;
        document.getElementById('buzzerPatternGroup').style.display = buzzerOn ? '' : 'none';
        document.getElementById('ledPatternGroup').style.display = ledOn ? '' : 'none';
        // Require at least one to be enabled
        document.getElementById('btnSendFind').disabled = !buzzerOn && !ledOn;
    }

    function updateFindDurDisplay() {
        document.getElementById('findDurValue').textContent = findDuration;
    }

    function adjustFindDuration(delta) {
        findDuration = Math.max(1, Math.min(60, findDuration + delta));
        updateFindDurDisplay();
    }

    function sendFind() {
        var buzzerOn = document.getElementById('findBuzzerEnabled').checked;
        var ledOn = document.getElementById('findLedEnabled').checked;
        var pattern = buzzerOn ? document.getElementById('findPattern').value : 'off';
        var flash = ledOn ? document.getElementById('findFlash').value : '0';

        var body = 'device=' + findTargetId.toString(16).padStart(4, '0') +
                   '&pattern=' + pattern +
                   '&flash=' + flash +
                   '&duration=' + findDuration +
                   '&buzzer=' + (buzzerOn ? '1' : '0') +
                   '&led=' + (ledOn ? '1' : '0');

        protectedFetch('/api/find', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              if (d.ok) {
                  fetchCommandFeedback();
                  closeFind();
              } else {
                  alert('Find command failed');
              }
          })
          .catch(function () {
              alert('Failed to send find command');
          });
    }

    // Jump to a device's location and open its popup
    function focusDevice(deviceId) {
        var dev = devices[deviceId];
        if (dev && dev.marker) {
            map.setView(dev.marker.getLatLng(), 17);  // Zoom level 17 = close-up
            dev.marker.openPopup();
        }
    }

    // ═══════════════════════════════════════════════
    // Bootstrap — App Entry Point
    //
    // Called on DOMContentLoaded. Sets up the map, SSE connection,
    // fetches initial device list, and wires up all button handlers.
    // ═══════════════════════════════════════════════
    function init() {
        refreshHubStatus();
        loadTheme();     // Restore dark/light preference from localStorage
        initMap();       // Create Leaflet map with tile layers
        if (typeof HubPresencePanel !== 'undefined') HubPresencePanel.start(map, protectedFetch, function(lat,lon) {
            hubHomeLat=lat; hubHomeLon=lon; // Only the hub's own GNSS is a distance origin.
        });
        connectSSE();    // Open SSE connection for real-time updates

        // On desktop (>768px), show sidebar by default. On mobile, hide it.
        if (window.innerWidth >= 768) {
            document.getElementById('panel').classList.add('open');
            document.body.classList.add('panel-open');
        }

        // Fetch the current device list via REST (in case SSE snapshot was missed)
        fetch('/api/devices')
            .then(function (r) { return r.json(); })
            .then(function (devs) {
                devs.forEach(function (d) {
                    updateDevice(d);
                    loadDeviceHistory(d.id).catch(function () {});
                });
                if (devs.length > 0) fitAllMarkers();  // Zoom to show all devices
            })
            .catch(function () { /* SSE will catch up — ignore fetch errors */ });

        // Wire up all UI button event handlers
        document.getElementById('btnHamburger').addEventListener('click', toggleSidebar);
        document.getElementById('btnTheme').addEventListener('click', toggleTheme);
        document.getElementById('btnSettings').addEventListener('click', openSettings);
        document.getElementById('btnCloseSettings').addEventListener('click', closeSettings);
        document.getElementById('btnSaveConfig').addEventListener('click', saveConfig);
        document.getElementById('btnConsoleLog').addEventListener('click', toggleConsoleLog);
        document.getElementById('btnExportConsoleLog').addEventListener('click', function () {
            exportLogCsv(consoleLogData, 'bluepaws_console_' + new Date().toISOString().slice(0, 10) + '.csv');
        });
        document.getElementById('btnHomeMode').addEventListener('click', function () { setHubMode('home'); });
        document.getElementById('btnPortableMode').addEventListener('click', function () { setHubMode('portable'); });
        document.getElementById('btnOffGridMode').addEventListener('click', function () { setHubMode('off_grid'); });
        document.getElementById('btnSetLocalPin').addEventListener('click', function () { setLocalPin(true); });
        document.getElementById('btnDisableLocalPin').addEventListener('click', function () { setLocalPin(false); });
        document.getElementById('cfgSSID').addEventListener('input', validateConfigForm);
        document.getElementById('cfgPass').addEventListener('input', validateConfigForm);
        document.getElementById('cfgSecondarySSID').addEventListener('input', validateConfigForm);
        document.getElementById('cfgSecondaryPass').addEventListener('input', validateConfigForm);
        document.getElementById('btnSendCmd').addEventListener('click', sendCommand);
        document.getElementById('btnCloseCmd').addEventListener('click', closeCommand);
        document.getElementById('btnSendFind').addEventListener('click', sendFind);
        document.getElementById('btnCloseFind').addEventListener('click', closeFind);
        document.getElementById('findBuzzerEnabled').addEventListener('change', updateFindToggles);
        document.getElementById('findLedEnabled').addEventListener('change', updateFindToggles);
        document.getElementById('findDurUp').addEventListener('click', function () { adjustFindDuration(1); });
        document.getElementById('findDurDown').addEventListener('click', function () { adjustFindDuration(-1); });

        // After sidebar CSS transition completes, tell Leaflet to recalculate map size
        setTimeout(function () { map.invalidateSize(); }, 350);
    }

    // Expose key functions globally so they can be called from HTML onclick or console
    window.BP = {
        sendModeCmd: sendModeCmd,
        openFindModal: openFindModal,
        focusDevice: focusDevice,
        toggleFollow: toggleFollow,
        toggleTrail: toggleTrail
    };

    // Start the app when DOM is ready
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
