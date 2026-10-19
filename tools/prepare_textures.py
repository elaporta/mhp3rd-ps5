#!/usr/bin/env python3
"""Prepare an immutable-input texture copy; never edit the source pack."""
import argparse
import json
import pathlib
import shutil
from audit_resources import inspect_textures, digest, ROOT


def write_text(path, text):
    reject_links(path)
    if path.exists():
        if path.read_bytes() != text.encode('utf-8'):
            raise ValueError('Different existing metadata preserved: ' + str(path))
        return
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def reject_links(path):
    for part in (path, *path.parents):
        if part.is_symlink():
            raise ValueError(f"Symlink in preparation path: {part}")


def prepare(source, destination):
    reject_links(source.absolute())
    reject_links(destination.absolute())
    source, destination = source.resolve(), destination.resolve()
    if source == destination or source.is_relative_to(destination) or destination.is_relative_to(source):
        raise ValueError("Source and output trees must be separate")
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Unexpected source symlink: {path}")
    report = inspect_textures(source)
    if report["unsafe"]:
        raise ValueError("Pack contains paths escaping its root")
    destination.mkdir(parents=True, exist_ok=True)
    for path in destination.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Unexpected output symlink: {path}")
    copied = 0
    manifest = {}
    # Preserve the INI exactly; copy supported images. Do not include editing projects.
    for path in sorted(source.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".png", ".ini"):
            continue
        if path.is_symlink():
            raise ValueError(f"Unexpected source symlink: {path.name}")
        out = destination / path.relative_to(source)
        reject_links(out)
        manifest[path.relative_to(source).as_posix()] = digest(path)
        if out.exists():
            if not out.is_file() or digest(out) != digest(path):
                raise ValueError(f"Existing output differs: {out}; use a fresh output directory")
            continue
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, out)
        copied += 1
    write_text(destination / "preparation-report.json", json.dumps(report, indent=2) + "\n")
    readme = source.parent / "README.md"
    if readme.is_file():
        reject_links(readme)
        reject_links(destination / "PACK-README.md")
        target = destination / "PACK-README.md"
        if target.exists() and digest(target) != digest(readme):
            raise ValueError('Different author notice preserved: ' + str(target))
        if not target.exists():
            shutil.copyfile(readme, target)
        manifest["PACK-README.md"] = digest(readme)
    case_index = ''.join(name + '\n' for name in sorted(manifest) if name.lower().endswith('.png'))
    write_text(destination / 'runtime-case-index.txt', case_index)
    manifest['runtime-case-index.txt'] = digest(destination / 'runtime-case-index.txt')
    for name, expected in manifest.items():
        if digest(destination / name) != expected:
            raise ValueError(f"Prepared bytes differ: {name}")
    write_text(destination / "runtime-manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"Copied {copied} files; missing mappings: {len(report['missing'])}; no mappings rewritten")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, required=True,
                        help="Author-supplied NPJB40001 texture directory")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "out/data/textures/NPJB40001")
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to((ROOT / "out/data").resolve()):
        parser.error("Texture assets must stay under ignored out/data")
    prepare(args.source, args.output)
