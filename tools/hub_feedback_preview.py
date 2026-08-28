"""Loopback-only UI fixture. No hardware, cloud calls, tokens or stored files.

Run: py -3.11 tools/hub_feedback_preview.py
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs
import json
import mimetypes
import time

ASSETS = Path(__file__).resolve().parents[1] / 'hub' / 'data'
START = time.monotonic()
COMMANDS = []
HUB = dict(format='hub_status',ingest_path='hub_self',gateway_guid16='0010',mode='home',
           latitude=51.907,longitude=-2.240,fix_age_s=3,uptime_s=1000,free_heap=150000,
           wifi_rssi_dbm=-40,ble_enabled=True,ble_advertising=True,ble_settled=True,
           reporting_profile='power_save',control_poll_s=5,
           display_name='Home Hub · UI test',home_emoji='🏡',portable_emoji='📱',marker_colour='#38bdf8')


def device():
    age = (time.monotonic() - START) % 20
    return dict(id=1001, name='Podge · UI test', emoji='🐱', colour='#1d9bf0',
                seq=1, time=int(time.time()), status='Lost', profile='Lost Alert',
                errorPresent=False, resetReason=3, lat=51.90597, lon=-2.2394,
                hasGps=True, batt=3900, rssi=-94, snr=8, age=age,
                localId=int((time.monotonic()-START)//20)+1,
                rxWindowMs=max(0,int(10000-age*1000)), verification='pending')


def commands():
    return [dict(device=c['device'],cmdSeq=c['seq'],type='profile',profile=c['profile'],
                 status='acked' if time.monotonic()-c['at'] >= 3 else 'queued',
                 age_ms=int((time.monotonic()-c['at'])*1000)) for c in COMMANDS]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, payload, mime='application/json'):
        body = payload if isinstance(payload,bytes) else json.dumps(payload).encode()
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path=self.path.split('?',1)[0]
        if path == '/events':
            self.send_response(200)
            self.send_header('Content-Type','text/event-stream')
            self.end_headers()
            try:
                last_packet = None
                last_commands = None
                for _ in range(120):
                    data=device()
                    if data['localId'] != last_packet:
                        self.wfile.write(('event: telemetry\ndata: '+json.dumps(data)+'\n\n').encode())
                        last_packet=data['localId']
                    items=commands()
                    signature=[(c['cmdSeq'],c['status']) for c in items]
                    if signature != last_commands:
                        for c in items:
                            self.wfile.write(('event: cmd_ack\ndata: '+json.dumps(c)+'\n\n').encode())
                        last_commands=signature
                    self.wfile.write(b'event: heartbeat\ndata: {}\n\n')
                    self.wfile.flush()
                    time.sleep(1)
            except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):
                pass
        elif path == '/api/hub-presence': self.send(HUB)
        elif path == '/api/devices': self.send([device()])
        elif path == '/api/commands': self.send(commands())
        elif path == '/api/status': self.send(dict(mode='off_grid',hubMode='off_grid',staConnected=False,apEnabled=True,apIP='192.168.4.1',freeHeap=150000,uptime=10,devices=1,rxCount=2,txCount=1))
        elif path == '/api/welcome': self.send(dict(hub_id='0010',recent_collars=1,known_collars=1,last_report_age_s=2,time_synced=False))
        elif path in ('/api/history','/api/ble'): self.send([])
        elif path == '/bluepaws-hub.url': self.send(b'[InternetShortcut]\r\nURL=http://192.168.4.1/\r\n','application/octet-stream')
        else:
            name={'/':'index.html','/welcome':'welcome.html'}.get(path,path.lstrip('/'))
            file=(ASSETS / name).resolve()
            if not file.is_relative_to(ASSETS) or not file.is_file(): self.send_error(404); return
            self.send(file.read_bytes(),(mimetypes.guess_type(str(file))[0] or 'text/plain')+'; charset=utf-8')

    def do_POST(self):
        if self.path == '/api/hub-preferences':
            HUB.update(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
            HUB['ble_advertising']=HUB['ble_enabled']
            self.send(dict(accepted=True))
            return
        if self.path != '/api/command': self.send_error(404); return
        form=parse_qs(self.rfile.read(int(self.headers['Content-Length'])).decode())
        COMMANDS.append(dict(device=1001, seq=len(COMMANDS)+1,
                             profile=form.get('mode',['Active'])[0],at=time.monotonic()))
        self.send(dict(ok=True,device=1001,cmdSeq=len(COMMANDS)))


if __name__ == '__main__':
    print('Synthetic hub UI: http://127.0.0.1:8792/ (no hardware/cloud)',flush=True)
    ThreadingHTTPServer(('127.0.0.1',8792),Handler).serve_forever()
