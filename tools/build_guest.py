#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the supported main corpus from the user's image; preserve native overlay interpreter fallback."""
import argparse
import json
import pathlib
import subprocess
import sys

from source_support import ROOT, DATA, complete, digest, record, run, save_json


def prepare(image):
    gate = json.loads((ROOT/'tools/native_probe_evidence.json').read_text())
    if gate.get('status') != 'HARDWARE_VERIFIED' or gate.get('manual_runs', 0) < 1:
        raise ValueError('The project native probe must have passed on hardware before a full-game build')
    host = DATA/'executable-tool'
    run(['cmake','-S',ROOT/'cmake/game-executable','-B',host,'-G','Ninja','-DCMAKE_CXX_COMPILER=clang++-18'],DATA/'build-logs/executable-configure.log')
    run(['cmake','--build',host,'-j2'],DATA/'build-logs/executable-build.log')
    run([sys.executable, ROOT/'tools/prepare_game_boot.py','--iso',image],DATA/'build-logs/executable-prepare.log')
    framework = DATA/'framework-host'
    upstream = ROOT/'upstream/Yakumo'
    run(['cmake','-S',upstream,'-B',framework,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
         '-DPSPRECOMP_PROFILE=','-DPSPRECOMP_BUILD_TESTS=OFF','-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF',
         '-DCMAKE_CXX_COMPILER=clang++-18'],DATA/'build-logs/recompiler-configure.log')
    run(['cmake','--build',framework,'--target','psp_analyze','psp_recomp','-j2'],DATA/'build-logs/recompiler-build.log')
    elf = DATA/'game-boot/EBOOT.ELF'
    manifest = DATA/'game-boot/corpus-input.json'
    expected = {'executable_sha256':digest(elf),'upstream':json.loads((ROOT/'dependencies.lock.json').read_text())['repositories']['yakumo']['commit'],
                'dispatch_target':'0x088E8B18'}
    generated = DATA/'game-boot/generated'
    if manifest.exists() and json.loads(manifest.read_text()) != expected:
        raise ValueError('Different generated corpus preserved; inspect the existing input manifest')
    if not (generated/'generated_registry.cpp').exists() or not manifest.exists():
        generated.mkdir(parents=True,exist_ok=True)
        run([framework/'psp_analyze',elf,DATA/'game-boot/analysis.json'],DATA/'build-logs/analyze.log')
        run([framework/'psp_recomp',elf,'--auto',generated,'--dispatch-target','0x088E8B18'],DATA/'build-logs/recompile.log')
        save_json(manifest,expected)
    units = list(generated.glob('generated_unit_*.cpp'))
    if len(units) != 89:
        raise ValueError('Expected all 89 supported main executable units')
    # Never accept changed generated code as a completed source step.
    files={p.name:digest(p) for p in generated.iterdir() if p.is_file()}
    receipt=DATA/'game-boot/corpus-files.json'
    if receipt.exists() and json.loads(receipt.read_text()) != files:
        raise ValueError('Completed generated corpus changed; preserved for inspection')
    if not receipt.exists():
        save_json(receipt,files)
    return elf


def host_aot():
    build = DATA/'game-boot/host-aot'
    run(['cmake','-S',ROOT/'cmake/game-aot','-B',build,'-G','Ninja','-DCMAKE_CXX_COMPILER=clang++-18',
         '-DYAKUMO_AOT_OPTIMIZATION=2'],DATA/'build-logs/host-aot-configure.log')
    run(['cmake','--build',build,'-j2'],DATA/'build-logs/host-aot-build.log')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso',type=pathlib.Path,required=True)
    parser.add_argument('--host-aot',action='store_true')
    args=parser.parse_args()
    prepare(args.iso.resolve())
    if args.host_aot:
        host_aot()
