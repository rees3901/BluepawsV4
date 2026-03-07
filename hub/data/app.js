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
    var consoleLog = [];           // Ring buffer of log entries (max 200)
    var MAX_LOG_ENTRIES = 200;

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
        var bars = '';
        for (var i = 1; i <= 5; i++) {
            var filled = i <= batt.level;
            var height = 4 + (i * 3);
            bars += '<span class="sig-bar' + (filled ? ' filled' : '') + '" style="' +
                'height:' + height + 'px;' +
                (filled ? 'background:' + batt.color + ';' : '') +
                '"></span>';
        }
        return '<span class="signal-indicator" title="' + (millivolts / 1000).toFixed(2) + ' V — ' + batt.label + '">' +
            bars +
            '<span class="sig-label" style="color:' + batt.color + '">' + batt.label + '</span>' +
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

    function loadHubHome() {
        try {
            var lat = localStorage.getItem('bp_hub_lat');
            var lon = localStorage.getItem('bp_hub_lon');
            if (lat && lon) {
                hubHomeLat = parseFloat(lat);
                hubHomeLon = parseFloat(lon);
            }
        } catch (e) {}
    }

    function setHubHome(lat, lon) {
        hubHomeLat = lat;
        hubHomeLon = lon;
        try {
            localStorage.setItem('bp_hub_lat', lat.toString());
            localStorage.setItem('bp_hub_lon', lon.toString());
        } catch (e) {}
    }

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
    function logEvent(type, msg) {
        var ts = new Date().toLocaleTimeString();
        consoleLog.push('[' + ts + '] ' + type + ': ' + msg);
        if (consoleLog.length > MAX_LOG_ENTRIES) consoleLog.shift();
        updateConsoleDisplay();
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
            center: [51.505, -0.09],  // Default center (overridden on first device)
            zoom: 13,
            zoomControl: false        // We add our own zoom control below
        });

        // Base tile layers — user can switch between them via the layer control.
        var street = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenStreetMap',
            maxZoom: 19
        });

        var satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
            attribution: '&copy; Esri World Imagery',
            maxZoom: 19
        });

        var esriClarity = L.tileLayer('https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
            attribution: '&copy; Esri Clarity',
            maxZoom: 19
        });

        var topo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenTopoMap',
            maxZoom: 17
        });

        var humanitarian = L.tileLayer('https://{s}.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenStreetMap, Tiles: HOT',
            maxZoom: 19
        });

        var esriTopo = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/{z}/{y}/{x}', {
            attribution: '&copy; Esri',
            maxZoom: 19
        });

        street.addTo(map);  // Street map is the default

        // Layer switcher control (top-right corner)
        L.control.layers({
            'Street': street,
            'Satellite': satellite,
            'Satellite HD': esriClarity,
            'Topographic': topo,
            'Humanitarian': humanitarian,
            'Esri Topo': esriTopo
        }, null, { position: 'topright', collapsed: true }).addTo(map);

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

        // "telemetry" events carry device data as JSON
        evtSource.addEventListener('telemetry', function (e) {
            resetHeartbeatWatchdog();
            try {
                var data = JSON.parse(e.data);
                logEvent('RX', data.name + ' id=' + data.id + ' lat=' + (data.lat||0).toFixed(5) + ' lon=' + (data.lon||0).toFixed(5) + ' rssi=' + data.rssi + ' batt=' + data.batt + 'mV');
                updateDevice(data);
            } catch (err) {
                logEvent('ERR', 'SSE parse: ' + err.message);
                console.error('SSE parse error:', err);
            }
        });

        // "heartbeat" events are empty — just prove the connection is alive
        evtSource.addEventListener('heartbeat', function () {
            resetHeartbeatWatchdog();
        });

        evtSource.onopen = function () {
            logEvent('SYS', 'SSE connected');
            resetHeartbeatWatchdog();
        };

        evtSource.onerror = function () {
            logEvent('SYS', 'SSE disconnected');
            clearTimeout(heartbeatTimer);
            setStatus('disconnected', 'Disconnected');
        };
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

        dev.data = data;               // Store latest telemetry payload
        dev.lastUpdate = Date.now();    // Timestamp for "last seen" calculation

        // Only update map if we have valid GPS coordinates
        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            var latlng = [data.lat, data.lon];

            // Set hub home position from first GPS fix (hub is near the collar at startup)
            if (hubHomeLat === null) {
                setHubHome(data.lat, data.lon);
            }

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
                if (data.status === 'LostTimeout') markerEl.classList.add('status-lost');
            }

            // ── Trail breadcrumb line ──
            // Each GPS update adds a point. Max 4 points to keep it clean.
            // Renders as a dashed polyline in the device's trail color.
            if (dev.showTrail) {
                dev.trail.push(latlng);
                while (dev.trail.length > 4) dev.trail.shift();
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

    function buildPopup(dev) {
        var data = dev.data;
        var isFollowed = (followedDeviceId === dev.id);
        var distStr = (data.hasGps && data.lat !== 0 && data.lon !== 0)
            ? formatDistFromHub(data.lat, data.lon) : '--';
        var statusClass = 'status-' + data.status.toLowerCase().replace('timeout', '');
        return '<div class="popup-content">' +
            '<div class="popup-header">' +
                '<span style="font-size:20px">' + dev.avatar.emoji + '</span> ' +
                '<strong>' + data.name + '</strong>' +
                '<span class="card-status ' + statusClass + '" style="margin-left:6px;font-size:10px">' + data.status + '</span>' +
            '</div>' +
            '<div class="popup-grid">' +
                '<span class="label">Signal</span><span class="value">' + renderSignalBars(data.rssi, data.snr) + '</span>' +
                '<span class="label">Battery</span><span class="value">' + renderBatteryBars(data.batt) + '</span>' +
                '<span class="label">GPS Acc</span><span class="value">' + data.acc + ' m</span>' +
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
        var stale = age > 600;  // 10 minutes
        var isExpanded = (expandedCardId === dev.id);
        card.className = 'device-card' + (stale ? ' stale' : '') + (isExpanded ? ' expanded' : '');

        var statusClass = 'status-' + data.status.toLowerCase().replace('timeout', '');
        var isFollowed = (followedDeviceId === dev.id);

        // Coordinate display — hyperlinked to Google Maps with share button
        var coordStr = '---, ---';
        var coordHtml = '<span class="card-coords">---, ---</span>';
        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            coordStr = data.lat.toFixed(5) + ', ' + data.lon.toFixed(5);
            var gmapsUrl = 'https://www.google.com/maps?q=' + data.lat.toFixed(6) + ',' + data.lon.toFixed(6);
            coordHtml =
                '<a href="' + gmapsUrl + '" target="_blank" rel="noopener" class="card-coords card-coords-link" title="Open in Google Maps">' + coordStr + '</a>';
        }

        // ── Compact summary (always visible) ──
        var html =
            '<div class="card-summary">' +
                '<div class="card-avatar" style="border-color:' + dev.avatar.color + '">' + dev.avatar.emoji + '</div>' +
                '<div class="card-identity">' +
                    '<span class="card-name">' + data.name + '</span>' +
                    coordHtml +
                '</div>' +
                renderSignalBars(data.rssi, data.snr) +
                '<span class="card-status ' + statusClass + '">' + data.status + '</span>' +
                '<span class="card-chevron">' + (isExpanded ? '&#9650;' : '&#9660;') + '</span>' +
            '</div>';

        // ── Expanded detail (shown only when card is expanded) ──
        if (isExpanded) {
            var distStr = (data.hasGps && data.lat !== 0 && data.lon !== 0)
                ? formatDistFromHub(data.lat, data.lon) : '--';

            html +=
                '<div class="card-detail">' +
                    '<div class="card-grid">' +
                        '<span class="label">Power Profile</span><span class="value">' + data.profile + '</span>' +
                        '<span class="label">Battery</span><span class="value">' + renderBatteryBars(data.batt) + '</span>' +
                        '<span class="label">GPS Acc</span><span class="value">' + data.acc + ' m</span>' +
                        '<span class="label">Dist From Hub</span><span class="value">' + distStr + '</span>' +
                        '<span class="label">Last seen</span><span class="value">' + formatAge(age) + '</span>' +
                    '</div>' +

                    '<div class="card-actions">' +
                        buildActionButtons(dev, isFollowed) +
                    '</div>' +
                '</div>';
        }

        card.innerHTML = html;

        // Wire up action buttons (only present when expanded)
        if (isExpanded) {
            wireActionButtons(card);
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

    // Human-friendly time display (e.g. "just now", "5s ago", "3m ago", "2h ago")
    function formatAge(seconds) {
        if (seconds < 5) return 'just now';
        if (seconds < 60) return seconds + 's ago';
        if (seconds < 3600) return Math.floor(seconds / 60) + 'm ago';
        return Math.floor(seconds / 3600) + 'h ago';
    }

    // Re-render all cards every 5 seconds to update "last seen" ages
    // (the actual telemetry data only changes on SSE events, but the
    //  age counter needs to tick every few seconds)
    setInterval(function () {
        for (var id in devices) {
            renderDeviceCard(devices[id]);
        }
    }, 5000);

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
        var ssidValid = ssid.length >= 1 && ssid.length <= 32 && /^[\x20-\x7E]+$/.test(ssid);
        var passValid = pass.length === 0 || (pass.length >= 8 && pass.length <= 63);
        btn.disabled = !(ssidValid && passValid);
    }

    function openSettings() {
        document.getElementById('settingsModal').classList.remove('hidden');
        validateConfigForm();  // Set initial button state

        // Fetch hub status to display diagnostics in the modal
        fetch('/api/status')
            .then(function (r) { return r.json(); })
            .then(function (s) {
                document.getElementById('hubStatus').innerHTML =
                    'Uptime: ' + formatAge(s.uptime) + '<br>' +
                    'Packets RX: ' + s.rxCount + '<br>' +
                    'Commands TX: ' + s.txCount + '<br>' +
                    'CRC Fails: ' + s.crcFails + '<br>' +
                    'Devices: ' + s.devices + '<br>' +
                    'Log entries: ' + s.logEntries + '<br>' +
                    'Free heap: ' + (s.freeHeap / 1024).toFixed(1) + ' KB<br>' +
                    'WiFi STA: ' + (s.staConnected ? s.staIP : 'Not connected') + '<br>' +
                    'AP IP: ' + s.apIP;
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
                   '&cloud_url=' + encodeURIComponent(cloud);

        fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function () {
            alert('Configuration saved. Hub will restart.');
        }).catch(function () {
            alert('Failed to save configuration.');
        });
    }

    // ═══════════════════════════════════════════════
    // Command Modal — Change Collar Operating Mode
    //
    // Opens a dialog where the user selects a mode (Normal, PowerSave,
    // Active Find, Emergency Lost) and sends it to the collar via
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

        fetch('/api/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              if (d.ok) {
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
    var findTargetId = 0;  // Device ID for the find modal

    function openFindModal(deviceId, deviceName) {
        findTargetId = deviceId;
        document.getElementById('findDeviceName').textContent = deviceName;
        document.getElementById('findModal').classList.remove('hidden');
    }

    function closeFind() {
        document.getElementById('findModal').classList.add('hidden');
    }

    function sendFind() {
        var pattern = document.getElementById('findPattern').value;
        var flash = document.getElementById('findFlash').value;
        var body = 'device=' + findTargetId.toString(16).padStart(4, '0') +
                   '&pattern=' + pattern +
                   '&flash=' + flash;

        fetch('/api/find', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              if (d.ok) {
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
        loadTheme();     // Restore dark/light preference from localStorage
        loadHubHome();   // Restore hub home position from localStorage
        initMap();       // Create Leaflet map with tile layers
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
                devs.forEach(function (d) { updateDevice(d); });
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
        document.getElementById('cfgSSID').addEventListener('input', validateConfigForm);
        document.getElementById('cfgPass').addEventListener('input', validateConfigForm);
        document.getElementById('btnSendCmd').addEventListener('click', sendCommand);
        document.getElementById('btnCloseCmd').addEventListener('click', closeCommand);
        document.getElementById('btnSendFind').addEventListener('click', sendFind);
        document.getElementById('btnCloseFind').addEventListener('click', closeFind);

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
