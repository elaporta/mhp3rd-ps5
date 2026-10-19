#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-only installation orchestration and resumable owned output state."""
import hashlib
import json
import pathlib
import sys
import time

from audit_resources import digest
from prepare_textures import prepare as prepare_textures, reject_links
from source_support import ROOT, DATA, private, run, save_json


def source_identity():
    sha=hashlib.sha256()
    paths=[]
    for directory in ('src','tools','cmake','assets','LICENSES'):
        paths.extend(p for p in (ROOT/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc')
    paths.extend(ROOT/name for name in ('README.md','INSTALL.md','LICENSE','dependencies.lock.json'))
    for path in sorted(paths):
        sha.update(path.relative_to(ROOT).as_posix().encode()+b'\0'+path.read_bytes())
    return sha.hexdigest()


def texture_identity(pack):
    reject_links(pack.absolute())
    sha=hashlib.sha256()
    files={}
    # Hash each input once. Avoid walking every parent for every mapping on WSL mounts.
    for path in sorted(pack.rglob('*')):
        if path.is_symlink() or (not path.is_dir() and not path.is_file()):
            raise ValueError('Unsupported texture input: '+str(path))
        if path.is_file():
            name=path.relative_to(pack).as_posix()
            files[name]=digest(path)
            sha.update(name.encode()+b'\0'+files[name].encode())
    manifest=pack/'runtime-manifest.json'
    if manifest.is_file():
        for name,expected in json.loads(manifest.read_text()).items():
            relative=pathlib.PurePosixPath(name)
            if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name:
                raise ValueError('Unsafe prepared texture manifest path')
            if files.get(name)!=expected:
                raise ValueError('Prepared texture differs: '+name)
    notice=pack.parent/'README.md'
    if not manifest.is_file() and notice.is_file():
        reject_links(notice.absolute())
        sha.update(b'author-parent-README.md\0'+digest(notice).encode())
    return sha.hexdigest()


def state_path(output):
    key=hashlib.sha256(str(output.relative_to(ROOT)).encode()).hexdigest()[:24]
    return DATA/'install-state'/('install-'+key+'.json')


def existing(output,resume,identity):
    state=state_path(output)
    if output.exists() or state.exists():
        if not resume or not state.is_file():
            raise ValueError('Existing output preserved; use --resume only for this assistant\'s matching state, or select a new --output')
        record=json.loads(state.read_text())
        if record.get('identity')!=identity or record.get('output')!=output.relative_to(ROOT).as_posix():
            raise ValueError('Existing output/input/source identity differs; preserved. Choose a new --output')
        return record
    return None


def execute(output,image,textures,inputs,identity,resume):
    from build_guest import prepare
    import build_application as app
    import build_native_dependencies as native
    from assemble_install import assemble
    state=existing(output,resume,identity)
    if state and state.get('status')=='BUILD_VERIFIED':
        from verify_install import verify
        verify(output)
        print('Existing complete installation verified; no files changed.',flush=True)
        return state['report']
    record={'identity':identity,'output':output.relative_to(ROOT).as_posix(),'status':'IN_PROGRESS'}
    save_json(state_path(output),record)
    run([sys.executable,ROOT/'tools/fetch_dependencies.py'],DATA/'build-logs/fetch.log')
    prepare(image)
    run([sys.executable,ROOT/'tools/prepare_game_codecs.py'],DATA/'build-logs/codec-source.log')
    run([sys.executable,ROOT/'tools/acquire_game_font.py'],DATA/'build-logs/font-source.log')
    native.sdk()
    native.tools()
    native.radv()
    app.runtime()
    app.payload()
    if (textures/'runtime-manifest.json').is_file():
        pack=textures  # Never regenerate an already prepared, verified pack.
    else:
        base=DATA/'prepared-textures'/identity['source_sha256']/identity['textures_sha256']
        pack=base/'NPJB40001'
        if not pack.exists():
            # Failed preparation attempts are preserved; publish a pack only after augmentation succeeds.
            attempt=base/('attempt-'+str(time.time_ns()))
            prepare_textures(textures,attempt)
            run([sys.executable,ROOT/'tools/prepare_resource_maps.py','--pack',attempt,'--acquire'],DATA/'build-logs/resource-maps.log')
            texture_identity(attempt)
            attempt.rename(pack)
        else:
            texture_identity(pack)
    if source_identity()!=identity['source_sha256']:
        raise ValueError('Public sources changed during build; work preserved. Use a new --output for the new sources')
    report=assemble(output,image,pack,inputs,identity['source_sha256'])
    from verify_install import verify
    verify(output)
    record.update(status='BUILD_VERIFIED',report=report)
    save_json(state_path(output),record)
    print('Install folder ready: '+str(output),flush=True)
    print('Finish the PS5 transfer, run mhp3rd-ps5.elf, wait for its success message, then launch.',flush=True)
    return report
