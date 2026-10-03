"""Bounded two-port serial observation. Does not reset devices or send commands.

Run with PlatformIO's Python (pyserial installed). Keep output under .secrets.
A clean serial log alone cannot certify power consumption, range or GNSS accuracy.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import threading
import time
import serial

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ports', nargs='+', required=True)
    parser.add_argument('--seconds', type=int, default=86400)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('Duration must be positive')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    lock = threading.Lock()
    errors = []
    counts = {port: 0 for port in args.ports}
    opens = {port: 0 for port in args.ports}
    disconnects = {port: 0 for port in args.ports}
    with args.output.open('x', encoding='utf-8') as log:
        def worker(port):
            while time.monotonic() < deadline:
                link = None
                try:
                    link = serial.Serial(port=None, baudrate=115200, timeout=1)
                    link.dtr = False; link.rts = False; link.port = port
                    link.open()
                    opens[port] += 1
                    while time.monotonic() < deadline:
                        line = link.readline().decode('utf-8', errors='replace').strip()
                        if not line: continue
                        with lock:
                            counts[port] += 1
                            log.write(json.dumps({'utc': datetime.now(timezone.utc).isoformat(), 'port': port, 'line': line}) + '\n')
                            log.flush()
                except (serial.SerialException, OSError) as exc:
                    # Native USB disappears during deep sleep. Reopen on wake;
                    # never toggle DTR/RTS to force a reset during observation.
                    disconnects[port] += 1
                    with lock:
                        log.write(json.dumps({'utc': datetime.now(timezone.utc).isoformat(), 'port': port,
                                              'event': 'serial_unavailable', 'error': str(exc)}) + '\n')
                        log.flush()
                    time.sleep(max(0, min(2, deadline - time.monotonic())))
                except Exception as exc:
                    with lock: errors.append({'port': port, 'error': str(exc)})
                    break
                finally:
                    if link is not None: link.close()
            if not opens[port]:
                with lock: errors.append({'port': port, 'error': 'Port never opened during observation'})
        threads = [threading.Thread(target=worker, args=(port,)) for port in args.ports]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
    summary = {'duration_requested_s': args.seconds, 'lines': counts, 'opens': opens,
               'serial_unavailable_events': disconnects, 'errors': errors}
    args.output.with_suffix('.summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary))
    if errors: raise SystemExit(1)

if __name__ == '__main__':
    main()
