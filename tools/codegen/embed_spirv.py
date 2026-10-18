"""Embed build-generated SPIR-V; no runtime dependency on the build directory."""
import argparse
import pathlib
import struct

parser = argparse.ArgumentParser()
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("inputs", nargs="+")
args = parser.parse_args()
lines = ["#pragma once", "#include <cstdint>", "namespace mgr::gpu::metal::shaders {"]
for item in args.inputs:
    name, path = item.split("=", 1)
    if not name.isidentifier():
        raise ValueError("Invalid shader identifier")
    data = pathlib.Path(path).read_bytes()
    if len(data) % 4 or len(data) < 20:
        raise ValueError("Invalid SPIR-V size")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError("Invalid SPIR-V magic")
    lines += [f"inline constexpr uint32_t {name}[] = {{"]
    for i in range(0, len(words), 8):
        lines += ["  " + ", ".join(f"0x{w:08x}" for w in words[i:i+8]) + ","]
    lines += ["};"]
lines += ["}"]
args.output.write_text("\n".join(lines) + "\n")
