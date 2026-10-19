#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble one private transfer folder from locally built artifacts and immutable user inputs."""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

import build_application as app
import build_native_dependencies as native
from audit_resources import digest
from generate_icon import png
from prepare_textures import reject_links
from source_support import ROOT, DATA, private, save_json

_OWNED = set()
_CHECKED_DIRECTORIES = set()
_TITLE_DIRECTORIES=('yakumo/config','yakumo/logs','yakumo/cache/radv','yakumo/cache/first-frame',
                    'yakumo/saves/ms0/PSP/SAVEDATA','sce_sys')


def checked_path(path, output=False):
    path=pathlib.Path(os.path.abspath(path))
    if output and (path==DATA or not path.is_relative_to(DATA)):
        raise ValueError('Build output must be a child of out/data: '+str(path))
    pending=[]
    # Inputs and output parents are checked once per locked assembly, not once
    # for every ancestor of every PNG on a Windows-mounted WSL filesystem.
    for parent in path.parents:
        if parent in _CHECKED_DIRECTORIES:
            break
        if parent.is_symlink():
            raise ValueError('Symlink in preparation path: '+str(parent))
        if parent.is_dir(): pending.append(parent)
    if path.is_symlink():
        raise ValueError('Symlink in preparation path: '+str(path))
    _CHECKED_DIRECTORIES.update(pending)
    return path


def owned_copy(source, destination):
    checked_path(destination,output=True)
    checked_path(source)
    if destination.exists():
        if not destination.is_file() or destination.stat().st_nlink != 1 or digest(destination) != digest(source):
            raise ValueError('Different existing output preserved: ' + str(destination))
        _OWNED.add(destination)
        return
    destination.parent.mkdir(parents=True,exist_ok=True)
    with source.open('rb') as incoming, destination.open('xb') as outgoing:
        shutil.copyfileobj(incoming,outgoing,1024*1024)
    _OWNED.add(destination)


def owned_bytes(destination, data):
    checked_path(destination,output=True)
    if destination.exists():
        if destination.stat().st_nlink != 1 or destination.read_bytes() != data:
            raise ValueError('Different existing output preserved: ' + str(destination))
        _OWNED.add(destination)
        return
    destination.parent.mkdir(parents=True,exist_ok=True)
    with destination.open('xb') as stream:
        stream.write(data)
    _OWNED.add(destination)


def managed_directories(output,title):
    result={parent for path in _OWNED for parent in path.parents if parent.is_relative_to(output)}
    for relative in _TITLE_DIRECTORIES:
        directory=title/relative
        result.add(directory)
        result.update(parent for parent in directory.parents if parent.is_relative_to(output))
    return result


def json_bytes(value):
    return (json.dumps(value,indent=2,sort_keys=True)+'\n').encode()


def licenses(output, title):
    paths = {
        'MHP3rd-PS5-GPL-3.0.txt': ROOT/'LICENSE',
        'Yakumo-MIT.txt': ROOT/'upstream/Yakumo/LICENSE',
        'tiny-AES-Unlicense.txt': ROOT/'LICENSES/tiny-AES-Unlicense.txt',
        'PS5-PayloadSDK-GPL.txt': ROOT/'deps/PS5_PayloadSDK/LICENSE',
        'PS5-Vulkan-GPL.txt': ROOT/'deps/PS5_Vulkan/LICENSE',
        'PS5-Vulkan-NOTICE.txt': ROOT/'deps/PS5_Vulkan/NOTICE.md',
        'musl-COPYRIGHT.txt': ROOT/'deps/PS5_PayloadSDK/platform/src/regex/COPYRIGHT.musl',
        'NotoSansCJK-OFL.txt': DATA/'dependencies/noto-cjk/LICENSE',
        'FFmpeg-LGPL-2.1.txt': DATA/'dependencies/ffmpeg-7.1.5/COPYING.LGPLv2.1',
        'LLVM-LICENSE.txt': native.PREFIX/'llvm-source/llvm-project-18.1.8.src/LICENSE.TXT',
    }
    # Preserve the actual dependency license filename at the pinned revisions.
    for name,path in list(paths.items()):
        if not path.is_file() and name == 'PS5-Vulkan-GPL.txt':
            paths[name] = ROOT/'deps/PS5_Vulkan/COPYING'
    llvm=native.PREFIX/'llvm-source/llvm-project-18.1.8.src'
    for component in ('libcxx','libcxxabi','libunwind'):
        paths['LLVM-'+component+'-LICENSE.txt']=llvm/component/'LICENSE.TXT'
    for name,path in paths.items():
        if not path.is_file():
            raise ValueError('Required source license missing: '+str(path))
        owned_copy(path,output/'LICENSES'/name)
        owned_copy(path,title/'yakumo/assets/licenses'/name)
    for path in sorted((ROOT/'deps/PS5_Mesa').glob('docs/license*')):
        if path.is_file():
            owned_copy(path,output/'LICENSES'/('Mesa-'+path.name))


