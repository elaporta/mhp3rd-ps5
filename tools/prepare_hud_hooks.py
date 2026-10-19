#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve upstream HUD originals from the existing private corpus; never regenerate it."""
import argparse
import pathlib
import re

ADDRESSES = ('08876874', '088EA9B0', '088EB07C', '088EBF0C', '088EC060',
             '088EC0F8', '088EC1F0', '088EC2EC', '088EC3E0', '088EC63C',
             '088ECB74', '088ECC38', '088ECCE4', '088ECD9C', '088ECE58')


def resolve(generated, addresses):
    found = {}
    pattern = re.compile(r'register_function\(0x([0-9A-F]+)u, &(recomp_unit_[0-9]+),')
    for source in sorted(generated.glob('generated_unit_*.cpp')):
        for address, unit in pattern.findall(source.read_text()):
            if address not in addresses:
                continue
            if address in found:
                raise ValueError('Duplicate original: ' + address)
            found[address] = unit
    if set(found) != set(addresses):
        raise ValueError('Missing originals: ' + ', '.join(sorted(set(addresses) - set(found))))
    return found


def hooks(generated):
    found = resolve(generated, ADDRESSES)
    return ''.join(f'MHP3RD_HUD_FUNCTION(0x{address}u, {found[address]})\n' for address in ADDRESSES)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    args = parser.parse_args()
    from prepare_textures import reject_links
    root = pathlib.Path(__file__).resolve().parents[1]
    for path in (args.generated, args.output):
        reject_links(path.absolute())
        if not path.resolve().is_relative_to(root / 'out/data'):
            parser.error('Generated game files must stay under out/data')
    content = hooks(args.generated)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() and args.output.read_text() != content:
        raise ValueError('Different generated output preserved; choose a new build directory')
    if not args.output.exists():
        args.output.write_text(content)
    print('Resolved all 15 HUD originals from the existing corpus')
