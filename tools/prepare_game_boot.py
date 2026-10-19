#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the user's executable locally; no prior build or game bytes are supplied."""
import argparse
import hashlib
import json
import pathlib
import subprocess

from audit_resources import EXPECTED_EBOOT, Iso, digest, sfo_strings
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]
DECRYPTED = '55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=pathlib.Path, required=True)
    parser.add_argument('--tool', type=pathlib.Path, default=ROOT / 'out/data/executable-tool/prepare_game_executable')
    args = parser.parse_args()
    output = ROOT / 'out/data/game-boot'
    for path in (args.iso.absolute(), args.tool.absolute(), output):
        reject_links(path)
    if not args.tool.resolve().is_relative_to(ROOT / 'out/data') or not args.tool.is_file():
        parser.error('Build the host preparation tool under out/data first')
    iso = Iso(args.iso)
    try:
        sfo = iso.contents('/PSP_GAME/PARAM.SFO')
        encrypted = iso.contents('/PSP_GAME/SYSDIR/EBOOT.BIN')
        if sfo_strings(sfo).get('DISC_ID') != 'NPJB40001' or hashlib.sha256(encrypted).hexdigest() != EXPECTED_EBOOT:
            raise ValueError('Unsupported image identity or executable; originals preserved')
    finally:
        iso.close()
    files = {'EBOOT.BIN': encrypted, 'PARAM.SFO': sfo}
    elf = output / 'EBOOT.ELF'
    for name, data in files.items():
        path = output / name
        reject_links(path)
        if path.exists() and path.read_bytes() != data:
            raise ValueError('Different existing output preserved: ' + str(path))
    reject_links(elf)
    if elf.exists() and digest(elf) != DECRYPTED:
        raise ValueError('Different existing executable preserved')
    output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = output / name
        if not path.exists():
            with path.open('xb') as stream:
                stream.write(data)
    if not elf.exists():
        subprocess.run([str(args.tool.resolve()), str(output / 'EBOOT.BIN'), str(elf)], check=True)
    if digest(elf) != DECRYPTED:
        raise ValueError('Prepared executable differs; preserve output for inspection')
    manifest = {name: digest(output / name) for name in ('EBOOT.BIN', 'EBOOT.ELF', 'PARAM.SFO')}
    report = output / 'preparation.json'
    reject_links(report)
    encoded = (json.dumps(manifest, indent=2) + '\n').encode()
    if report.exists() and report.read_bytes() != encoded:
        raise ValueError('Different preparation report preserved')
    if not report.exists():
        with report.open('xb') as stream:
            stream.write(encoded)
    print('Guest executable prepared locally; the installation assistant builds the remaining source stages.')


if __name__ == '__main__':
    main()
