#!/usr/bin/env python3
"""Summarize fixed-LBA CPU fio latency logs using the same AP warmup rule."""
import argparse
import csv
import json
from pathlib import Path
import re
import statistics

from summarize_ap import percentile


def summarize(path, warmup=200):
    text = path.read_text()
    # SPDK may print its EAL banner before fio's JSON object.
    start = re.search(r'\{\s*"fio version"\s*:', text)
    if not start:
        raise ValueError(f'{path}: missing fio JSON')
    document = json.loads(text[start.start():])
    job, = document['jobs']
    if job['error'] or job['write']['total_ios'] or job['read']['total_ios'] != 5200:
        raise ValueError(f'{path}: expected 5200 successful reads and no writes')
    name = path.name.removesuffix('-fio.json')
    log = path.with_name(name + '_lat.log')
    with log.open() as stream:
        rows = [[int(field.strip()) for field in row] for row in csv.reader(stream)]
    if len(rows) != 5200 or not 0 <= warmup < len(rows):
        raise ValueError(f'{log}: incomplete samples or invalid warmup')
    block = rows[0][3]
    for row in rows:
        if row[2] != 0 or row[3] != block or row[4] != 1073741824 or row[1] <= 0:
            raise ValueError(f'{log}: not a fixed read at 1 GiB: {row}')
    values = sorted(row[1] / 1000 for row in rows[warmup:])
    metrics = dict(mean=statistics.mean(values), min=values[0],
                   p50=percentile(values, .50), p95=percentile(values, .95),
                   p99=percentile(values, .99), max=values[-1])
    return dict(name=name, unit='us', samples=len(values), warmup=warmup,
                total_samples=len(rows), block_bytes=block, offset=1073741824,
                timestamp=document['time'], fio_version=document['fio version'],
                options=job['job options'], metric='fio_lat', metrics=metrics)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('json', type=Path, nargs='+')
    parser.add_argument('--warmup', type=int, default=200)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    results = [summarize(path, args.warmup) for path in args.json]
    text = json.dumps(results, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')
