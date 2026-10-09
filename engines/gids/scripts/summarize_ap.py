#!/usr/bin/env python3
"""Summarize valid GPU-clock samples; reject incomplete/unstable/error runs."""
import argparse
import csv
import io
import json
import math
from pathlib import Path
import statistics


def percentile(values, p):
    index = (len(values) - 1) * p
    low = math.floor(index)
    high = math.ceil(index)
    return values[low] + (values[high] - values[low]) * (index - low)


def summarize(path, warmup):
    if warmup < 0:
        raise ValueError('warmup must be nonnegative')
    meta = {}
    lines = []
    for line in path.read_text().splitlines():
        if line.startswith('# '):
            key, value = line[2:].split('=', 1)
            meta[key] = value
        else:
            lines.append(line)
    if 'end_calibration_error' in meta:
        raise ValueError(meta['end_calibration_error'])
    before = float(meta['ticks_per_us_before'])
    after = float(meta['ticks_per_us_after'])
    if not (math.isfinite(before) and math.isfinite(after) and before > 0 and after > 0):
        raise ValueError(f'{path}: invalid clock calibration: {before}, {after}')
    drift = abs(after / before - 1)
    if drift > 0.02:
        raise ValueError(f'{path}: unstable clock calibration: {before}, {after}')
    rate = (before + after) / 2
    reader = csv.DictReader(io.StringIO('\n'.join(lines)))
    timing_columns = ('cqe_observed', 'slot_reclaimed')
    present = [key in (reader.fieldnames or []) for key in timing_columns]
    if any(present) and not all(present):
        raise ValueError(f'{path}: incomplete per-entry timing columns')
    rows = [{key: int(value) for key, value in row.items()} for row in reader]
    planned = meta.get('access') == 'planned_random'
    operation = meta.get('operation', 'read')
    if operation not in ('read', 'write') or (operation == 'write' and not planned):
        raise ValueError(f'{path}: unsupported profiling operation')
    if planned and len(rows) != int(meta['expected_samples']):
        raise ValueError(f'{path}: incomplete planned replay')
    if operation == 'write' and meta.get('write_readback') != '1':
        raise ValueError(f'{path}: writes have no readback verification')
    # Upstream per-entry timing columns are filled by the PCIe profile
    # path only; RDMA CSVs leave BOTH columns zero in EVERY row. Once any
    # stamp is populated, require valid timing for the whole run, including
    # warmup. A missing stamp must not disable validation of the other rows.
    entry_timing = all(present) and (
        meta.get('transport') == 'pcie'
        or any(row[key] != 0 for row in rows for key in timing_columns))
    middle = timing_columns if entry_timing else ()
    for sequence, row in enumerate(rows):
        if row['sample'] != sequence or row['bytes'] != rows[0]['bytes'] or row['bytes'] <= 0:
            raise ValueError(f'{path}: invalid sequence or mixed block sizes')
        if planned:
            if row['offset'] < 0 or row['offset'] % row['bytes'] or row['offset'] + row['bytes'] > int(meta['namespace_bytes']):
                raise ValueError(f'{path}: offset outside plan namespace or unaligned')
        elif row['offset'] != rows[0]['offset']:
            raise ValueError(f'{path}: unplanned reads require a fixed block')
        if row['status'] or row['mismatches']:
            raise ValueError(f'{path}: failed payload/completion sample {row["sample"]}')
        if entry_timing and any(row[key] <= 0 for key in timing_columns):
            raise ValueError(f'{path}: missing per-entry timestamps in sample {row["sample"]}')
        tail = ('ap_done',) if operation == 'write' else ('first_read', 'all_read')
        stamps = [row[k] for k in ('sq_begin', 'doorbell', 'cq_seen') + middle + tail]
        if operation == 'write' and (row['first_read'] or row['all_read']):
            raise ValueError(f'{path}: write sample contains read endpoint timestamps')
        if stamps[0] == 0 or sorted(stamps) != stamps:
            raise ValueError(f'{path}: missing/nonmonotonic timestamps: {row}')
    if len(rows) <= warmup:
        raise ValueError(f'{path}: fewer samples than warmup={warmup}')
    rows = rows[warmup:]
    metrics = {}
    stages = [
        ('sqe_to_doorbell', 'sq_begin', 'doorbell'),
        ('sqe_to_cq', 'sq_begin', 'cq_seen'),
        ('doorbell_to_cq', 'doorbell', 'cq_seen'),
    ]
    stages += [
        ('sqe_to_ap_done', 'sq_begin', 'ap_done'),
        ('cq_to_ap_done', 'cq_seen', 'ap_done'),
    ] if operation == 'write' else [
        ('sqe_to_first_load', 'sq_begin', 'first_read'),
        ('cq_to_first_load', 'cq_seen', 'first_read'),
        ('sqe_to_full_verify', 'sq_begin', 'all_read'),
    ]
    if entry_timing:
        # cq_seen and cqe_observed stamp the same event through two hooks
        # (generated queue header vs upstream timing), so their delta is a
        # cross-check, not a latency. slot_reclaimed closes cq_dequeue +
        # put_cid; the read-side tail then splits the old cq_to_first_load
        # fixed overhead into queue-reclaim and post-reclaim portions.
        stages += [
            ('cq_seen_to_cqe_observed', 'cq_seen', 'cqe_observed'),
            ('cqe_to_slot_reclaimed', 'cqe_observed', 'slot_reclaimed'),
        ]
        if operation != 'write':
            stages.append(('slot_reclaimed_to_first_load', 'slot_reclaimed', 'first_read'))
    for name, start, end in stages:
        values = sorted((r[end] - r[start]) / rate for r in rows)
        metrics[name] = dict(mean=statistics.mean(values), min=values[0],
                             p50=percentile(values, .5), p95=percentile(values, .95),
                             p99=percentile(values, .99), max=values[-1])
    selected = [r['offset'] for r in rows]
    address = dict(unique_offsets=len(set(selected)), min_offset=min(selected), max_offset=max(selected))
    if planned:
        capacity = int(meta['namespace_bytes'])
        address['namespace_bytes'] = capacity
        address['sampled_bytes_fraction'] = len(set(selected)) * rows[0]['bytes'] / capacity
        address['decile_counts'] = [sum(o * 10 // capacity == i for o in selected) for i in range(10)]
    return dict(file=str(path), metadata=meta, samples=len(rows), warmup=warmup,
                total_samples=len(rows) + warmup,
                operation=operation, block_bytes=rows[0]['bytes'],
                offset=None if planned else rows[0]['offset'], address=address,
                clock_drift_percent=100 * drift, ticks_per_us=rate,
                unit='us', metrics=metrics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', nargs='+', type=Path)
    parser.add_argument('--warmup', type=int, default=200)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error('warmup must be nonnegative')
    result = [summarize(path, args.warmup) for path in args.csv]
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + '\n')
    else:
        print(text)


if __name__ == '__main__':
    main()
