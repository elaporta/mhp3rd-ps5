#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Acquire and extract only pinned FFmpeg source, compatible with Python 3.9+."""
import hashlib
import json
import pathlib
import tarfile
import urllib.request

from audit_resources import digest
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    spec = json.loads((ROOT / 'dependencies.lock.json').read_text())['libraries']['ffmpeg']
    output = ROOT / 'out/data/dependencies'
    archive = output / ('ffmpeg-' + spec['version'] + '.tar.xz')
    reject_links(archive)
    if not archive.exists():
        # Preserve partial downloads on failure; never replace an existing partial.
        partial = archive.with_suffix('.xz.part')
        reject_links(partial)
        output.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(spec['url'], timeout=30) as response, partial.open('xb') as stream:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                stream.write(chunk)
        if digest(partial) != spec['sha256']:
            raise ValueError('FFmpeg source digest differs; partial preserved')
        partial.rename(archive)
    if digest(archive) != spec['sha256']:
        raise ValueError('Existing FFmpeg source archive differs; preserved')
    prefix = 'ffmpeg-' + spec['version']
    with tarfile.open(archive, 'r:xz') as tar:
        members = tar.getmembers()
        for member in members:
            parts = pathlib.PurePosixPath(member.name).parts
            if not parts or parts[0] != prefix or '..' in parts or not (member.isdir() or member.isfile()):
                raise ValueError('Unsafe FFmpeg source member: ' + member.name)
            reject_links(output / member.name)
        # Check every existing destination before writing any extracted files.
        for member in members:
            path = output / member.name
            if path.exists() and member.isfile():
                expected = hashlib.sha256()
                with tar.extractfile(member) as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                        expected.update(chunk)
                if not path.is_file() or digest(path) != expected.hexdigest():
                    raise ValueError('Different existing FFmpeg source preserved: ' + str(path))
            if path.exists() and member.isdir() and not path.is_dir():
                raise ValueError('Different existing FFmpeg directory preserved: ' + str(path))
        for member in members:
            path = output / member.name
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif not path.exists():
                path.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, path.open('xb') as target:
                    for chunk in iter(lambda: source.read(1024 * 1024), b''):
                        target.write(chunk)
                path.chmod(member.mode & 0o777)
    print('Pinned FFmpeg source ready under out/data; no precompiled codec downloaded.')


if __name__ == '__main__':
    main()
