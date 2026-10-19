#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate user inputs, then generate a complete installation from pinned public sources."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

from audit_resources import EXPECTED_EBOOT, Iso, digest, inspect_textures, sfo_strings
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]


def output_path(value, root=ROOT):
    path = pathlib.Path(os.path.abspath(value)) if value else root / 'out/data/install'
    data = root / 'out/data'
    reject_links(path)
    if path == data or not path.is_relative_to(data) or not path.resolve().is_relative_to(data.resolve()):
        raise ValueError('Output must be a child of ' + str(data))
    reserved = {'native-source','game-boot','application-source-0.10.0','install-state','install-build',
                'dependencies','downloads','prepared-textures','build-logs','framework-host','executable-tool'}
    if path.relative_to(data).parts[0] in reserved:
        raise ValueError('Output overlaps a reserved build directory: '+str(path))
    return path


def check_existing(output, resume, identity=None):
    if identity is None:
        if output.exists():
            raise ValueError('Existing output preserved; a matching source/input state is required for resume')
        return None
    from install_pipeline import existing
    return existing(output, resume, identity)


def check_resources(image, textures):
    reject_links(image)
    reject_links(textures)
    if not image.is_file():
        raise ValueError('ISO file not found: ' + str(image))
    if not textures.is_dir() or not (textures / 'textures.ini').is_file():
        raise ValueError('Texture directory must contain textures.ini: ' + str(textures))
    for path in textures.rglob('*'):
        if path.is_symlink() or (not path.is_dir() and not path.is_file()):
            raise ValueError('Unsupported texture input: ' + str(path))
    iso = Iso(image)
    try:
        if sfo_strings(iso.contents('/PSP_GAME/PARAM.SFO')).get('DISC_ID') != 'NPJB40001':
            raise ValueError('Unsupported image: DISC_ID must be NPJB40001')
        if hashlib.sha256(iso.contents('/PSP_GAME/SYSDIR/EBOOT.BIN')).hexdigest() != EXPECTED_EBOOT:
            raise ValueError('Unsupported image executable; originals preserved')
    finally:
        iso.close()
    pack = inspect_textures(textures)
    if pack['unsafe'] or not pack['png_count'] or not pack['effective_keys']:
        raise ValueError('Texture input must have PNG files and safe [hashes] mappings')
    return dict(iso_sha256=digest(image), textures=pack)


def dependency_status(root=ROOT):
    from fetch_dependencies import inspect
    lock=json.loads((root/'dependencies.lock.json').read_text())
    result=[]
    for name,entry in lock['repositories'].items():
        state=inspect(name,entry,root)
        result.append(name+': '+{'pinned':'pinned, clean source checkout','pending':'owned fetch can resume',
                                  'missing':'not fetched (pinned source checkout required)'}[state])
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description='Prepare a complete private PS5 install from pinned public sources and your own inputs.')
    parser.add_argument('--iso', type=pathlib.Path, required=True)
    parser.add_argument('--textures', type=pathlib.Path, required=True)
    parser.add_argument('--output', help='Child of out/data (default: out/data/install)')
    parser.add_argument('--preflight-only', action='store_true', help='Audit inputs/requirements without downloads or writes')
    parser.add_argument('--resume', action='store_true', help='Resume owned build steps and matching staged files; preserve conflicts')
    args = parser.parse_args(argv)
    try:
        output = output_path(args.output)
        if any(char.isspace() or char in '\"\'`$;&|<>(){}[]*?!#' for char in str(ROOT)):
            raise ValueError('Use a checkout path without whitespace or shell metacharacters, or use --container. ISO/texture paths may contain spaces.')
        if sys.platform!='linux' or platform.machine().lower() not in ('x86_64','amd64'):
            raise ValueError('Native builds require amd64 Linux/WSL; use the --container backend on other hosts')
        required = ('git','cmake','c++','clang-18','clang++-18','llvm-config-18','lld-18','llvm-ar-18',
                    'llvm-nm-18','llvm-readelf-18','llvm-ranlib-18','llvm-strip-18','rsync','make','ninja','pkg-config','bison','flex','glslangValidator','spirv-val')
        missing=[name for name in required if not shutil.which(name)]
        if missing:
            raise ValueError('Missing source-build tools: '+', '.join(missing)+'. See INSTALL.md for native/container setup.')
        version = subprocess.check_output(['cmake', '--version'], text=True)
        match = re.search(r'cmake version (\d+)\.(\d+)', version)
        if not match or tuple(map(int, match.groups())) < (3, 22):
            raise ValueError('CMake 3.22 or newer is required')
        for module in ('yaml','ply','packaging','elftools'):
            try:
                __import__(module)
            except ImportError:
                raise ValueError('Missing Python source-build module: '+module+'. See INSTALL.md')
        print('Checking image identity and texture inputs (read-only)...', flush=True)
        image=args.iso.absolute()
        textures=args.textures.absolute()
        inputs=check_resources(image,textures)
        from install_pipeline import source_identity, texture_identity, execute
        identity=dict(iso_sha256=inputs['iso_sha256'],textures_sha256=texture_identity(textures),source_sha256=source_identity())
        check_existing(output,args.resume,identity)
        for item in dependency_status():
            print(item,flush=True)
        context=ROOT/'out/data/build-context.json'
        expected_context={'root':str(ROOT),'backend':'linux-amd64-llvm18'}
        if context.exists() and json.loads(context.read_text())!=expected_context:
            raise ValueError('Build context differs; use a separate checkout for native and container builds')
        gate=json.loads((ROOT/'tools/native_probe_evidence.json').read_text())
        if gate.get('status')!='HARDWARE_VERIFIED':
            raise ValueError('Project native-probe hardware gate is not satisfied')
        parent=output.parent
        while not parent.exists(): parent=parent.parent
        required_space=40*1024**3+2*image.stat().st_size+3*inputs['textures']['bytes']
        if shutil.disk_usage(parent).free<required_space:
            raise ValueError('Insufficient free space: allow at least 40 GiB plus image/texture staging space')
        print('Image and texture identity accepted. The English v6.3.0 patch label is not independently authenticated.',flush=True)
        if source_identity()!=identity['source_sha256']:
            raise ValueError('Public sources changed during audit; retry the same command before building')
        if args.preflight_only:
            print('Preflight PASS. Pinned public sources will be fetched as needed; no files changed.',flush=True)
            return 0
        # One owner at a time. Never run two CMake builds over the same private work trees.
        import fcntl
        lock=ROOT/'out/data/install-build.lock'
        lock.parent.mkdir(parents=True,exist_ok=True)
        with lock.open('a') as stream:
            try:
                fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError('Another installation build is active; existing work preserved')
            from source_support import save_json
            if not context.exists():
                save_json(context,expected_context)
            execute(output,image,textures,inputs,identity,args.resume)
        return 0
    except KeyboardInterrupt:
        print('Interrupted; partial outputs and logs preserved. Repeat the same command with --resume.',file=sys.stderr)
        return 130
    except (ValueError, OSError, KeyError, UnicodeError, subprocess.CalledProcessError, RuntimeError) as error:
        print('ERROR: '+str(error),file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
