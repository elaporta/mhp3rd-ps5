#!/usr/bin/env python3
"""Compile and validate the five original pinned Yakumo GE shader variants."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
VARIANTS = (
    ("kGeVertexShader", "ge.vert", []),
    ("kGeFragmentShader", "ge.frag", []),
    ("kGeFragmentShaderPlain", "ge.frag", ["GE_NO_SPECIALIZATION"]),
    ("kGeRawVertexShader", "ge.vert", ["GE_RAW_VERTICES"]),
    ("kGeCheckVertexShader", "ge.vert", ["GE_RAW_VERTICES", "GE_CHECK_DECODE"]),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_shaders(output, root=ROOT):
    upstream = root / "upstream/Yakumo"
    lock = json.loads((root / "dependencies.lock.json").read_text())
    revision = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    if revision != lock["repositories"]["yakumo"]["commit"]:
        raise ValueError("Yakumo revision differs from dependency lock")
    directory = upstream / "profiles/mhp3rd/host/gpu/shaders"
    helper = upstream / "profiles/mhp3rd/tools/embed_shaders.py"
    paths = [directory / "ge.vert", directory / "ge.frag", helper]
    for path in paths:
        relative = path.relative_to(upstream).as_posix()
        original = subprocess.check_output(["git", "-C", str(upstream), "show", f"HEAD:{relative}"])
        if original != path.read_bytes():
            raise ValueError(f"Modified pinned shader input: {relative}")
    compiler = shutil.which("glslangValidator")
    validator = shutil.which("spirv-val")
    if not compiler or not validator:
        raise ValueError("glslangValidator and spirv-val are required")
    inputs = {str(p.relative_to(upstream)): sha(p) for p in paths}
    fingerprint = {"revision": revision, "inputs": inputs,
                   "compiler": subprocess.check_output([compiler, "--version"], text=True),
                   "validator": subprocess.check_output([validator, "--version"], text=True),
                   "compiler_sha256": sha(pathlib.Path(compiler)),
                   "validator_sha256": sha(pathlib.Path(validator)),
                   "wrapper_sha256": sha(pathlib.Path(__file__)), "variants": VARIANTS}
    # JSON normalizes tuples consistently for comparison with the saved manifest.
    fingerprint = json.loads(json.dumps(fingerprint))
    manifest = output / "shader-manifest.json"
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if prior.get("fingerprint") == fingerprint and all(
                (output / name).is_file() and sha(output / name) == digest for name, digest in prior["outputs"].items()):
            print("GE shaders: verified incremental reuse")
            return prior
    output.mkdir(parents=True, exist_ok=True)
    parts, outputs, commands = [], {}, []
    for name, filename, defines in VARIANTS:
        source = directory / filename
        spv = output / f"{name}.spv"
        # Same compiler invocation as the pinned embed_shaders.compile_shader;
        # retain the output and exact replayable command instead of a temporary file.
        command = [compiler, "-V", *[f"-D{d}" for d in defines], str(source), "-o", str(spv)]
        subprocess.run(command, check=True)
        data = spv.read_bytes()
        subprocess.run([validator, "--target-env", "vulkan1.1", str(spv)], check=True)
        words = [int.from_bytes(data[i:i+4], "little") for i in range(0, len(data), 4)]
        body = ",\n    ".join(", ".join(f"0x{word:08x}u" for word in words[i:i+6]) for i in range(0, len(words), 6))
        parts.append(f"// Original {filename}, defines={defines}\nconstexpr std::uint32_t {name}[] = {{\n    {body}\n}};\n")
        outputs[spv.name] = sha(spv)
        commands.append(command)
    include = output / "ge_shaders.inc"
    include.write_text("// Generated from pinned MIT Yakumo shaders; see shader-manifest.json.\n\n" + "\n".join(parts))
    outputs[include.name] = sha(include)
    record = {"fingerprint": fingerprint, "outputs": outputs, "commands": commands,
              "validation": "spirv-val --target-env vulkan1.1 (all five variants)",
              "hardware_verified": False}
    manifest.write_text(json.dumps(record, indent=2) + "\n")
    print("GE shaders: five original variants compiled and SPIR-V validated")
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    compile_shaders(args.output)
