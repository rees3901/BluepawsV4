"""Summarise Walter GNSS JSONL logs against a user-supplied reference point.

This measures separation from the supplied reference, not surveyed accuracy.
Firmware selection never uses the reference. No coordinate averaging is applied.
"""
import argparse
import collections
import csv
import json
import math
import pathlib
import re
import statistics

def separation(lat, lon, ref):
    a, b = map(math.radians, (lat, ref[0]))
    h = math.sin((b-a)/2)**2 + math.cos(a)*math.cos(b)*math.sin(math.radians(ref[1]-lon)/2)**2
    return 6371000*2*math.asin(math.sqrt(min(1, h)))

def fields(text):
    return dict(re.findall(r'([a-zA-Z_][a-zA-Z_0-9]*)=([^ ]+)', text))

def analyse(paths, ref):
    trials = []
    samples = []
    for path in paths:
        runs = {}
        for line in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
            record = json.loads(line)
            key = record['trial']
            if not key:
                continue
            run = runs.setdefault(key, dict(file=pathlib.Path(path).name, trial=key, samples=[], rejects=[], summary={}))
            text = record['text']
            if record['kind'] == 'command' and text != 'status':
                run.setdefault('start_s', record['elapsed_s'])
            if '[GNSS SAMPLE]' in text:
                sample = fields(text)
                for k in ('n', 'elapsed_ms', 'ttf_ms', 'status', 'uncertainty', 'lat', 'lon', 'utc', 'entries', 'cn0_ge30'):
                    value = sample[k].removesuffix('m')
                    sample[k] = float(value)
                sample['error_m'] = separation(sample['lat'], sample['lon'], ref)
                sample['arrival_s'] = record['elapsed_s']-run.get('start_s', record['elapsed_s'])
                sample['file'], sample['trial'] = run['file'], key
                run['samples'].append(sample)
                samples.append(sample)
            elif '[GNSS REJECT]' in text:
                run['rejects'].append(fields(text)['reason'])
            elif '[GNSS SETTLE] CLOSED' in text:
                run['summary'] = fields(text)
        for run in runs.values():
            if not run['samples'] and not run['summary']:
                continue
            summary = run['summary']
            selected = None
            if summary.get('selected') == '1':
                matches = [s for s in run['samples'] if s['utc'] == float(summary['utc'])
                           and abs(s['uncertainty']-float(summary['uncertainty'].removesuffix('m'))) < .11]
                if matches:
                    selected = matches[-1]
            run['selected_error_m'] = round(selected['error_m'], 2) if selected else None
            run['selected_uncertainty_m'] = selected['uncertainty'] if selected else None
            run['first_event_s'] = round(run['samples'][0]['arrival_s'], 3) if run['samples'] else None
            run['first_error_m'] = round(run['samples'][0]['error_m'], 2) if run['samples'] else None
            run['mode'] = run['trial'].split(':', 1)[1]
            trials.append(run)
    return trials, samples

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+')
    parser.add_argument('--reference', nargs=2, type=float, required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    trials, samples = analyse(args.logs, args.reference)
    dest = pathlib.Path(args.out)
    dest.mkdir(parents=True, exist_ok=True)
    (dest/'results.json').write_text(json.dumps(dict(reference=args.reference, trials=trials), indent=2), encoding='utf-8')
    if samples:
        with (dest/'samples.csv').open('w', newline='', encoding='utf-8') as f:
            writer=csv.DictWriter(f, fieldnames=list(dict.fromkeys(k for sample in samples for k in sample)))
            writer.writeheader();writer.writerows(samples)
    grouped = collections.defaultdict(list)
    for run in trials:
        grouped[(run['file'], run['mode'])].append(run)
    rows=[]
    for (file, mode), runs in grouped.items():
        valid=[r for r in runs if r['selected_error_m'] is not None]
        errors=sorted(r['selected_error_m'] for r in valid)
        durations=[float(r['summary']['elapsed_ms'])/1000 for r in runs if 'elapsed_ms' in r['summary']]
        first_times=[r['first_event_s'] for r in runs if r['first_event_s'] is not None]
        first_ttf=[r['samples'][0]['ttf_ms']/1000 for r in runs if r['samples']]
        rows.append(dict(file=file, mode=mode, trials=len(runs), selected=len(valid),
            settled=sum(r['summary'].get('quality')=='SETTLED' for r in runs),
            median_error_m=round(statistics.median(errors),2) if errors else None,
            max_error_m=max(errors) if errors else None,
            within25m=sum(e<=25 for e in errors),
            median_first_event_s=round(statistics.median(first_times),2) if first_times else None,
            median_modem_first_ttf_s=round(statistics.median(first_ttf),2) if first_ttf else None,
            median_window_s=round(statistics.median(durations),2) if durations else None,
            max_window_s=max(durations) if durations else None,
            rejects=dict(collections.Counter(reason for r in runs for reason in r['rejects']))))
    (dest/'summary.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
    print(json.dumps(rows,indent=2))

if __name__=='__main__':
    main()
