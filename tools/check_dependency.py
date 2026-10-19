#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check a pinned, clean source checkout without fetching or changing anything."""
import argparse
import json
import pathlib
import subprocess
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name')
    args = parser.parse_args()
    entry = json.loads((ROOT / 'dependencies.lock.json').read_text())['repositories'][args.name]
    path = ROOT / entry['path']
    reject_links(path)
    if not (path / '.git').exists():
        parser.error('Missing pinned source checkout: ' + args.name)
    head = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '--no-optional-locks', '-C', str(path), 'status', '--porcelain'], text=True).strip()
    if head != entry['commit'] or dirty:
        parser.error('Dependency revision/worktree differs: ' + args.name)
    print(args.name + ': pinned source verified')


if __name__ == '__main__':
    main()
