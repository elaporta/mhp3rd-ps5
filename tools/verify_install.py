#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only verification of a complete local transfer folder; never hardware evidence."""
import argparse
import json
import pathlib

from audit_resources import digest
from prepare_textures import reject_links


def verify(output):
    output=pathlib.Path(output).absolute()
    reject_links(output)
    title=output/'PPSA99373'
    for path in output.rglob('*'):
        if path.is_symlink() or not (path.is_file() or path.is_dir()) or (path.is_file() and path.stat().st_nlink!=1):
            raise ValueError('Unsafe output file: '+str(path))
    param=json.loads((title/'sce_sys/param.json').read_text())
    if param.get('titleId')!='PPSA99373' or param.get('localizedParameters',{}).get('en-US',{}).get('titleName')!='MHP3rd':
        raise ValueError('Installed title identity differs')
    if (title/'yakumo/title.id').read_text()!='PPSA99373\n':
        raise ValueError('Title ownership marker differs')
    manifest=json.loads((title/'yakumo/assets/package-manifest.json').read_text())
    for name,expected in manifest.items():
        relative=pathlib.PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
            raise ValueError('Unsafe package manifest path')
        if digest(title/name)!=expected:
            raise ValueError('Managed output differs: '+name)
    for name in ('eboot.bin','sce_module/libc.prx','yakumo/game/EBOOT.ELF','sce_sys/icon0.png',
                 'yakumo/assets/web/index.html','yakumo/assets/web/menu.js','yakumo/assets/web/menu.css',
                 'yakumo/assets/fonts/NotoSansCJKjp-Regular.otf'):
        if name not in manifest:
            raise ValueError('Required application asset missing: '+name)
    for path,magic in ((title/'eboot.bin',b'\x4f\x15\x3d\x1d'),
                       (title/'sce_module/libc.prx',b'\x54\x14\xf5\xee'),
                       (title/'yakumo/game/EBOOT.ELF',b'\x7fELF'),(output/'mhp3rd-ps5.elf',b'\x7fELF')):
        with path.open('rb') as stream:
            if stream.read(4)!=magic:
                raise ValueError('Program format differs: '+str(path))
    build=json.loads((title/'yakumo/assets/build.json').read_text())
    if build.get('source_only') is not True or build.get('precompiled_baseline') is not False:
        raise ValueError('Source installation provenance differs')
    if build.get('build')=='mhp3rd-ps5-source-0.11.0':
        if param.get('contentVersion')!='00.110.000' or param.get('attribute3')!=0x40:
            raise ValueError('Graphics build identity or high-refresh metadata differs')
    if build.get('build')=='mhp3rd-ps5-source-0.12.0':
        expected='00.120.001' if build.get('revision')=='mods-r1' else '00.120.000'
        if param.get('contentVersion')!=expected or param.get('attribute3')!=0x40:
            raise ValueError('Mods build identity or high-refresh metadata differs')
        mods=build.get('mods',{})
        if not (title/'yakumo/mods').is_dir() or mods.get('apply')!='restart' or mods.get('new_mods_enabled') is not False:
            raise ValueError('Local mods layout or restart policy differs')
    if digest(title/'yakumo/game/game.iso')!=build['inputs']['iso_sha256']:
        raise ValueError('Private image differs')
    if digest(title/'yakumo/game/EBOOT.ELF')!='55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c':
        raise ValueError('Guest executable differs')
    if digest(output/'mhp3rd-ps5.elf')!=build['permission_payload_sha256']:
        raise ValueError('Permission payload differs')
    pack=title/'yakumo/textures/NPJB40001'
    required=json.loads((title/'yakumo/assets/required-texture-pack.json').read_text())
    if digest(pack/'runtime-manifest.json')!=required['runtime_manifest_sha256']:
        raise ValueError('Texture manifest differs')
    for name,expected in json.loads((pack/'runtime-manifest.json').read_text()).items():
        relative=pathlib.PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name or digest(pack/name)!=expected:
            raise ValueError('Staged texture differs: '+name)
    if not (title/'yakumo/saves/ms0/PSP/SAVEDATA').is_dir() or not (output/'LICENSES').is_dir():
        raise ValueError('Required save/license directory missing')
    print('Local installation hashes/layout PASS.',flush=True)
    return build


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=pathlib.Path)
    verify(parser.parse_args().output)
