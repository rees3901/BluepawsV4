"""Run bounded serial GNSS experiments without printing credentials or resetting USB.

Example: python tools/walter_gnss_run.py --out .pio/baseline.jsonl --commands "gnss" "gnss settle" "gnss hot" --repeat 3
Commands run while the collar schedule is stopped; --resume restores it afterwards.
"""
import argparse
import json
import pathlib
import time
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', default='COM10')
parser.add_argument('--out', required=True)
parser.add_argument('--commands', nargs='+', required=True)
parser.add_argument('--repeat', type=int, default=1)
parser.add_argument('--resume', action='store_true')
args = parser.parse_args()
path = pathlib.Path(args.out)
path.parent.mkdir(parents=True, exist_ok=True)
p = serial.Serial()
p.port, p.baudrate, p.timeout, p.dtr = args.port, 115200, .2, False
p.open()
start = time.monotonic()
with path.open('w', encoding='utf-8') as log:
    def record(kind, text, trial=''):
        log.write(json.dumps(dict(elapsed_s=round(time.monotonic()-start, 3), kind=kind, trial=trial, text=text))+'\n')
        log.flush()
        print(f'[{trial or kind}] {text}', flush=True)

    def send(command, trial=''):
        record('command', command, trial)
        p.write((command+'\n').encode())

    def wait_idle(timeout, trial='', stop_at=None):
        if not trial:
            time.sleep(.3)
            send('status', trial)
        deadline = time.monotonic()+timeout
        stopped = False
        while time.monotonic()<deadline:
            line = p.readline().decode('utf-8', 'replace').strip()
            if line:
                record('serial', line, trial)
                if (trial and 'Idle after inspection' in line) or (not trial and (
                    '[WALTER] Idle' in line or ('busy=0 running=0' in line and 'device=' in line))):
                    return True
            if stop_at is not None and not stopped and time.monotonic()>=stop_at:
                send('stop', trial)
                stopped = True
        return False

    try:
        send('stop')
        if not wait_idle(45):
            raise RuntimeError('No idle confirmation; refusing overlapping modem commands')
        for repeat in range(args.repeat):
            for command in args.commands:
                trial = f'{repeat+1}:{command}'
                send(command, trial)
                # Host stop is an extra guard for the old 180-second single-fix image.
                if not wait_idle(100, trial, time.monotonic()+61):
                    raise RuntimeError(f'No idle confirmation after {trial}')
                send('status', trial)
                until = time.monotonic()+.5
                while time.monotonic()<until:
                    line=p.readline().decode('utf-8','replace').strip()
                    if line: record('serial',line,trial)
    finally:
        send('stop')
        if wait_idle(40) and args.resume:
            send('start')
        p.close()
