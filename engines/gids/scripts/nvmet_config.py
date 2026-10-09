#!/usr/bin/env python3
"""Snapshot/restore configfs around a target-module change. Never modifies disks.

clear and restore require an explicit --execute. The snapshot is also a record
of the exact allowed object set; clear refuses unexpected ports/subsystems.
"""
import argparse
import json
from pathlib import Path
import stat

ROOT = Path('/sys/kernel/config/nvmet')

def snapshot():
    result = {'objects': {}, 'attributes': {}, 'links': {}}
    for group in ('hosts', 'subsystems', 'ports'):
        result['objects'][group] = sorted(p.name for p in (ROOT/group).iterdir())
    for path in ROOT.rglob('*'):
        name = str(path.relative_to(ROOT))
        if path.is_symlink():
            result['links'][name] = str(path.resolve().relative_to(ROOT))
        elif path.is_file() and path.stat().st_mode & stat.S_IRUSR:
            try:
                result['attributes'][name] = path.read_text().strip()
            except OSError:
                pass
    return result

def write(name, value):
    path = ROOT/name
    if not path.exists() or not path.stat().st_mode & stat.S_IWUSR:
        return
    if value == '(null)':
        return
    path.write_text(value+'\n')

def clear(data):
    # Include namespace attributes and ACL/port links in the precondition:
    # a new child under an existing subsystem is still an unexpected change.
    current_snapshot = snapshot()
    if current_snapshot != data:
        raise RuntimeError('configfs changed since snapshot; refusing to clear')
    for group, names in data['objects'].items():
        current = {p.name for p in (ROOT/group).iterdir()}
        if current != set(names):
            raise RuntimeError(f'{group} changed since snapshot; refusing to clear')
    for name in data['links']:
        (ROOT/name).unlink()
    for subsystem in data['objects']['subsystems']:
        path = ROOT/'subsystems'/subsystem
        for ns in (path/'namespaces').iterdir():
            (ns/'enable').write_text('0\n')
            ns.rmdir()
        path.rmdir()
    for group in ('ports', 'hosts'):
        for name in data['objects'][group]:
            (ROOT/group/name).rmdir()

def existing_link(name, target):
    path = ROOT/name
    if path.is_symlink():
        if path.resolve() != (ROOT/target).resolve():
            raise RuntimeError(f'conflicting configfs link: {name}')
        return True
    if path.exists():
        raise RuntimeError(f'configfs link path already exists: {name}')
    return False

def restore(data):
    # Detect conflicts before changing attributes or enabling namespaces.
    for name, target in data['links'].items():
        existing_link(name, target)
    for group in ('hosts', 'subsystems', 'ports'):
        for name in data['objects'][group]:
            (ROOT/group/name).mkdir(exist_ok=True)
    attrs = data['attributes']
    for name in attrs:
        parts = name.split('/')
        if len(parts) == 5 and parts[2] == 'namespaces':
            (ROOT/Path(*parts[:4])).mkdir(exist_ok=True)
    for name, value in attrs.items():
        if name.endswith('/enable'):
            continue
        write(name, value)
    for name, value in attrs.items():
        if name.endswith('/enable'):
            write(name, value)
    for name, target in data['links'].items():
        if not existing_link(name, target):
            (ROOT/name).symlink_to(ROOT/target)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('snapshot', 'clear', 'restore'))
    p.add_argument('file', type=Path)
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    if args.action == 'snapshot':
        # Refuse overwriting the original rollback record.
        with args.file.open('x') as f:
            json.dump(snapshot(), f, indent=2)
    else:
        data = json.loads(args.file.read_text())
        if not args.execute:
            print(json.dumps(data['objects'], indent=2))
            print('No change; add --execute to apply.')
            return
        (clear if args.action == 'clear' else restore)(data)

if __name__ == '__main__':
    main()
