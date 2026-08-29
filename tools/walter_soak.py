"""Capture a real Walter 1010 soak; never synthesize packets or cloud ACKs.

Run with the PlatformIO Python (pyserial). Output belongs under ignored .pio/.
Only this process may own COM26. Control through --request status/stop, not a
second serial terminal. Closing the monitor does NOT stop the collar.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import time


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def observe(state, line, stamp):
    """Summarize evidence, without turning serial claims into cloud verification."""
    state['updated_at'] = stamp
    if line.startswith('[WALTER] device='):
        state['status'] = line
    if line.startswith('[WALTER] uptime='):
        state['health'] = line
    for prefix, key in (('[CYCLE] START ', 'cycle'), ('[CYCLE] SLEEP ', 'sleep'),
                        ('[GNSS] Event ', 'gnss'), ('[BENCH] ', 'bench')):
        if line.startswith(prefix):
            state[key] = {'at': stamp, 'line': line}
    if any(line.startswith(p) for p in ('[CYCLE]', '[LTE] ACCEPTED',
            '[LTE CMD]', '[WALTER] Idle', '[WALTER] Could not', '[GNSS] Valid')):
        state.setdefault('events', []).append({'at': stamp, 'line': line})
        state['events'] = state['events'][-1000:]


def ready_to_start(lines):
    return any(re.fullmatch(r'\[WALTER\] device=1010 hub=\d+ busy=0 running=0 '
                           r'profile=\S+ home_stub=[01] credentials=configured', x)
               for x in lines)


def save(path, state):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(state, indent=2) + '\n', encoding='utf8')
    temporary.replace(path)


def monitor(args):
    import serial  # Pure evidence-parser tests do not require serial access.
    directory = args.run_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    lock = directory / 'monitor.lock'
    # A stale lock requires explicit operator investigation; never steal a port.
    with lock.open('x', encoding='utf8') as f:
        f.write(str(os.getpid()))
    state = {'pid': os.getpid(), 'started_at': utc(), 'port': 'COM26',
             'baud': 115200, 'monitor': 'starting', 'events': []}
    port = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
    port.port, port.dtr, port.rts = 'COM26', True, False
    pending = bytearray()
    try:
        with (directory / 'serial.log').open('a', encoding='utf8', buffering=1) as log:
            port.open()

            def record(line):
                stamp = utc()
                log.write(stamp + ' ' + line + '\n')
                observe(state, line, stamp)

            def send(command):
                record('[HOST] ' + command)
                port.write((command + '\n').encode('ascii'))

            def read_for(seconds):
                lines = []
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    pending.extend(port.read(port.in_waiting or 1))
                    while b'\n' in pending:
                        raw, _, rest = pending.partition(b'\n')
                        pending[:] = rest
                        line = raw.decode('utf8', errors='replace').rstrip('\r')
                        record(line)
                        lines.append(line)
                    if len(pending) > 16384:
                        raise RuntimeError('Unterminated serial line exceeds limit')
                return lines

            read_for(1)
            send('status')
            initial = read_for(3)
            if args.start:
                if not ready_to_start(initial):
                    raise RuntimeError('Refusing start: expected idle, configured device 1010')
                send('clock ' + str(int(time.time())))
                if not any('Explicit host UTC seed accepted' in x for x in read_for(1)):
                    raise RuntimeError('Host UTC seed was not accepted')
                for command in ('bench off', 'home off', 'profile normal'):
                    send(command)
                    read_for(0.5)
                send('status')
                configured = read_for(2)
                if not (ready_to_start(configured)
                        and any('profile=Normal home_stub=0' in x for x in configured)
                        and any(x.startswith('[BENCH] offline=0 ') for x in configured)):
                    raise RuntimeError('Online Normal/away configuration not confirmed')
                send('start')
            state['monitor'] = 'running'
            next_status = time.monotonic() + 30
            while True:
                read_for(1)
                request = directory / 'request.txt'
                if request.exists():
                    command = request.read_text(encoding='utf8').strip()
                    request.unlink()
                    if command not in ('status', 'stop'):
                        record('[HOST] Rejected unknown control request')
                    else:
                        send(command)
                if time.monotonic() >= next_status:
                    send('status')
                    next_status = time.monotonic() + 30
                save(directory / 'state.json', state)
    except BaseException as error:
        state['monitor'] = 'stopped'
        state['error'] = str(error)
        save(directory / 'state.json', state)
        raise
    finally:
        port.close()
        lock.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', required=True, type=Path)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--start', action='store_true', help='Start idle 1010 online, Normal/away')
    action.add_argument('--request', choices=('status', 'stop'))
    args = parser.parse_args()
    if args.request:
        if not (args.run_dir / 'monitor.lock').exists():
            parser.error('No monitor lock; check monitor health before requesting control')
        with (args.run_dir / 'request.txt').open('x', encoding='utf8') as f:
            f.write(args.request)
    else:
        monitor(args)


if __name__ == '__main__':
    main()
