/*
  Bluepaws V4 — Hub Web GUI
  Leaflet.js map with SSE real-time updates
*/

(function () {
    'use strict';

    // ═══════════════════════════════════════════════
    // State
    // ═══════════════════════════════════════════════
    const devices = {};   // keyed by device id
    let map = null;
    let evtSource = null;
    let measuring = false;
    let measurePoints = [];
    let measureLine = null;
    let measureLabels = [];

    // ═══════════════════════════════════════════════
    // Map Initialisation
    // ═══════════════════════════════════════════════
    function initMap() {
        map = L.map('map', {
            center: [51.505, -0.09],
            zoom: 13,
            zoomControl: false
        });

        // Layer options
        const street = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenStreetMap',
            maxZoom: 19
        });

        const satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
            attribution: '&copy; Esri',
            maxZoom: 19
        });

        const topo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; OpenTopoMap',
            maxZoom: 17
        });

        street.addTo(map);

        // Layer control (top-right)
        L.control.layers({
            'Street': street,
            'Satellite': satellite,
            'Topographic': topo
        }, null, { position: 'topright' }).addTo(map);

        // Zoom control (top-left)
        L.control.zoom({ position: 'topleft' }).addTo(map);

        // Click handler for measurement mode
        map.on('click', onMapClick);
    }

    // ═══════════════════════════════════════════════
    // SSE Connection
    // ═══════════════════════════════════════════════
    function connectSSE() {
        if (evtSource) evtSource.close();

        evtSource = new EventSource('/events');

        evtSource.addEventListener('telemetry', function (e) {
            try {
                const data = JSON.parse(e.data);
                updateDevice(data);
            } catch (err) {
                console.error('SSE parse error:', err);
            }
        });

        evtSource.onopen = function () {
            setStatus('connected', 'Connected');
        };

        evtSource.onerror = function () {
            setStatus('disconnected', 'Disconnected');
            // Auto-reconnect is built into EventSource
        };
    }

    function setStatus(state, text) {
        const banner = document.getElementById('statusBanner');
        const textEl = document.getElementById('statusText');
        banner.className = state;
        textEl.textContent = text;
    }

    // ═══════════════════════════════════════════════
    // Device Updates
    // ═══════════════════════════════════════════════
    function updateDevice(data) {
        const id = data.id;
        let dev = devices[id];

        if (!dev) {
            dev = {
                id: id,
                name: data.name,
                marker: null,
                trail: [],
                trailLine: null
            };
            devices[id] = dev;
        }

        // Update stored data
        dev.data = data;
        dev.lastUpdate = Date.now();

        // Update map marker
        if (data.hasGps && data.lat !== 0 && data.lon !== 0) {
            const latlng = [data.lat, data.lon];

            if (!dev.marker) {
                const icon = L.divIcon({
                    className: '',
                    html: '<div class="bp-marker" id="marker-' + id + '">&#128062;</div>',
                    iconSize: [28, 28],
                    iconAnchor: [14, 14]
                });
                dev.marker = L.marker(latlng, { icon: icon }).addTo(map);
                dev.marker.bindPopup('');

                // Zoom to first device
                if (Object.keys(devices).length === 1) {
                    map.setView(latlng, 16);
                }
            } else {
                dev.marker.setLatLng(latlng);

                // Pop animation
                const el = document.getElementById('marker-' + id);
                if (el) {
                    el.classList.remove('updated');
                    void el.offsetWidth;  // force reflow
                    el.classList.add('updated');
                }
            }

            // Update popup content
            dev.marker.setPopupContent(buildPopup(data));

            // Update marker style based on status
            const el = document.getElementById('marker-' + id);
            if (el) {
                el.className = 'bp-marker';
                if (data.status === 'Home') el.classList.add('status-home');
                if (data.status === 'LostTimeout') el.classList.add('status-lost');
            }

            // Trail line
            dev.trail.push(latlng);
            if (dev.trail.length > 100) dev.trail.shift();
            if (dev.trailLine) {
                dev.trailLine.setLatLngs(dev.trail);
            } else {
                dev.trailLine = L.polyline(dev.trail, {
                    color: '#1d9bf0',
                    weight: 2,
                    opacity: 0.5,
                    dashArray: '4 4'
                }).addTo(map);
            }
        }

        // Update device card in panel
        renderDeviceCard(dev);
    }

    function buildPopup(data) {
        return '<div style="font-size:13px;line-height:1.6">' +
            '<strong>' + data.name + '</strong><br>' +
            'Status: ' + data.status + '<br>' +
            'Profile: ' + data.profile + '<br>' +
            'Battery: ' + data.batt + ' mV<br>' +
            'RSSI: ' + data.rssi + ' dBm<br>' +
            'SNR: ' + data.snr + ' dB<br>' +
            'Accuracy: ' + data.acc + ' m<br>' +
            'Fix age: ' + data.fixAge + ' s' +
            '</div>';
    }

    // ═══════════════════════════════════════════════
    // Device Cards
    // ═══════════════════════════════════════════════
    function renderDeviceCard(dev) {
        const data = dev.data;
        const container = document.getElementById('deviceCards');
        let card = document.getElementById('card-' + dev.id);

        if (!card) {
            card = document.createElement('div');
            card.id = 'card-' + dev.id;
            card.className = 'device-card';
            container.appendChild(card);

            card.addEventListener('click', function (e) {
                if (e.target.tagName === 'BUTTON') return;
                if (dev.marker) {
                    map.setView(dev.marker.getLatLng(), 17);
                    dev.marker.openPopup();
                }
            });
        }

        const age = Math.floor((Date.now() - dev.lastUpdate) / 1000);
        const stale = age > 600;
        card.className = 'device-card' + (stale ? ' stale' : '');

        const statusClass = 'status-' + data.status.toLowerCase().replace('timeout', '');
        const battPct = Math.min(100, Math.max(0, Math.round((data.batt - 3000) / 12)));

        card.innerHTML =
            '<div class="card-header">' +
                '<span class="card-name">' + data.name + '</span>' +
                '<span class="card-status ' + statusClass + '">' + data.status + '</span>' +
            '</div>' +
            '<div class="card-grid">' +
                '<span class="label">Profile</span><span class="value">' + data.profile + '</span>' +
                '<span class="label">Battery</span><span class="value">' + data.batt + ' mV (' + battPct + '%)</span>' +
                '<span class="label">RSSI</span><span class="value">' + data.rssi + ' dBm</span>' +
                '<span class="label">SNR</span><span class="value">' + data.snr + ' dB</span>' +
                '<span class="label">GPS Acc</span><span class="value">' + data.acc + ' m</span>' +
                '<span class="label">Fix Age</span><span class="value">' + data.fixAge + ' s</span>' +
                '<span class="label">Last seen</span><span class="value">' + formatAge(age) + '</span>' +
            '</div>' +
            '<div class="card-actions">' +
                '<button onclick="BP.sendModeCmd(' + dev.id + ',\'' + data.name + '\')">Change Mode</button>' +
                '<button onclick="BP.focusDevice(' + dev.id + ')">Locate</button>' +
            '</div>';
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
    // Measurement Tool
    // ═══════════════════════════════════════════════
    function toggleMeasure() {
        measuring = !measuring;
        document.getElementById('btnMeasure').classList.toggle('active', measuring);

        if (!measuring) {
            clearMeasure();
        }

        map.getContainer().style.cursor = measuring ? 'crosshair' : '';
    }

    function onMapClick(e) {
        if (!measuring) return;

        measurePoints.push(e.latlng);

        L.circleMarker(e.latlng, {
            radius: 4,
            color: '#1d9bf0',
            fillOpacity: 1
        }).addTo(map);

        if (measurePoints.length > 1) {
            var prev = measurePoints[measurePoints.length - 2];
            var curr = e.latlng;
            var dist = prev.distanceTo(curr);
            var total = totalMeasureDistance();

            if (measureLine) {
                measureLine.addLatLng(curr);
            } else {
                measureLine = L.polyline(measurePoints, {
                    color: '#1d9bf0',
                    weight: 2,
                    dashArray: '6 4'
                }).addTo(map);
            }

            var label = L.marker(curr, {
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
        // Also remove circle markers — they're on the map directly
        map.eachLayer(function (layer) {
            if (layer instanceof L.CircleMarker && !(layer instanceof L.Circle)) {
                map.removeLayer(layer);
            }
        });
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

        // Fetch hub status
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
        initMap();
        connectSSE();

        // Fetch initial device list
        fetch('/api/devices')
            .then(function (r) { return r.json(); })
            .then(function (devs) {
                devs.forEach(function (d) { updateDevice(d); });
                if (devs.length > 0) fitAllMarkers();
            })
            .catch(function () { /* SSE will catch up */ });

        // Button handlers
        document.getElementById('btnFitAll').addEventListener('click', fitAllMarkers);
        document.getElementById('btnMeasure').addEventListener('click', toggleMeasure);
        document.getElementById('btnSettings').addEventListener('click', openSettings);
        document.getElementById('btnCloseSettings').addEventListener('click', closeSettings);
        document.getElementById('btnSaveConfig').addEventListener('click', saveConfig);
        document.getElementById('btnSendCmd').addEventListener('click', sendCommand);
        document.getElementById('btnCloseCmd').addEventListener('click', closeCommand);
    }

    // Public API for inline onclick handlers
    window.BP = {
        sendModeCmd: sendModeCmd,
        focusDevice: focusDevice
    };

    // Wait for DOM
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
