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
                btn.innerHTML = '<svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">' +
                    '<path d="M2 2h4V0H0v6h2V2zm12 0h-4V0h6v6h-2V2zM2 14h4v2H0v-6h2v4zm12 0h-4v2h6v-6h-2v4z"/>' +
                    '</svg>';
                L.DomEvent.disableClickPropagation(btn);
                L.DomEvent.on(btn, 'click', function () { fitAllMarkers(); });
                return btn;
            }
        });
        new FitAllControl().addTo(map);

        // Theme toggle (crescent moon / sun)
        var ThemeControl = L.Control.extend({
            options: { position: 'topleft' },
            onAdd: function () {
                var btn = L.DomUtil.create('div', 'leaflet-map-btn');
                btn.id = 'btnTheme';
                btn.title = 'Toggle dark/light theme';
                btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">' +
                    '<path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.92-.1-1.36a5.389 5.389 0 0 1-4.4 2.26 5.403 5.403 0 0 1-3.14-9.8c-.44-.06-.9-.1-1.36-.1z"/>' +
                    '</svg>';
                L.DomEvent.disableClickPropagation(btn);
                L.DomEvent.on(btn, 'click', function () { toggleTheme(); });
                return btn;
            }
        });
        new ThemeControl().addTo(map);

        // Measure tool (ruler icon)
        var MeasureControl = L.Control.extend({
            options: { position: 'topleft' },
            onAdd: function () {
                var btn = L.DomUtil.create('div', 'leaflet-map-btn');
                btn.id = 'btnMeasure';
                btn.title = 'Measure distance (click points on map)';
                btn.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
                    '<path d="M2 22L22 2"/>' +
                    '<path d="M6 18l2-2"/><path d="M10 14l2-2"/><path d="M14 10l2-2"/><path d="M18 6l2-2"/>' +
                    '</svg>';
                L.DomEvent.disableClickPropagation(btn);
                L.DomEvent.on(btn, 'click', function () { toggleMeasure(); });
                return btn;
            }
        });
        new MeasureControl().addTo(map);

        // Click handler for measurement mode
        map.on('click', onMapClick);
    }

    // ═══════════════════════════════════════════════
    // Theme Toggle
    // ═══════════════════════════════════════════════
    function toggleTheme() {
        darkMode = !darkMode;
        document.body.classList.toggle('light', !darkMode);

        var btn = document.getElementById('btnTheme');
        if (btn) {
            if (darkMode) {
                btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">' +
                    '<path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.92-.1-1.36a5.389 5.389 0 0 1-4.4 2.26 5.403 5.403 0 0 1-3.14-9.8c-.44-.06-.9-.1-1.36-.1z"/>' +
                    '</svg>';
            } else {
                btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">' +
                    '<circle cx="12" cy="12" r="5"/>' +
                    '<path d="M12 1v2m0 18v2M4.22 4.22l1.42 1.42m12.72 12.72l1.42 1.42M1 12h2m18 0h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/>' +
                    '</svg>';
            }
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
                updateDevice(data);  // Update map marker + device card
            } catch (err) {
                console.error('SSE parse error:', err);
            }
        });

        // "heartbeat" events are empty — just prove the connection is alive
        evtSource.addEventListener('heartbeat', function () {
            resetHeartbeatWatchdog();
        });

        evtSource.onopen = function () {
            resetHeartbeatWatchdog();
        };

        evtSource.onerror = function () {
            clearTimeout(heartbeatTimer);
            setStatus('disconnected', 'Disconnected');
            // EventSource will auto-reconnect after a short delay
        };
    }

    // Update the connection status pill (top-right corner)
    function setStatus(state, text) {
        var banner = document.getElementById('statusBanner');
        var textEl = document.getElementById('statusText');
        banner.className = state;    // 'connected' or 'disconnected' — CSS styles the color
        textEl.textContent = text;
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
                dev.marker.bindPopup('');  // Popup gets content below

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

            // Apply status-based marker styles (green border for home, red pulse for lost)
            var markerEl = document.getElementById('marker-' + id);
            if (markerEl) {
                markerEl.className = 'bp-marker';
                markerEl.style.borderColor = dev.avatar.color;
                if (data.status === 'Home') markerEl.classList.add('status-home');
                if (data.status === 'LostTimeout') markerEl.classList.add('status-lost');
            }

            // ── Trail breadcrumb line ──
            // Each GPS update adds a point. Max 100 points (oldest dropped).
            // Renders as a dashed polyline in the device's trail color.
            if (dev.showTrail) {
                dev.trail.push(latlng);
                if (dev.trail.length > 100) dev.trail.shift();  // Keep trail manageable
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
        return '<div style="font-size:13px;line-height:1.6">' +
            '<span style="font-size:20px">' + dev.avatar.emoji + '</span> ' +
            '<strong>' + data.name + '</strong><br>' +
            'Status: ' + data.status + '<br>' +
            'Profile: ' + data.profile + '<br>' +
            'Battery: ' + data.batt + ' mV<br>' +
            'Signal: ' + getSignalQuality(data.rssi, data.snr).label + '<br>' +
            (data.hasGps ? 'Lat: ' + data.lat.toFixed(6) + '  Lon: ' + data.lon.toFixed(6) + '<br>' : '') +
            'Accuracy: ' + data.acc + ' m' +
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
                '<span class="card-coords-row">' +
                    '<a href="' + gmapsUrl + '" target="_blank" rel="noopener" class="card-coords card-coords-link" title="Open in Google Maps">' + coordStr + '</a>' +
                    '<button class="btn-share" data-url="' + gmapsUrl + '" data-name="' + data.name + '" title="Share location">' +
                        '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M13.5 1a2.5 2.5 0 00-2.4 3.2L5.7 7.4a2.5 2.5 0 100 1.2l5.4 3.2a2.5 2.5 0 101-1.7L6.7 6.9a2.5 2.5 0 000-1.8l5.4-3.2A2.5 2.5 0 1013.5 1z"/></svg>' +
                    '</button>' +
                '</span>';
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
            var battPct = Math.min(100, Math.max(0, Math.round((data.batt - 3000) / 12)));

            html +=
                '<div class="card-detail">' +
                    '<div class="card-grid">' +
                        '<span class="label">Profile</span><span class="value">' + data.profile + '</span>' +
                        '<span class="label">Battery</span><span class="value">' + battPct + '% (' + data.batt + ' mV)</span>' +
                        '<span class="label">GPS Acc</span><span class="value">' + data.acc + ' m</span>' +
                        '<span class="label">Fix Age</span><span class="value">' + data.fixAge + ' s</span>' +
                        '<span class="label">Last seen</span><span class="value">' + formatAge(age) + '</span>' +
                    '</div>' +

                    '<div class="card-actions">' +
                        '<button class="btn-action btn-jump" data-action="jump" title="Jump to location">' +
                            '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0l3 6H5l3-6zm0 16l-3-6h6l-3 6z"/></svg>' +
                            ' Jump' +
                        '</button>' +
                        '<button class="btn-action btn-follow' + (isFollowed ? ' active' : '') + '" data-action="follow" title="Auto-follow on map">' +
                            '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M8 0a5.5 5.5 0 00-5.5 5.5C2.5 10 8 16 8 16s5.5-6 5.5-10.5A5.5 5.5 0 008 0zm0 8a2.5 2.5 0 110-5 2.5 2.5 0 010 5z"/></svg>' +
                            (isFollowed ? ' Following' : ' Follow') +
                        '</button>' +
                        '<button class="btn-action btn-trail' + (dev.showTrail ? ' active' : '') + '" data-action="trail" title="Toggle breadcrumb trail">' +
                            '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M2 14l4-4 3 3 5-7v2l-5 7-3-3-4 4v-2z"/></svg>' +
                            ' Trail' +
                        '</button>' +
                        '<button class="btn-action btn-find" data-action="find" title="Find collar (buzzer + LED)">' +
                            '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M11.7 1.3a1 1 0 011.4 1.4L4.4 11.4a1 1 0 01-1.4-1.4l8.7-8.7zM3 13a2 2 0 100-4 2 2 0 000 4z"/></svg>' +
                            ' Find' +
                        '</button>' +
                        '<button class="btn-action btn-cmd" data-action="cmd" title="Command & Control">' +
                            '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M6 1h4v2h3v4h-2V5H5v2H3V3h3V1zm4 14H6v-2H3v-4h2v2h6v-2h2v4h-3v2z"/></svg>' +
                            ' Cmd' +
                        '</button>' +
                    '</div>' +
                '</div>';
        }

        card.innerHTML = html;

        // Wire up action buttons (only present when expanded)
        if (isExpanded) {
            var buttons = card.querySelectorAll('.btn-action');
            buttons.forEach(function (btn) {
                btn.addEventListener('click', function (e) {
                    e.stopPropagation();
                    var action = btn.getAttribute('data-action');
                    if (action === 'jump') focusDevice(dev.id);
                    if (action === 'follow') toggleFollow(dev.id);
                    if (action === 'trail') toggleTrail(dev.id);
                    if (action === 'find') openFindModal(dev.id, data.name);
                    if (action === 'cmd') sendModeCmd(dev.id, data.name);
                });
            });
        }

        // Sheen animation on update
        if (!isNew) {
            card.classList.remove('sheen');
            void card.offsetWidth;
            card.classList.add('sheen');
        }

        // Wire up share button (uses Web Share API or clipboard fallback)
        var shareBtn = card.querySelector('.btn-share');
        if (shareBtn) {
            shareBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                var url = shareBtn.getAttribute('data-url');
                var name = shareBtn.getAttribute('data-name');
                var text = name + ' is here: ' + url;

                if (navigator.share) {
                    navigator.share({ title: name + ' Location', text: text, url: url }).catch(function () {});
                } else if (navigator.clipboard) {
                    navigator.clipboard.writeText(text).then(function () {
                        shareBtn.classList.add('shared');
                        setTimeout(function () { shareBtn.classList.remove('shared'); }, 1500);
                    });
                }
            });
        }

        // Click card summary to toggle expand/collapse
        var summary = card.querySelector('.card-summary');
        if (summary) {
            summary.addEventListener('click', function (e) {
                if (e.target.closest('.btn-action') || e.target.closest('.btn-share') || e.target.closest('.card-coords-link')) return;
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
    function openSettings() {
        document.getElementById('settingsModal').classList.remove('hidden');

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
        document.getElementById('btnSettings').addEventListener('click', openSettings);
        document.getElementById('btnCloseSettings').addEventListener('click', closeSettings);
        document.getElementById('btnSaveConfig').addEventListener('click', saveConfig);
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
