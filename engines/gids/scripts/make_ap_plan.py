#!/usr/bin/env python3
"""Record uniform random offsets in an active range and matching fio v2 replay."""
import argparse
import json
from pathlib import Path
import random


def active_range(capacity, bs, active_offset=0, active_length=None):
    if not 0 < bs <= 16 * 1024 * 1024 or bs % 8 or not bs <= capacity < 2**64:
        raise ValueError('invalid namespace capacity or block size')
    if active_offset < 0 or active_offset % bs:
        raise ValueError('active offset must be nonnegative and block-size aligned')
    if active_length is None:
        # Preserve full-namespace sampling: omit any final partial block.
        active_length = (capacity - active_offset) // bs * bs
    if active_length <= 0 or active_length % bs:
        raise ValueError('active length must be positive and block-size aligned')
    if active_offset + active_length > capacity:
        raise ValueError('active range exceeds namespace capacity')
    return active_offset, active_length


def offsets(capacity, bs, count, seed, active_offset=0, active_length=None):
    active_offset, active_length = active_range(capacity, bs, active_offset, active_length)
    if not 0 < count <= 65536:
        raise ValueError('sample count must be between 1 and 65536')
    if not 0 <= seed < 2**64:
        raise ValueError('seed must fit uint64')
    rng = random.Random(seed)
    return [active_offset + rng.randrange(active_length // bs) * bs for _ in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capacity', type=int, required=True, help='actual namespace capacity in bytes')
    parser.add_argument('--bs', type=int, required=True, help='request block size in bytes')
    parser.add_argument('--active-offset', type=int, default=0,
                        help='first byte of the active range, aligned to bs (default: 0)')
    parser.add_argument('--active-length', type=int,
                        help='active range length in bytes, aligned to bs '
                             '(default: all remaining complete blocks)')
    parser.add_argument('--count', type=int, default=5200)
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--operation', choices=('read', 'write'), required=True)
    parser.add_argument('--filename', required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    args = parser.parse_args()
    if any(c.isspace() for c in args.filename) or not args.filename:
        parser.error('fio replay filename must contain no whitespace')
    try:
        active_offset, active_length = active_range(
            args.capacity, args.bs, args.active_offset, args.active_length)
        selected = offsets(args.capacity, args.bs, args.count, args.seed,
                           active_offset, active_length)
        if args.operation == 'read' and args.count * args.bs > 2 * 1024**3:
            raise ValueError('reference snapshot exceeds 2 GiB')
    except ValueError as error:
        parser.error(str(error))
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
                         max_offset=max(selected), namespace_bytes=args.capacity,
                         active_offset=active_offset, active_length=active_length,
                         active_end=active_offset + active_length)))


if __name__ == '__main__':
    main()
