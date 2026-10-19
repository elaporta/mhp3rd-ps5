#!/usr/bin/env python3
"""Read-only ISO/resource inspection. Outputs metadata, never commercial file bytes."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXPECTED_EBOOT = "79e25f3512d56e0f7bf5c48351d7d0d255269675ffc8811ac5599322bb66945e"


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


class Iso:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.size = self.path.stat().st_size
        self.file = self.path.open("rb")
        self.entries = {}
        try:
            pvd = self.read(16 * 2048, 2048)
            if pvd[:7] != b"\x01CD001\x01":
                raise ValueError("Not an ISO9660 primary volume")
            if struct.unpack_from("<H", pvd, 128)[0] != 2048:
                raise ValueError("Unsupported ISO sector size")
            self._directory(*struct.unpack_from("<I4xI", pvd, 158), "", set(), 0)
        except BaseException:
            self.file.close()
            raise

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError("ISO extent outside file")
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise ValueError("Truncated ISO")
        return data

    def _directory(self, lba, size, parent, seen, depth):
        if depth > 12 or lba in seen or size > 16 * 1024 * 1024:
            raise ValueError("Invalid ISO directory graph")
        seen = seen | {lba}
        data = self.read(lba * 2048, size)
        at = 0
        while at < len(data):
            length = data[at]
            if not length:
                at = (at // 2048 + 1) * 2048
                continue
            if length < 34 or at + length > len(data):
                raise ValueError("Malformed ISO directory record")
            record = data[at:at + length]
            at += length
            name_len = record[32]
            if 33 + name_len > length:
                raise ValueError("Malformed ISO filename")
            name_bytes = record[33:33 + name_len]
            if name_bytes in (b"\0", b"\1"):
                continue
            name = name_bytes.decode("ascii").split(";")[0]
            if name in (".", "..") or "/" in name or "\\" in name:
                raise ValueError("Unsafe ISO filename")
            block, count = struct.unpack_from("<I4xI", record, 2)
            if block * 2048 + count > self.size:
                raise ValueError("ISO file extent outside image")
            key = parent + "/" + name
            if key in self.entries:
                raise ValueError("Duplicate ISO path")
            self.entries[key] = {"offset": block * 2048, "size": count,
                                 "directory": bool(record[25] & 2)}
            if record[25] & 2:
                self._directory(block, count, key, seen, depth + 1)

    def contents(self, key):
        e = self.entries[key]
        return self.read(e["offset"], e["size"])

    def close(self):
        self.file.close()


def sfo_strings(data):
    if len(data) < 20 or data[:4] != b"\0PSF":
        raise ValueError("Invalid PARAM.SFO")
    keys, values, count = struct.unpack_from("<III", data, 8)
    if count > 1024 or 20 + count * 16 > len(data):
        raise ValueError("Invalid SFO table")
    result = {}
    for i in range(count):
        key, fmt, length, maximum, offset = struct.unpack_from("<HHIII", data, 20 + i * 16)
        if keys + key >= len(data) or values + offset + length > len(data) or length > maximum:
            raise ValueError("Invalid SFO range")
        name = data[keys + key:].split(b"\0", 1)[0].decode("utf-8")
        if fmt == 0x204:
            result[name] = data[values + offset:values + offset + length].rstrip(b"\0").decode("utf-8")
    return result


def inspect_iso(path):
    iso = Iso(path)
    try:
        meta = sfo_strings(iso.contents("/PSP_GAME/PARAM.SFO"))
        encrypted = hashlib.sha256(iso.contents("/PSP_GAME/SYSDIR/EBOOT.BIN")).hexdigest()
        return {"filename": iso.path.name, "bytes": iso.size, "sha256": digest(iso.path),
                "sfo": meta, "eboot_sha256": encrypted,
                "upstream_executable_match": encrypted == EXPECTED_EBOOT,
                "identity_match": meta.get("DISC_ID") == "NPJB40001", "entries": iso.entries,
                "translation_version": "User-supplied label v6.3.0; not independently authenticated",
                "overlay_compatibility": "Not yet compared to original overlay hashes"}
    finally:
        iso.close()


def inspect_textures(root):
    root = pathlib.Path(root).resolve()
    entries = {}
    section = ""
    for raw in (root / "textures.ini").read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("["):
            section = line[1:line.index("]")].lower()
        elif section == "hashes" and "=" in line:
            key, value = (p.strip() for p in line.split("=", 1))
            if re.fullmatch(r"[0-9a-fA-F]{1,24}(?:_\d+)?", key):
                parts = key.split("_", 1)
                if len(parts) == 2 and int(parts[1]) != 0:
                    continue
                digits = parts[0]
                canonical = (int(digits[:16], 16), int(digits[16:] or "0", 16))
                entries[canonical] = value.replace("\\", "/")
    paths = list(root.rglob("*"))
    links = {p.relative_to(root) for p in paths if p.is_symlink()}
    files = [p for p in paths if p.is_file()]
    exact = {os.path.normcase(str(p.relative_to(root))) for p in files}
    folded = {p.relative_to(root).as_posix().lower() for p in files}
    missing, case_fallback, unsafe = [], [], []
    for name in sorted(set(entries.values()) - {""}):
        target = pathlib.Path(os.path.abspath(root / name))
        if pathlib.PurePosixPath(name).is_absolute() or not target.is_relative_to(root):
            unsafe.append(name)
        elif any(part in links for part in (pathlib.Path(name), *pathlib.Path(name).parents)):
            unsafe.append(name)
        elif os.path.normcase(str(target.relative_to(root))) not in exact:
            (case_fallback if name.lower() in folded else missing).append(name)
    return {"file_count": len(files), "bytes": sum(p.stat().st_size for p in files),
            "png_count": sum(p.suffix.lower() == ".png" for p in files),
            "effective_keys": len(entries), "disabled_keys": sum(not v for v in entries.values()),
            "missing": missing, "case_fallback": case_fallback, "unsafe": unsafe,
            "ini_sha256": digest(root / "textures.ini")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "out/data/audit")
    parser.add_argument("--iso", type=pathlib.Path, required=True)
    parser.add_argument("--textures", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to((ROOT / "out/data").resolve()):
        parser.error("Audit output must stay under ignored out/data")
    if not args.iso.is_file() or not args.textures.is_dir():
        parser.error("--iso must name a file and --textures must name a directory")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"game": inspect_iso(args.iso), "textures": inspect_textures(args.textures)}
    (args.output / "resources.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"game_identity": report["game"]["identity_match"],
                      "eboot_match": report["game"]["upstream_executable_match"],
                      "missing_textures": len(report["textures"]["missing"])}))
    if not report["game"]["identity_match"] or not report["game"]["upstream_executable_match"]:
        raise SystemExit("Unsupported image: originals preserved; inspect resources.json")


if __name__ == "__main__":
    main()
