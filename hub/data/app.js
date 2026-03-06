/*
  Bluepaws V4 — Hub Web GUI
  Leaflet.js map with SSE real-time updates
  Left sidebar, dark/light theme, enhanced device cards with avatar, follow, trail controls
*/

(function () {
    'use strict';

    // ═══════════════════════════════════════════════
    // State
    // ═══════════════════════════════════════════════
    const devices = {};
    let map = null;
    let evtSource = null;
    let measuring = false;
    let measurePoints = [];
    let measureLine = null;
    let measureLabels = [];
    let measureMarkers = [];
    let darkMode = true;
    let followedDeviceId = null;

    // Emoji + color palette for distinguishing animals
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

    let avatarIndex = 0;

    // ═══════════════════════════════════════════════
    // Map Initialisation
    // ═══════════════════════════════════════════════
    function initMap() {
        map = L.map('map', {
            center: [51.505, -0.09],
            zoom: 13,
            zoomControl: false
        });

        var street = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenStreetMap',
            maxZoom: 19
        });

        var satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
            attribution: '&copy; Esri',
            maxZoom: 19
        });

        var topo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenTopoMap',
            maxZoom: 17
        });

        street.addTo(map);

        L.control.layers({
            'Street': street,
            'Satellite': satellite,
            'Topographic': topo
        }, null, { position: 'topright' }).addTo(map);

        // Zoom control (top-left, below hamburger)
        L.control.zoom({ position: 'topleft' }).addTo(map);

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
    // SSE Connection with heartbeat watchdog
    // ═══════════════════════════════════════════════
    var heartbeatTimer = null;
    var HEARTBEAT_TIMEOUT_MS = 10000;  // mark disconnected if no event in 10s

    function resetHeartbeatWatchdog() {
        clearTimeout(heartbeatTimer);
        setStatus('connected', 'Connected');
        heartbeatTimer = setTimeout(function () {
            setStatus('disconnected', 'No heartbeat');
        }, HEARTBEAT_TIMEOUT_MS);
    }

    function connectSSE() {
        if (evtSource) evtSource.close();

        evtSource = new EventSource('/events');

        evtSource.addEventListener('telemetry', function (e) {
            resetHeartbeatWatchdog();
            try {
                var data = JSON.parse(e.data);
                updateDevice(data);
            } catch (err) {
                console.error('SSE parse error:', err);
            }
        });

        evtSource.addEventListener('heartbeat', function () {
            resetHeartbeatWatchdog();
        });

        evtSource.onopen = function () {
            resetHeartbeatWatchdog();
        };

        evtSource.onerror = function () {
            clearTimeout(heartbeatTimer);
            setStatus('disconnected', 'Disconnected');
        };
    }

    function setStatus(state, text) {
        var banner = document.getElementById('statusBanner');
        var textEl = document.getElementById('statusText');
        banner.className = state;
        textEl.textContent = text;
    }

    // ═══════════════════════════════════════════════
    // Device Updates
    // ═══════════════════════════════════════════════
    function updateDevice(data) {
        var id = data.id;
        var dev = devices[id];

        if (!dev) {
            var av = AVATARS[avatarIndex % AVATARS.length];
            var tc = TRAIL_COLORS[avatarIndex % TRAIL_COLORS.length];
            dev = {
                id: id,
                name: data.name,
                marker: null,
                trail: [],
                trailLine: null,
                showTrail: true,
                avatar: av,
                trailColor: tc
            };
            avatarIndex++;
            devices[id] = dev;
        }

        dev.data = data;
        dev.lastUpdate = Date.now();

        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            var latlng = [data.lat, data.lon];

            if (!dev.marker) {
                var icon = L.divIcon({
                    className: '',
                    html: '<div class="bp-marker" id="marker-' + id + '" style="border-color:' + dev.avatar.color + '">' + dev.avatar.emoji + '</div>',
                    iconSize: [32, 32],
                    iconAnchor: [16, 16]
                });
                dev.marker = L.marker(latlng, { icon: icon }).addTo(map);
                dev.marker.bindPopup('');

                if (Object.keys(devices).length === 1) {
                    map.setView(latlng, 16);
                }
            } else {
                dev.marker.setLatLng(latlng);

                var el = document.getElementById('marker-' + id);
                if (el) {
                    el.classList.remove('updated');
                    void el.offsetWidth;
                    el.classList.add('updated');
                }
            }

            dev.marker.setPopupContent(buildPopup(dev));

            var markerEl = document.getElementById('marker-' + id);
            if (markerEl) {
                markerEl.className = 'bp-marker';
                markerEl.style.borderColor = dev.avatar.color;
                if (data.status === 'Home') markerEl.classList.add('status-home');
                if (data.status === 'LostTimeout') markerEl.classList.add('status-lost');
            }

            // Trail line
            if (dev.showTrail) {
                dev.trail.push(latlng);
                if (dev.trail.length > 100) dev.trail.shift();
                if (dev.trailLine) {
                    dev.trailLine.setLatLngs(dev.trail);
                } else {
                    dev.trailLine = L.polyline(dev.trail, {
                        color: dev.trailColor,
                        weight: 2,
                        opacity: 0.6,
                        dashArray: '4 4'
                    }).addTo(map);
                }
            }

            // Follow mode: auto-center
            if (followedDeviceId === id) {
                map.setView(latlng);
            }
        }

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
            'RSSI: ' + data.rssi + ' dBm / SNR: ' + data.snr + ' dB<br>' +
            (data.hasGps ? 'Lat: ' + data.lat.toFixed(6) + '  Lon: ' + data.lon.toFixed(6) + '<br>' : '') +
            'Accuracy: ' + data.acc + ' m' +
            '</div>';
    }

    // ═══════════════════════════════════════════════
    // Device Cards
    // ═══════════════════════════════════════════════
    function renderDeviceCard(dev) {
        var data = dev.data;
        var container = document.getElementById('deviceCards');
        var card = document.getElementById('card-' + dev.id);
        var isNew = false;

        if (!card) {
            card = document.createElement('div');
            card.id = 'card-' + dev.id;
            card.className = 'device-card';
            container.appendChild(card);
            isNew = true;
        }

        var age = Math.floor((Date.now() - dev.lastUpdate) / 1000);
        var stale = age > 600;
        card.className = 'device-card' + (stale ? ' stale' : '');

        var statusClass = 'status-' + data.status.toLowerCase().replace('timeout', '');
        var battPct = Math.min(100, Math.max(0, Math.round((data.batt - 3000) / 12)));
        var isFollowed = (followedDeviceId === dev.id);

        // Coordinate display
        var coordStr = '---, ---';
        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            coordStr = data.lat.toFixed(5) + ', ' + data.lon.toFixed(5);
        }

        card.innerHTML =
            // Header: avatar + name + coords + status
            '<div class="card-header">' +
                '<div class="card-avatar" style="border-color:' + dev.avatar.color + '">' + dev.avatar.emoji + '</div>' +
                '<div class="card-identity">' +
                    '<span class="card-name">' + data.name + '</span>' +
                    '<span class="card-coords">' + coordStr + '</span>' +
                '</div>' +
                '<span class="card-status ' + statusClass + '">' + data.status + '</span>' +
            '</div>' +

            // Telemetry grid
            '<div class="card-grid">' +
                '<span class="label">Profile</span><span class="value">' + data.profile + '</span>' +
                '<span class="label">Battery</span><span class="value">' + battPct + '% (' + data.batt + ' mV)</span>' +
                '<span class="label">Signal</span><span class="value">' + data.rssi + ' dBm / ' + data.snr + ' dB</span>' +
                '<span class="label">GPS Acc</span><span class="value">' + data.acc + ' m</span>' +
                '<span class="label">Fix Age</span><span class="value">' + data.fixAge + ' s</span>' +
                '<span class="label">Last seen</span><span class="value">' + formatAge(age) + '</span>' +
            '</div>' +

            // Action buttons
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
            '</div>';

        // Wire up action buttons
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

        // Sheen animation on update
        if (!isNew) {
            card.classList.remove('sheen');
            void card.offsetWidth;
            card.classList.add('sheen');
        }

        // Click card body to jump
        card.onclick = function (e) {
            if (e.target.closest('.btn-action')) return;
            focusDevice(dev.id);
        };
    }

    function formatAge(seconds) {
        if (seconds < 5) return 'just now';
        if (seconds < 60) return seconds + 's ago';
        if (seconds < 3600) return Math.floor(seconds / 60) + 'm ago';
        return Math.floor(seconds / 3600) + 'h ago';
    }

    // Periodically refresh card ages
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
    // Measurement Tool (on map)
    // ═══════════════════════════════════════════════
    function toggleMeasure() {
        measuring = !measuring;
        var btn = document.getElementById('btnMeasure');
        if (btn) btn.classList.toggle('active', measuring);

        if (!measuring) {
            clearMeasure();
        }

        map.getContainer().style.cursor = measuring ? 'crosshair' : '';
    }

    function onMapClick(e) {
        if (!measuring) return;

        measurePoints.push(e.latlng);

        var cm = L.circleMarker(e.latlng, {
            radius: 4,
            color: '#1d9bf0',
            fillOpacity: 1
        }).addTo(map);
        measureMarkers.push(cm);

        if (measurePoints.length > 1) {
            var total = totalMeasureDistance();

            if (measureLine) {
                measureLine.addLatLng(e.latlng);
            } else {
                measureLine = L.polyline(measurePoints, {
                    color: '#1d9bf0',
                    weight: 2,
                    dashArray: '6 4'
                }).addTo(map);
            }

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

    function totalMeasureDistance() {
        var total = 0;
        for (var i = 1; i < measurePoints.length; i++) {
            total += measurePoints[i - 1].distanceTo(measurePoints[i]);
        }
        return total;
    }

    function formatDistance(meters) {
        if (meters < 1000) return Math.round(meters) + ' m';
        return (meters / 1000).toFixed(2) + ' km';
    }

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
    // ═══════════════════════════════════════════════
    function openSettings() {
        document.getElementById('settingsModal').classList.remove('hidden');

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
    // Command Modal
    // ═══════════════════════════════════════════════
    var cmdTargetId = 0;

    function sendModeCmd(deviceId, deviceName) {
        cmdTargetId = deviceId;
        document.getElementById('cmdDeviceName').textContent = deviceName;
        document.getElementById('commandModal').classList.remove('hidden');
    }

    function closeCommand() {
        document.getElementById('commandModal').classList.add('hidden');
    }

    function sendCommand() {
        var mode = document.getElementById('cmdMode').value;
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
    // Find Modal
    // ═══════════════════════════════════════════════
    var findTargetId = 0;

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

    function focusDevice(deviceId) {
        var dev = devices[deviceId];
        if (dev && dev.marker) {
            map.setView(dev.marker.getLatLng(), 17);
            dev.marker.openPopup();
        }
    }

    // ═══════════════════════════════════════════════
    // Bootstrap
    // ═══════════════════════════════════════════════
    function init() {
        loadTheme();
        initMap();
        connectSSE();

        // Open sidebar by default on desktop, closed on mobile
        if (window.innerWidth >= 768) {
            document.getElementById('panel').classList.add('open');
            document.body.classList.add('panel-open');
        }

        // Fetch initial device list
        fetch('/api/devices')
            .then(function (r) { return r.json(); })
            .then(function (devs) {
                devs.forEach(function (d) { updateDevice(d); });
                if (devs.length > 0) fitAllMarkers();
            })
            .catch(function () { /* SSE will catch up */ });

        // Button handlers
        document.getElementById('btnHamburger').addEventListener('click', toggleSidebar);
        document.getElementById('btnFitAll').addEventListener('click', fitAllMarkers);
        document.getElementById('btnSettings').addEventListener('click', openSettings);
        document.getElementById('btnCloseSettings').addEventListener('click', closeSettings);
        document.getElementById('btnSaveConfig').addEventListener('click', saveConfig);
        document.getElementById('btnSendCmd').addEventListener('click', sendCommand);
        document.getElementById('btnCloseCmd').addEventListener('click', closeCommand);
        document.getElementById('btnSendFind').addEventListener('click', sendFind);
        document.getElementById('btnCloseFind').addEventListener('click', closeFind);

        // Tell map about initial layout
        setTimeout(function () { map.invalidateSize(); }, 350);
    }

    // Public API
    window.BP = {
        sendModeCmd: sendModeCmd,
        openFindModal: openFindModal,
        focusDevice: focusDevice,
        toggleFollow: toggleFollow,
        toggleTrail: toggleTrail
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
