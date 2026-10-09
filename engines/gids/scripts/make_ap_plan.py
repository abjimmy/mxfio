#!/usr/bin/env python3
"""Record uniform full-namespace random offsets and matching fio v2 replay."""
import argparse
import json
from pathlib import Path
import random


def offsets(capacity, bs, count, seed):
    if not 0 < bs <= 16 * 1024 * 1024 or bs % 8 or capacity < bs or not 0 < count <= 65536:
        raise ValueError('invalid capacity, block size or sample count')
    if not 0 <= seed < 2**64:
        raise ValueError('seed must fit uint64')
    rng = random.Random(seed)
    return [rng.randrange(capacity // bs) * bs for _ in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capacity', type=int, required=True)
    parser.add_argument('--bs', type=int, required=True)
    parser.add_argument('--count', type=int, default=5200)
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--operation', choices=('read', 'write'), required=True)
    parser.add_argument('--filename', required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    args = parser.parse_args()
    if any(c.isspace() for c in args.filename) or not args.filename:
        parser.error('fio replay filename must contain no whitespace')
    if args.operation == 'read' and args.count * args.bs > 2 * 1024**3:
        parser.error('reference snapshot exceeds 2 GiB')
    selected = offsets(args.capacity, args.bs, args.count, args.seed)
    plan = Path(str(args.prefix) + '.plan')
    iolog = Path(str(args.prefix) + '.iolog')
    # Exclusive creation prevents an old measurement from acquiring a new plan.
    with plan.open('x') as f:
        f.write(f'GIDS_AP_PLAN_V1 {args.operation} {args.bs} {args.capacity} {args.count} {args.seed}\n')
        f.writelines(f'{offset}\n' for offset in selected)
    with iolog.open('x') as f:
        f.write(f'fio version 2 iolog\n{args.filename} add\n{args.filename} open\n')
        f.writelines(f'{args.filename} {args.operation} {offset} {args.bs}\n' for offset in selected)
        f.write(f'{args.filename} close\n')
    print(json.dumps(dict(plan=str(plan), iolog=str(iolog), count=len(selected),
                         unique_offsets=len(set(selected)), min_offset=min(selected),
                         max_offset=max(selected), namespace_bytes=args.capacity)))


if __name__ == '__main__':
    main()
