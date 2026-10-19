#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Name the existing original camera helper without regenerating game code."""
import argparse
import pathlib
from prepare_hud_hooks import resolve


def header(generated):
    unit = resolve(generated, ('08878B70',))['08878B70']
    return f'''// Resolved from the existing private corpus; do not commit.
#pragma once
#include "camera/game_camera.hpp"
namespace psprecomp {{ void {unit}(Runtime&, AllegrexContext&); }}
namespace yakumo {{
inline mhp3rd::camera::RotationFunction native_camera_original() {{ return &psprecomp::{unit}; }}
}}
'''


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
    content = header(args.generated)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() and args.output.read_text() != content:
        raise ValueError('Different generated output preserved; choose a new build directory')
    if not args.output.exists():
        args.output.write_text(content)
    print('Resolved original camera helper from existing AOT registrations')
