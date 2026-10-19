#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the same source installer in an amd64 Linux toolchain container (macOS or opt-in Linux)."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]


def check_context():
    context=ROOT/'out/data/build-context.json'
    if context.exists() and json.loads(context.read_text())!={'root':'/work','backend':'linux-amd64-llvm18'}:
        raise ValueError('Build context differs; use a separate checkout for native and container builds')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--container',action='store_true')
    parser.add_argument('--iso',type=pathlib.Path,required=True)
    parser.add_argument('--textures',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path)
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--preflight-only',action='store_true')
    args=parser.parse_args()
    if not shutil.which('docker'):
        parser.error('Docker with Linux amd64 container support is required for this backend')
    from prepare_install import output_path
    output=output_path(str(args.output) if args.output else None)
    from prepare_textures import reject_links
    reject_links(args.iso.absolute())
    reject_links(args.textures.absolute())
    image=args.iso.resolve(strict=True)
    textures=args.textures.resolve(strict=True)
    if not image.is_file() or not (textures/'textures.ini').is_file():
        parser.error('Supply an existing image and the author pack directory containing textures.ini')
    # Validate private resources before building the public toolchain image.
    from prepare_install import check_resources
    from install_pipeline import texture_identity,source_identity,existing
    inputs=check_resources(image,textures)
    identity={'iso_sha256':inputs['iso_sha256'],'textures_sha256':texture_identity(textures),'source_sha256':source_identity()}
    existing(output,args.resume,identity)
    try:
        check_context()
    except ValueError as error:
        parser.error(str(error))
    tag='mhp3rd-source-tools:'+hashlib.sha256((ROOT/'tools/Containerfile').read_bytes()).hexdigest()[:16]
    present=subprocess.run(['docker','image','inspect',tag],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
    if not present:
        if args.preflight_only:
            parser.error('The local toolchain image is not built yet; a normal invocation builds it from tools/Containerfile')
        subprocess.run(['docker','build','--platform','linux/amd64','-t',tag,'-f',str(ROOT/'tools/Containerfile'),str(ROOT)],check=True)
    command=['docker','run','--rm','--platform','linux/amd64','--user',str(os.getuid())+':'+str(os.getgid()),
             '-e','HOME=/tmp','-v',str(ROOT)+':/work','-v',str(image)+':/inputs/game.iso:ro',
             '-v',str(textures)+':/inputs/textures:ro']
    if (textures.parent/'README.md').is_file():
        command+=['-v',str(textures.parent/'README.md')+':/inputs/README.md:ro']
    command += [tag,'bash','tools/prepare_install.sh','--iso','/inputs/game.iso','--textures','/inputs/textures',
                '--output',str(pathlib.PurePosixPath('/work')/output.relative_to(ROOT).as_posix())]
    if args.resume: command.append('--resume')
    if args.preflight_only: command.append('--preflight-only')
    return subprocess.run(command).returncode


if __name__=='__main__':
    raise SystemExit(main())
