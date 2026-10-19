#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch only the locked public font and its license; never rewrite the lock."""
import hashlib
import json
import pathlib
import urllib.request
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]


def fetch(path, spec):
    reject_links(path)
    if path.exists():
        data = path.read_bytes()
    else:
        with urllib.request.urlopen(spec['url'], timeout=30) as response:
            data = response.read()
    if len(data) != spec['bytes'] or hashlib.sha256(data).hexdigest() != spec['sha256']:
        raise ValueError('Locked asset differs; existing input preserved: ' + str(path))
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)


if __name__ == '__main__':
    spec = json.loads((ROOT / 'dependencies.lock.json').read_text())['assets']['game_font']
    output = ROOT / 'out/data/dependencies/noto-cjk'
    for name, entry in spec['files'].items():
        if pathlib.Path(name).name != name:
            raise ValueError('Asset lock must use simple filenames')
        fetch(output / name, entry)
    print('Locked public font and license ready under out/data; dependency lock unchanged.')
