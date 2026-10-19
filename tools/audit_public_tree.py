#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Git publication candidates without staging, committing or publishing anything."""
import pathlib
import subprocess

ROOT=pathlib.Path(__file__).resolve().parents[1]
FORBIDDEN={'.iso','.cso','.elf','.prx','.bin','.a','.o','.obj','.so','.dylib','.dll','.exe',
           '.self','.pkg','.pbp','.edat','.sav','.zip','.7z','.rar','.tar','.gz','.xz','.bc','.spv'}
# User-authorized PS5 screenshots for documentation, separate from game assets.
SCREENSHOTS={'photos/'+name for name in (
    'ps5-forest.jpg','ps5-waterfall.jpg','ps5-forest-canopy.jpg','ps5-gathering-hall.jpg',
    'ps5-online-party.jpg','ps5-riverside.jpg','ps5-online-status.jpg')}


def audit():
    raw=subprocess.check_output(['git','-C',str(ROOT),'ls-files','-z','--cached','--others','--exclude-standard'])
    names=sorted(set(name.decode('utf-8') for name in raw.split(b'\0') if name))
    failures=[]
    for name in names:
        relative=pathlib.PurePosixPath(name)
        path=ROOT/name
        if relative.parts[0] in ('out','deps','upstream') or relative.suffix.lower() in FORBIDDEN:
            failures.append(name+': private/build output')
            continue
        if any(part in ('NPJB40001','SAVEDATA','textures','saves','__pycache__') for part in relative.parts):
            failures.append(name+': private resource directory')
            continue
        if path.is_symlink() or not path.is_file():
            failures.append(name+': unsupported publication file')
            continue
        with path.open('rb') as stream:
            magic=stream.read(8)
        if relative.parts[0]=='photos':
            if name not in SCREENSHOTS or not magic.startswith(b'\xff\xd8\xff') or path.stat().st_size>1024*1024:
                failures.append(name+': expected an approved web screenshot JPEG under 1 MiB')
        elif magic.startswith(b'\xff\xd8\xff'):
            failures.append(name+': screenshot outside the approved documentation set')
        if magic.startswith((b'\x7fELF',b'MZ',b'PK\x03\x04',b'!<arch>\n',b'\x89PNG',b'DDS ',b'\x00PBP')):
            failures.append(name+': binary/archive signature')
        if relative.name.startswith(('generated_unit_','generated_registry')) or relative.name=='recomp_units.hpp':
            failures.append(name+': generated game code')
    for name in ('README.md','INSTALL.md','LICENSE','LICENSES/tiny-AES-Unlicense.txt','dependencies.lock.json',
                 'tools/prepare_install.sh','tools/project_online_sources.py','cmake/install-runtime/CMakeLists.txt'):
        if name not in names:
            failures.append(name+': required public source missing')
    if failures:
        raise ValueError('Public source audit failed:\n'+'\n'.join(failures))
    print('Public source audit PASS: %d source/doc/media files; no private game resources, executables or archives in Git candidates.'%len(names))
    return names


if __name__=='__main__':
    audit()
