#!/usr/bin/env python3
"""Check complete Tutti files against the manifest (base revision plus local changes)."""
import hashlib
import json
from pathlib import Path

base = Path(__file__).resolve().parents[1] / 'vendor'
manifest = json.loads((base / 'manifest.json').read_text())
root = base / manifest['source']
expected = manifest['sha256']
actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
errors = [f'file set differs: {sorted(actual ^ set(expected))}'] if actual != set(expected) else []
for name, digest in expected.items():
    path = root / name
    if path.is_file():
        data = path.read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != digest:
            errors.append(f'changed vendor file: {name}')
if errors:
    raise SystemExit('\n'.join(errors))
local_changes = len(manifest.get('local_changes', {}))
print(f'Tutti base {manifest["commit"]} + {local_changes} local source changes: '
      f'{len(expected)} complete files verified')
