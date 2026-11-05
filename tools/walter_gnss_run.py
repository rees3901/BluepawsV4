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
parser.add_argument('--startup', action='store_true', help='Capture the first automatic cycle before stopping it')
parser.add_argument('--seed-clock', action='store_true', help='Explicit diagnostic host UTC seed; never supplies coordinates')
parser.add_argument('--abort-after', type=float, help='Cancellation stress: stop each GNSS trial after this many seconds')
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
        # Always drain serial into the file. Keep stdout small so an unattended
        # shell pipe cannot block CN0 reads and distort acquisition timing.
        if kind == 'command' or any(marker in text for marker in (
            '[GNSS SAMPLE]', '[GNSS REJECT]', '[GNSS SETTLE]', '[GNSS CLEANUP]',
            '[ASSIST]', '[MODEM RELEASE]', 'bootstrap failed', '[LTE] ACCEPTED')):
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
        if args.startup:
            deadline=time.monotonic()+300
            while time.monotonic()<deadline:
                line=p.readline().decode('utf-8','replace').strip()
                if line:
                    record('serial',line,'0:startup')
                    if '[CYCLE] SLEEP' in line: break
            else:
                raise RuntimeError('Automatic startup cycle did not reach sleep')
        send('stop')
        if not wait_idle(45):
            raise RuntimeError('No idle confirmation; refusing overlapping modem commands')
        if args.seed_clock:
            send(f'clock {int(time.time())}')
            time.sleep(.3)
        for repeat in range(args.repeat):
            for command in args.commands:
                trial = f'{repeat+1}:{command}'
                send(command, trial)
                # Host stop is an extra guard for the old 180-second single-fix image.
                gnss=command.startswith('gnss')
                # Sampling has its own device deadline; allow RF cleanup afterwards.
                # Use --abort-after 61 if comparing the original 180 s single-fix image.
                cutoff=args.abort_after if args.abort_after is not None else 90
                if not wait_idle(130 if gnss else 360, trial, time.monotonic()+cutoff if gnss else None):
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
