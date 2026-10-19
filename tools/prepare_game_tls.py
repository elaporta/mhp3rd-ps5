#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project pinned LLVM emutls with one key-cleanup grace round for C++ TLS."""
import argparse
import hashlib
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def prepare(output):
    from prepare_textures import reject_links
    reject_links(output.absolute())
    if not output.resolve().is_relative_to((ROOT / 'out/data').resolve()):
        raise ValueError('Generated runtime sources must stay under out/data')
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
    spec = lock['runtime_adapters']['game_emutls']
    source = ROOT / lock['repositories']['sdk']['path'] / spec['file']
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != spec['sha256']:
        raise ValueError('Pinned emutls source differs')
    text = raw.decode()
    anchor = '#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 0'
    if text.count(anchor) != 1:
        raise ValueError('Emutls lifetime anchor differs')
    # pthread key order does not order compiler-rt storage after C++ objects.
    # A grace round keeps the storage alive while the SDK drains object dtors.
    text = text.replace(anchor, '// PS5: C++ thread_local destructors need live emulated storage.\n'
                               '#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 1')
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and output.read_bytes() != text.encode():
        raise ValueError('Different generated TLS source preserved; choose a new build directory')
    if not output.exists():
        with output.open('xb') as stream:
            stream.write(text.encode())
    report = dict(source=str(source.relative_to(ROOT)), source_sha256=spec['sha256'],
                  license='Apache-2.0 WITH LLVM-exception', cleanup_grace_rounds=1,
                  output_sha256=hashlib.sha256(output.read_bytes()).hexdigest())
    from prepare_textures import write_text
    write_text(output.with_suffix('.json'), json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    print(json.dumps(prepare(parser.parse_args().output), indent=2))
