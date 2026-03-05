#!/usr/bin/env python3
"""
Bluepaws V4 — Mock Hub Server for local GUI preview.
Serves hub/data/ files and simulates collar telemetry via SSE.

Usage:
    python3 mock_server.py
    Open http://localhost:8080 in your browser.
"""

import http.server
import json
import math
import os
import random
import threading
import time

PORT = 8080
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hub', 'data')

# Simulated collars
COLLARS = [
    {'id': 1, 'name': 'Collar_0001', 'lat': 51.8740, 'lon': -2.2390, 'profile': 'normal'},
    {'id': 2, 'name': 'Collar_0002', 'lat': 51.8735, 'lon': -2.2405, 'profile': 'active'},
    {'id': 3, 'name': 'Collar_0003', 'lat': 51.8728, 'lon': -2.2378, 'profile': 'powersave'},
]

sse_clients = []
sse_lock = threading.Lock()
seq_counter = 0
start_time = time.time()


def generate_telemetry(collar):
    global seq_counter
    seq_counter += 1

    # Drift position slightly
    collar['lat'] += random.uniform(-0.0002, 0.0002)
    collar['lon'] += random.uniform(-0.0002, 0.0002)

    statuses = ['Out', 'Out', 'Out', 'Home']
    batt = random.randint(3200, 4100)

    return {
        'id': collar['id'],
        'name': collar['name'],
        'seq': seq_counter,
        'time': int(time.time()),
        'status': random.choice(statuses),
        'profile': collar['profile'],
        'lat': round(collar['lat'], 7),
        'lon': round(collar['lon'], 7),
        'hasGps': True,
        'batt': batt,
        'acc': random.randint(2, 25),
        'fixAge': random.randint(0, 30),
        'rssi': random.randint(-120, -60),
        'snr': round(random.uniform(-5, 12), 1),
        'bleHome': False,
        'cellular': random.random() < 0.1,
    }


def sse_broadcaster():
    """Background thread that sends fake telemetry to all SSE clients."""
    while True:
        time.sleep(random.uniform(3, 8))
        collar = random.choice(COLLARS)
        data = generate_telemetry(collar)
        msg = f"event: telemetry\ndata: {json.dumps(data)}\n\n"

        with sse_lock:
            dead = []
            for i, wfile in enumerate(sse_clients):
                try:
                    wfile.write(msg.encode())
                    wfile.flush()
                except Exception:
                    dead.append(i)
            for i in reversed(dead):
                sse_clients.pop(i)


class MockHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DATA_DIR, **kwargs)

    def do_GET(self):
        if self.path == '/events':
            self.handle_sse()
        elif self.path == '/api/devices':
            self.handle_api_devices()
        elif self.path == '/api/status':
            self.handle_api_status()
        else:
            super().do_GET()

    def do_POST(self):
        if self.path == '/api/command':
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode()
            print(f"[CMD] {body}")
            self.send_json({'ok': True})
        elif self.path == '/api/config':
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode()
            print(f"[CONFIG] {body}")
            self.send_json({'ok': True, 'restart': False})
        else:
            self.send_error(404)

    def handle_sse(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Connection', 'keep-alive')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()

        # Send current state for all collars
        for collar in COLLARS:
            data = generate_telemetry(collar)
            msg = f"event: telemetry\ndata: {json.dumps(data)}\n\n"
            self.wfile.write(msg.encode())
        self.wfile.flush()

        # Register for future broadcasts
        with sse_lock:
            sse_clients.append(self.wfile)

        # Keep connection alive
        try:
            while True:
                time.sleep(1)
        except Exception:
            pass

    def handle_api_devices(self):
        devs = [generate_telemetry(c) for c in COLLARS]
        for d in devs:
            d['age'] = random.randint(0, 60)
        self.send_json(devs)

    def handle_api_status(self):
        self.send_json({
            'uptime': int(time.time() - start_time),
            'rxCount': seq_counter,
            'txCount': 0,
            'crcFails': random.randint(0, 3),
            'devices': len(COLLARS),
            'logEntries': seq_counter,
            'staConnected': False,
            'staIP': '',
            'apIP': '192.168.4.1',
            'freeHeap': 180000,
        })

    def send_json(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(body))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # Quieter logging
        if '/events' not in (args[0] if args else ''):
            super().log_message(fmt, *args)


if __name__ == '__main__':
    # Start SSE broadcaster thread
    t = threading.Thread(target=sse_broadcaster, daemon=True)
    t.start()

    print(f"Bluepaws V4 Mock Server")
    print(f"Serving hub/data/ from: {DATA_DIR}")
    print(f"Open http://localhost:{PORT} in your browser")
    print(f"Simulating {len(COLLARS)} collars with live telemetry")
    print()

    server = http.server.HTTPServer(('', PORT), MockHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutdown.")
