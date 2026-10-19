#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared source-build I/O: pinned downloads, confined extraction and retained logs."""
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import tarfile
import time
import urllib.request
import uuid
import zipfile

from audit_resources import digest
from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]
DATA = ROOT / 'out/data'


def private(path):
    path = pathlib.Path(os.path.abspath(path))
    reject_links(path)
    if path == DATA or not path.resolve().is_relative_to(DATA.resolve()):
        raise ValueError('Build output must be a child of out/data: ' + str(path))
    return path


def save_json(path, value):
    private(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()
    temporary = path.with_name(path.name + '.new-' + uuid.uuid4().hex)
    with temporary.open('xb') as stream:
        stream.write(raw)
    os.replace(temporary, path)


def run(command, log, cwd=ROOT, env=None):
    private(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    print('Running: ' + ' '.join(map(str, command[:5])) + ' (log: ' + str(log.relative_to(ROOT)) + ')', flush=True)
    process_env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', GIT_OPTIONAL_LOCKS='0')
    if env:
        process_env.update({str(k): str(v) for k, v in env.items()})
    with log.open('a') as stream:
        begin=stream.tell()
        stream.write('\nCOMMAND ' + repr(list(map(str, command))) + '\n')
        stream.flush()
        with subprocess.Popen(list(map(str, command)), cwd=cwd, env=process_env,
                              stdout=stream, stderr=subprocess.STDOUT,start_new_session=(os.name=='posix')) as process:
            try:
                while True:
                    try:
                        code = process.wait(timeout=20)
                        break
                    except subprocess.TimeoutExpired:
                        print('  Still working (%ds); log: %s' % (time.monotonic()-started, log.relative_to(ROOT)), flush=True)
            except BaseException:
                if process.poll() is None:
                    if os.name=='posix': os.killpg(process.pid,signal.SIGTERM)
                    else: process.terminate()
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        if os.name=='posix': os.killpg(process.pid,signal.SIGKILL)
                        else: process.kill()
                        process.wait()
                raise

    if code:
        with log.open('rb') as errors:
            errors.seek(begin)
            print(errors.read().decode(errors='replace')[-7000:],flush=True)
        raise RuntimeError('Command failed (%d); partial outputs and log preserved: %s' % (code, log))


def download(spec, path=None):
    path = private(path or DATA / 'downloads' / spec.get('filename', spec['url'].rsplit('/', 1)[1]))
    if path.exists():
        if digest(path) != spec['sha256']:
            raise ValueError('Different cached source preserved: ' + str(path))
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.part-' + uuid.uuid4().hex)
    print('Downloading pinned public source: ' + spec['url'], flush=True)
    sha = hashlib.sha256()
    with urllib.request.urlopen(spec['url'], timeout=60) as source, partial.open('xb') as target:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            target.write(block)
            sha.update(block)
    if sha.hexdigest() != spec['sha256']:
        raise ValueError('Source checksum differs; partial retained: ' + str(partial))
    if 'bytes' in spec and partial.stat().st_size != spec['bytes']:
        raise ValueError('Source size differs; partial retained: ' + str(partial))
    partial.rename(path)
    return path


def extract(archive, destination, stamp, include=(), exclude=()):
    destination = private(destination)
    stamp = json.loads(json.dumps(stamp))
    receipt = destination / '.source-input.json'
    if receipt.is_file():
        if json.loads(receipt.read_text()) != stamp:
            raise ValueError('Different source export preserved: ' + str(destination))
        return destination
    destination.mkdir(parents=True, exist_ok=True)
    # Validate existing links once. Source archives can contain thousands of files;
    # resolving every ancestor of every file makes Windows-mounted WSL prohibitively slow.
    links = {p.relative_to(destination) for p in destination.rglob('*') if p.is_symlink()}
    def target_for(name):
        parts = pathlib.PurePosixPath(name)
        if parts.is_absolute() or '..' in parts.parts or '\\' in name or ':' in name:
            raise ValueError('Unsafe source archive member: ' + name)
        relative = pathlib.Path(*parts.parts)
        if relative in links or any(parent in links for parent in relative.parents):
            raise ValueError('Existing symlink would redirect source extraction')
        return destination / relative
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for info in source.infolist():
                target = target_for(info.filename)
                if ((info.external_attr >> 16) & 0o170000) == 0o120000:
                    raise ValueError('Unsupported ZIP source symlink')
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    data = source.read(info)
                    if target.exists():
                        if target.read_bytes() != data:
                            raise ValueError('Different extracted source preserved: ' + str(target))
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with target.open('xb') as stream:
                            stream.write(data)
    else:
        with tarfile.open(archive) as source:
            members = source.getmembers()
            if include:
                members = [item for item in members if len(pathlib.PurePosixPath(item.name).parts) <= 1 or
                           any('/'.join(pathlib.PurePosixPath(item.name).parts[1:]).startswith(prefix) for prefix in include)]
            if exclude:
                members = [item for item in members if not any(
                    '/'.join(pathlib.PurePosixPath(item.name).parts[1:]).startswith(prefix) for prefix in exclude)]
            for item in members:
                target = target_for(item.name)
                if item.issym():
                    link = pathlib.Path(os.path.abspath(target.parent / item.linkname))
                    if not link.is_relative_to(destination):
                        raise ValueError('Source symlink escapes its export')
                elif not (item.isdir() or item.isfile()):
                    raise ValueError('Unsupported source archive member: ' + item.name)
            # Regular members first. Archive links cannot redirect subsequent writes.
            for item in members:
                if item.issym():
                    continue
                target = target_for(item.name)
                if target.relative_to(destination) in links:
                    raise ValueError('Existing source symlink preserved: ' + str(target))
                if item.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(item) as stream:
                        if target.exists():
                            sha = hashlib.sha256()
                            for block in iter(lambda: stream.read(1024*1024), b''):
                                sha.update(block)
                            if digest(target) != sha.hexdigest():
                                raise ValueError('Different extracted source preserved: ' + str(target))
                        else:
                            with target.open('xb') as output:
                                shutil.copyfileobj(stream, output)
                            target.chmod(item.mode & 0o777)
                            os.utime(target,(item.mtime,item.mtime))
            for item in members:
                if item.issym():
                    target = destination / item.name
                    if not target.is_symlink():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.symlink_to(item.linkname)
                    elif os.readlink(target) != item.linkname:
                        raise ValueError('Different source symlink preserved')
    save_json(receipt, stamp)
    return destination


def export_git(name, destination):
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())['repositories'][name]
    source = ROOT / lock['path']
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != lock['commit']:
        raise ValueError('Pinned source revision differs: ' + name)
    destination = private(destination)
    receipt = destination / '.source-input.json'
    if receipt.is_file():
        return extract(DATA / 'unused', destination, {'repository': name, 'commit': actual})
    archive = private(destination.with_suffix('.source.tar'))
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        with archive.open('xb') as stream:
            subprocess.run(['git', '-C', str(source), 'archive', actual], stdout=stream, check=True)
    return extract(archive, destination, {'repository': name, 'commit': actual})


def complete(receipt, artifacts, fingerprint):
    if not receipt.exists():
        return False
    record = json.loads(receipt.read_text())
    if record.get('fingerprint') != fingerprint:
        raise ValueError('Existing component belongs to another source build: ' + str(receipt))
    for path in artifacts:
        if not path.is_file() or record.get('files', {}).get(str(path.relative_to(DATA))) != digest(path):
            raise ValueError('Completed component changed; preserve and inspect: ' + str(path))
    return True


def record(receipt, artifacts, fingerprint):
    save_json(receipt, {'fingerprint': fingerprint,
                       'files': {str(path.relative_to(DATA)): digest(path) for path in artifacts}})