def assemble(output, image, pack, inputs, source_identity):
    _OWNED.clear()
    _CHECKED_DIRECTORIES.clear()
    output = private(output)
    title = output/'PPSA99373'
    eboot = app.APP/'native/link/eboot.bin'
    libc = app.APP/'runtime/libc-a.prx'
    payload = app.APP/'permission-payload/mhp3rd-ps5.elf'
    copies = {
        'eboot.bin': eboot, 'sce_module/libc.prx':libc,
        'yakumo/game/EBOOT.ELF':DATA/'game-boot/EBOOT.ELF',
        'yakumo/assets/nids.csv':ROOT/'upstream/Yakumo/configs/nids.csv',
        'yakumo/assets/fonts/NotoSansCJKjp-Regular.otf':DATA/'dependencies/noto-cjk/NotoSansCJKjp-Regular.otf',
        'yakumo/assets/fonts/LICENSE':DATA/'dependencies/noto-cjk/LICENSE',
    }
    for name in ('index.html','menu.js','menu.css'):
        copies['yakumo/assets/web/'+name] = ROOT/'assets/web'/name
    for name,path in copies.items():
        owned_copy(path,title/name)
    for relative in _TITLE_DIRECTORIES:
        private(title/relative).mkdir(parents=True,exist_ok=True)
    param = dict(titleId='PPSA99373',conceptId='99373',contentId='UP9000-PPSA99373_00-YAKUMOPROBE00001',
                 applicationCategoryType=0,applicationDrmType='free',contentBadgeType=1,contentVersion='00.100.001',
                 masterVersion='00.01',downloadDataSize=0,attribute=0,attribute2=0,attribute3=0,
                 requiredSystemSoftwareVersion='0x0000000000000000',sdkVersion='0x0000000000000000',
                 localizedParameters={'defaultLanguage':'en-US','en-US':{'titleName':'MHP3rd'}},
                 gameIntent={'permittedIntents':[{'intentType':'launchActivity'}]})
    owned_bytes(title/'sce_sys/param.json',json_bytes(param))
    owned_bytes(title/'sce_sys/icon0.png',png())
    owned_bytes(title/'yakumo/title.id',b'PPSA99373\n')
    owned_copy(image,title/'yakumo/game/game.iso')
    for path in sorted(pack.rglob('*')):
        if path.is_file():
            owned_copy(path,title/'yakumo/textures/NPJB40001'/path.relative_to(pack))
    owned_copy(payload,output/'mhp3rd-ps5.elf')
    for source,name in [('INSTALL.md','README.md'),('LICENSE','LICENSE')]:
        owned_copy(ROOT/source,output/name)
    licenses(output,title)
    texture = {'runtime_manifest_sha256':digest(pack/'runtime-manifest.json'),'root':'yakumo/textures/NPJB40001','png_count':sum(p.is_file() and p.suffix.lower()=='.png' for p in pack.rglob('*')),
               'source':'User-supplied author texture pack; prepared locally; not distributed'}
    owned_bytes(title/'yakumo/assets/required-texture-pack.json',json_bytes(texture))
    metadata = dict(build='mhp3rd-ps5-source-0.10.0',title_id='PPSA99373',source_identity=source_identity,
        source_only=True,precompiled_baseline=False,hardware_verified=False,manual_acceptance='TODO',
        inputs=inputs,dependencies=json.loads((ROOT/'dependencies.lock.json').read_text()),
        defaults={'internal_scale':6,'presentation_fps':60,'hd_textures':True},
        features=['native_vulkan','compressed_audio','movies','native_controller','system_keyboard','hud_camera_hooks',
                  'web_settings_L3_R3','adhoc_online','save_reload','overlay_interpreter_fallback'],
        source_projection='All 233 host/runtime projections and entry match the maintained accepted-source lock',
        sdk_built_from_source=True,payload_source='title-permissions-0.1.3-final2',
        permission_payload_sha256=digest(payload),permission_success='Permissions applied ... Launch the game now.',
        game_code_private=True,toolchain={'clang':subprocess.check_output(['clang-18','--version'],text=True).splitlines()[0],
                                        'cmake':subprocess.check_output(['cmake','--version'],text=True).splitlines()[0],
                                        'python':sys.version.split()[0]})
    owned_bytes(title/'yakumo/assets/build.json',json_bytes(metadata))
    managed = {p.relative_to(title).as_posix():digest(p) for p in sorted(_OWNED) if p.is_relative_to(title)
               and p.relative_to(title).as_posix() not in ('yakumo/game/game.iso','yakumo/assets/package-manifest.json')
               and not p.relative_to(title).as_posix().startswith('yakumo/textures/')}
    owned_bytes(title/'yakumo/assets/package-manifest.json',json_bytes(managed))
    mode_warning = False
    owned_dirs=managed_directories(output,title)
    for path in output.rglob('*'):
        if path.is_symlink() or not (path.is_file() or path.is_dir()) or (path.is_file() and path.stat().st_nlink != 1):
            raise ValueError('Unsafe output preserved for inspection: '+str(path))
        if path not in _OWNED and path not in owned_dirs:
            continue
        desired = 0o777 if path.is_dir() or path in (title/'eboot.bin',title/'sce_module/libc.prx',output/'mhp3rd-ps5.elf') else 0o666
        path.chmod(desired)
        mode_warning |= (path.stat().st_mode & 0o7777) != desired
    output.chmod(0o777)
    if digest(title/'yakumo/game/game.iso') != inputs['iso_sha256']:
        raise ValueError('Staged image differs; originals remain untouched')
    if mode_warning:
        print('WARNING: host filesystem reports synthetic modes. Run the included title permission payload after transfer.',flush=True)
    return {'status':'BUILD_VERIFIED','hardware_verified':False,'title_id':'PPSA99373',
            'output':output.relative_to(ROOT).as_posix(),'managed_files':len(managed),
            'eboot_sha256':digest(eboot),'payload_sha256':digest(payload),'synthetic_host_modes':mode_warning}
