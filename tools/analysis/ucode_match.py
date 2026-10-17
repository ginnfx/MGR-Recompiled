"""Compare microcode the GPU received (Xenia shader storage .xsh) against XDK shader containers.

Reports exact matches and, for misses, the closest container by matching 32-bit words
(reveals whether D3D patches microcode at bind time).
usage: ucode_match.py <4B4E080A.xsh> <shader container dir>
"""
import os
import struct
import sys

def read_xsh(path):
    data = open(path, "rb").read()
    pos, out = 8, []
    while pos + 12 <= len(data):
        h, w = struct.unpack_from("<QI", data, pos)
        count, stype = w & 0x7FFFFFFF, w >> 31
        pos += 12
        out.append((h, stype, data[pos:pos + 4 * count]))
        pos += 4 * count
    return out

def container_ucode(blob):
    flags, vsize, psize, _, _, _, shader_off = struct.unpack_from(">7I", blob, 0)
    phys_off, size = struct.unpack_from(">II", blob, shader_off)
    start = vsize + phys_off
    return ("vs" if flags & 1 else "ps"), blob[start:start + size]

def main():
    stored = read_xsh(sys.argv[1])
    containers = {}
    for root, _, files in os.walk(sys.argv[2]):
        for f in files:
            stage, code = container_ucode(open(os.path.join(root, f), "rb").read())
            containers.setdefault(code, f)
    exact = sum(1 for _, _, code in stored if code in containers)
    print(f"stored shaders: {len(stored)}, exact container matches: {exact}")
    words = {code: set(struct.unpack(f">{len(code)//4}I", code)) for code in containers}
    for h, stype, code in stored[:12]:
        if code in containers:
            print(f"  {h:016x} {'ps' if stype else 'vs'} {len(code)//4:4d}w exact {containers[code]}")
            continue
        sw = struct.unpack(f">{len(code)//4}I", code)
        best = max(containers, key=lambda c: (len(c) == len(code), sum(1 for a, b in zip(sw, struct.unpack(f'>{len(c)//4}I', c)) if a == b)))
        bw = struct.unpack(f">{len(best)//4}I", best)
        same = sum(1 for a, b in zip(sw, bw) if a == b)
        print(f"  {h:016x} {'ps' if stype else 'vs'} {len(code)//4:4d}w best {containers[best]} ({len(best)//4}w) same-position words {same}/{len(sw)}")

if __name__ == "__main__":
    main()


def container_vfetch_words(blob):
    """Word indices of vertex-fetch instructions (from the vertex element table)."""
    _, _, _, _, _, _, shader_off = struct.unpack_from(">7I", blob, 0)
    field18, count = struct.unpack_from(">II", blob, shader_off + 0x18)
    masked = set()
    for i in range(count):
        (elem,) = struct.unpack_from(">I", blob, shader_off + 0x24 + 4 * (field18 + i))
        address = elem & 0xFFF  # VertexElement {address:12, usage:4, index:4} on the BE-decoded word
        masked.update(range(address * 3, address * 3 + 3))
    return masked


def masked_equal(runtime, container, masked):
    a = struct.unpack(f">{len(runtime)//4}I", runtime)
    b = struct.unpack(f">{len(container)//4}I", container)
    return len(a) == len(b) and all(x == y or i in masked for i, (x, y) in enumerate(zip(a, b)))
