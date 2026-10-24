#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build all application features and the existing permission payload from source."""
import argparse
import json
import pathlib
import sys

import build_native_dependencies as native
from source_support import ROOT, DATA, digest, run, save_json

APP = DATA/'application-source-0.12.0'


def native_aot():
    native.sdk()
    build = DATA/'game-boot/native-aot'
    run(['cmake','-S',ROOT/'cmake/game-aot','-B',build,'-G','Ninja',
         '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/runtime/ps5-toolchain.cmake'),
         '-DMHP3RD_SDK_ROOT='+str(native.SDK),'-DYAKUMO_AOT_OPTIMIZATION=2'],DATA/'build-logs/native-aot-configure.log',env=native.ENV)
    run(['cmake','--build',build,'-j2'],DATA/'build-logs/native-aot-build.log',env=native.ENV)
    return build/'libactual_game_aot.a'


def runtime():
    native.sdk()
    native.radv()
    native.tools()
    aot = native_aot()
    build = APP/'native'
    run(['cmake','-S',ROOT/'cmake/install-runtime','-B',build,'-G','Ninja',
         '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/runtime/ps5-toolchain.cmake'),
         '-DMHP3RD_SDK_ROOT='+str(native.SDK),'-DMHP3RD_RADV_ROOT='+str(native.RADV),
         '-DMHP3RD_VK_HEADERS='+str(native.RADV/'include'),'-DMHP3RD_AOT_ARCHIVE='+str(aot),
         '-DMHP3RD_NATIVE_TOOL='+str(native.PREFIX/'tools/ps5-native-tool')],DATA/'build-logs/application-configure.log',env=native.ENV)
    run(['cmake','--build',build,'-j2'],DATA/'build-logs/application-build.log',env=native.ENV)
    return build/'link/eboot.bin', runtime_shim()


def runtime_shim():
    native.tools()
    # Build the existing runtime shim from its public manifests twice, never copy a prebuilt module.
    directory = APP/'runtime'
    directory.mkdir(parents=True,exist_ok=True)
    driver = ROOT/'deps/PS5_Vulkan/tooling/native'
    for copy in ('a','b'):
        raw = directory/('libc-'+copy+'.elf')
        signed = directory/('libc-'+copy+'.prx')
        run([native.PREFIX/'tools/libc-builder',driver/'runtime/api-surface.txt',driver/'runtime/imports.txt',raw],DATA/'build-logs/runtime-shim.log')
        run([native.PREFIX/'tools/ps5-native-tool','self','--sign','--in',raw,'--out',signed],DATA/'build-logs/runtime-shim.log')
    if digest(directory/'libc-a.prx') != digest(directory/'libc-b.prx') or digest(directory/'libc-a.prx') != 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036':
        raise ValueError('Generated runtime shim does not reproduce the pinned identity')
    return directory/'libc-a.prx'


def payload():
    native.sdk()
    run([sys.executable,ROOT/'tools/prepare_payload_sources.py'],DATA/'build-logs/payload-source.log')
    source = DATA/'install-build/payload-source'
    build = APP/'permission-payload'
    run(['cmake','-S',ROOT/'cmake/title-permissions','-B',build,'-G','Ninja','-DYAKUMO_NATIVE=ON',
         '-DYAKUMO_PERMISSION_SOURCE_OVERRIDE='+str(source/'title_permissions_final.c'),
         '-DYAKUMO_PERMISSION_CRT_OVERRIDE='+str(source/'crt_final.c'),
         '-DYAKUMO_PERMISSION_OUTPUT_NAME=mhp3rd-ps5.elf','-DCMAKE_SYSTEM_NAME=FreeBSD',
         '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY','-DCMAKE_C_COMPILER='+str(native.SDK/'bin/prospero-clang'),
         '-DCMAKE_ASM_COMPILER='+str(native.SDK/'bin/prospero-clang')],DATA/'build-logs/payload-configure.log',env=native.ENV)
    run(['cmake','--build',build,'-j2'],DATA/'build-logs/payload-build.log',env=native.ENV)
    result = build/'mhp3rd-ps5.elf'
    if not result.is_file():
        raise ValueError('Existing permission payload did not build')
    return result


def host_tests():
    # A clean source archive has no retained host AOT cache. Build the generated
    # main archive before linking the feature-test application's entry point.
    from build_guest import host_aot
    # Host checks verify behavior/signatures, not timing. Keep their complete
    # generated corpus at O0; the separate native application remains O2.
    aot=host_aot(optimization=0)
    build = APP/'host'
    run(['cmake','-S',ROOT/'cmake/install-runtime','-B',build,'-G','Ninja','-DCMAKE_CXX_COMPILER=clang++-18',
         '-DMHP3RD_VK_HEADERS='+str(ROOT/'deps/PS5_Mesa/include'),
         '-DMHP3RD_HOST_AOT_ARCHIVE='+str(aot)],DATA/'build-logs/features-configure.log')
    run(['cmake','--build',build,'-j2'],DATA/'build-logs/features-build.log')
    run(['ctest','--test-dir',build,'--output-on-failure','--timeout','90'],DATA/'build-logs/features-tests.log')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage',choices=['native-aot','runtime','runtime-shim','payload','host-tests'],required=True)
    stage=parser.parse_args().stage.replace('-','_')
    globals()[stage]()
