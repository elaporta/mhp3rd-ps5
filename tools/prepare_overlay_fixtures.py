#!/usr/bin/env python3
"""Generate two public synthetic overlay corpora with the pinned host recompiler."""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = 0x08900000
HOST_IMPORT = 0x08806000


def fnv(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def elf(image):
    header = bytearray(84)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", header, 16, 2, 8, 1, BASE + 64, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    # Only executable code is mapped by the host generator. Metadata and data
    # are still present in the synthetic image, as in a real overlay container.
    code_size = struct.unpack_from("<I", image, 12)[0]
    struct.pack_into("<IIIIIIII", header, 52, 1, 84 + 64, BASE + 64, BASE + 64, code_size, code_size, 5, 4)
    return bytes(header) + image


def prepare(output):
    revision = subprocess.check_output(["git", "-C", str(ROOT / "upstream/Yakumo"), "rev-parse", "HEAD"], text=True).strip()
    expected = json.loads((ROOT / "dependencies.lock.json").read_text())["repositories"]["yakumo"]["commit"]
    if revision != expected:
        raise ValueError("Wrong Yakumo revision")
    compiler = ROOT / "out/framework/psp_recomp"
    output.mkdir(parents=True, exist_ok=True)
    modules = []
    for name, immediate, tag in (("a", 11, 101), ("b", 22, 202)):
        prefix = "synthetic_" + name
        words = [0x03E07821, 0x24840000 | immediate, 0x0C000000 | (HOST_IMPORT >> 2), 0,
                 0xACA20000, 0x01E00008, 0x24030000 | tag]
        if name == "a":
            words += [0x24020063, 0x03E00008, 0]
        code = struct.pack("<" + "I" * len(words), *words)
        header = bytearray(64)
        header[:4] = b"MWo3"
        struct.pack_into("<IIIIIII", header, 4, tag, BASE, len(code), 128 - 64 - len(code), 0, BASE + 128, BASE + 128)
        header[32:32 + len(prefix)] = prefix.encode()
        image = bytes(header) + code + bytes(128 - 64 - len(code))
        directory = output / name
        directory.mkdir(exist_ok=True)
        fixture = directory / "synthetic.elf"
        content = elf(image)
        if not fixture.exists() or fixture.read_bytes() != content:
            fixture.write_bytes(content)
        generated = directory / "generated"
        command = [str(compiler), str(fixture), "--auto", str(generated), hex(BASE), "4096", "--prefix", prefix]
        # Do not rewrite unchanged generated units: preserve incremental dependency state.
        signature = hashlib.sha256(content + compiler.read_bytes()).hexdigest()
        stamp = directory / "generation.sha256"
        if not stamp.exists() or stamp.read_text() != signature or not (generated / f"{prefix}_registry.cpp").exists():
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
            (directory / "generation.log").write_text(result.stdout)
            stamp.write_text(signature)
        identity = fnv(image[:64 + len(code)])
        # Metadata/entry shape follows the original MIT overlay_module.cpp.in;
        # this independent GPL wrapper registers only original generated code.
        wrapper = f'''// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic wrapper using Yakumo's original MIT overlay_module.hpp ABI.
#define MHP3RD_OVERLAY_MODULE 1
#include "overlay_module.hpp"
#include "psprecomp/runtime.hpp"
namespace psprecomp {{ void register_{prefix}_functions(Runtime&); }}
namespace {{
constexpr mhp3rd::OverlayModuleInfo info{{mhp3rd::kOverlayAbiVersion, 0x{BASE:08X}u, 128u, {len(code)}u, {identity}ull, "{prefix}"}};
}}
const mhp3rd::OverlayModuleInfo* mhp3rd_overlay_info() {{ return &info; }}
void mhp3rd_register_overlay(psprecomp::Runtime& runtime) {{
    psprecomp::register_{prefix}_functions(runtime);
}}
'''
        wrapper_path = directory / "module.cpp"
        if not wrapper_path.exists() or wrapper_path.read_text() != wrapper:
            wrapper_path.write_text(wrapper)
        modules.append({"name": prefix, "code_size": len(code), "fnv1a64": identity, "command": command,
                        "image": list(image), "expected_v0_input7": 1018 if name == "a" else 1029, "expected_v1": tag})
    include = "// Public synthetic images only; no game data.\n"
    for module in modules:
        include += f'constexpr unsigned char image_{module["name"][-1]}[] = {{' + ",".join(map(str, module["image"])) + "};\n"
    path = output / "overlay_images.inc"
    if not path.exists() or path.read_text() != include:
        path.write_text(include)
    files = {p.relative_to(output).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(output.rglob("*")) if p.is_file() and p.name != "fixture-manifest.json"}
    (output / "fixture-manifest.json").write_text(json.dumps({"yakumo": revision, "synthetic_only": True,
        "generator_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "modules": modules, "files": files}, indent=2) + "\n")
    print("Two synthetic overlay corpora generated/verified using pinned psp_recomp")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    prepare(parser.parse_args().output)
