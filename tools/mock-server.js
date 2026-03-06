#!/usr/bin/env node
/*
  BluePaws V4 — Mock Hub Server
  Simulates multiple animals sending TLV telemetry over SSE.
  Run:  node tools/mock-server.js
  Open: http://localhost:3000
*/

const http = require('http');
const fs   = require('fs');
const path = require('path');

const PORT = 3000;
const DATA_DIR = path.join(__dirname, '..', 'hub', 'data');

// ── Simulated animals ──────────────────────────────
const animals = [
    { id: 0x0001, name: 'Whiskers',  lat: 51.5055, lon: -0.0900, status: 'Out',     profile: 'Normal',    batt: 4050 },
    { id: 0x0002, name: 'Mittens',   lat: 51.5040, lon: -0.0880, status: 'Home',    profile: 'PowerSave', batt: 3900 },
    { id: 0x0003, name: 'Shadow',    lat: 51.5070, lon: -0.0920, status: 'Out',     profile: 'Active',    batt: 3750 },
    { id: 0x0004, name: 'Patches',   lat: 51.5030, lon: -0.0860, status: 'Out',     profile: 'Normal',    batt: 4100 },
    { id: 0x0005, name: 'Luna',      lat: 51.5062, lon: -0.0940, status: 'Home',    profile: 'Normal',    batt: 3500 },
];

let seq = 0;

function randomDrift() { return (Math.random() - 0.5) * 0.0004; }

function buildTelemetry(a) {
    seq++;
    // Drift position slightly each tick
    a.lat += randomDrift();
    a.lon += randomDrift();
    return JSON.stringify({
        id:       a.id,
        name:     a.name,
        seq:      seq,
        time:     Math.floor(Date.now() / 1000),
        status:   a.status,
        profile:  a.profile,
        lat:      parseFloat(a.lat.toFixed(7)),
        lon:      parseFloat(a.lon.toFixed(7)),
        hasGps:   true,
        batt:     a.batt + Math.floor(Math.random() * 20 - 10),
        acc:      Math.floor(Math.random() * 15) + 3,
        fixAge:   Math.floor(Math.random() * 30),
        rssi:     -80 - Math.floor(Math.random() * 30),
        snr:      (Math.random() * 10 + 2).toFixed(1),
        bleHome:  a.status === 'Home',
        cellular: false
    });
}

// ── SSE clients ────────────────────────────────────
const sseClients = new Set();

function sseBroadcast(eventName, data) {
    const frame = 'event: ' + eventName + '\ndata: ' + data + '\n\n';
    for (const res of sseClients) {
        res.write(frame);
    }
}

// Heartbeat every 5 seconds so the browser knows the connection is alive
setInterval(function () {
    sseBroadcast('heartbeat', '{}');
}, 5000);

// Send a random animal update every 2 seconds
setInterval(function () {
    const a = animals[Math.floor(Math.random() * animals.length)];
    sseBroadcast('telemetry', buildTelemetry(a));
}, 2000);

// ── MIME types ─────────────────────────────────────
const MIME = {
    '.html': 'text/html',
    '.css':  'text/css',
    '.js':   'application/javascript',
    '.json': 'application/json',
    '.png':  'image/png',
    '.svg':  'image/svg+xml'
};

// ── HTTP server ────────────────────────────────────
const server = http.createServer(function (req, res) {
    // SSE endpoint
    if (req.url === '/events') {
        res.writeHead(200, {
            'Content-Type':  'text/event-stream',
            'Cache-Control': 'no-cache',
            'Connection':    'keep-alive',
            'Access-Control-Allow-Origin': '*'
        });
        res.write('\n');
        sseClients.add(res);
        req.on('close', function () { sseClients.delete(res); });

        // Send initial burst of all animals
        animals.forEach(function (a) {
            var d = buildTelemetry(a);
            res.write('event: telemetry\ndata: ' + d + '\n\n');
        });
        return;
    }

    // REST: device list
    if (req.url === '/api/devices') {
        var devs = animals.map(function (a) { return JSON.parse(buildTelemetry(a)); });
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(devs));
        return;
    }

    // REST: hub status
    if (req.url === '/api/status') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            uptime: Math.floor(process.uptime()),
            rxCount: seq,
            txCount: 0,
            crcFails: 0,
            devices: animals.length,
            logEntries: seq,
            freeHeap: 180000,
            staConnected: true,
            staIP: '192.168.1.42',
            apIP: '192.168.4.1'
        }));
        return;
    }

    // REST: command (stub)
    if (req.url === '/api/command' && req.method === 'POST') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true }));
        return;
    }

    // REST: config (stub)
    if (req.url === '/api/config' && req.method === 'POST') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true }));
        return;
    }

    // Static files
    var filePath = req.url === '/' ? '/index.html' : req.url;
    var fullPath = path.join(DATA_DIR, filePath);
    var ext = path.extname(fullPath);
    var mime = MIME[ext] || 'application/octet-stream';

    fs.readFile(fullPath, function (err, data) {
        if (err) {
            res.writeHead(404);
            res.end('Not found');
            return;
        }
        res.writeHead(200, { 'Content-Type': mime });
        res.end(data);
    });
});

server.listen(PORT, function () {
    console.log('BluePaws mock server running at http://localhost:' + PORT);
    console.log('Simulating ' + animals.length + ' animals with live SSE updates every 2s');
});
