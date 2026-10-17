"""Find code pointers in guest data that target unregistered, function-like addresses.

usage: find_missing_funcs.py image.bin mgr_register.cpp
"""
import re
import struct
import sys
from bisect import bisect_right

IMAGE_BASE = 0x82000000
CODE_START, CODE_END = 0x82270000, 0x831FD75C
BLR = 0x4E800020
MFLR_R12 = 0x7D8802A6


def word(img, addr):
    off = addr - IMAGE_BASE
    return struct.unpack_from(">I", img, off)[0]


def looks_like_start(img, addr):
    prev = word(img, addr - 4)
    ins = word(img, addr)
    prev_is_end = prev == BLR or prev == 0 or (prev >> 26) == 18  # blr, pad, b
    ins_is_prologue = ins == MFLR_R12 or (ins >> 16) == 0x9421  # mflr r12 / stwu r1
    return ins != 0 and (prev_is_end or ins_is_prologue)


def main():
    img = open(sys.argv[1], "rb").read()
    reg = sorted(int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)", open(sys.argv[2]).read()))
    known = set(reg)
    found = {}
    for off in range(0, len(img) - 4, 4):
        src = IMAGE_BASE + off
        if CODE_START <= src < CODE_END:
            continue
        p = struct.unpack_from(">I", img, off)[0]
        if not (CODE_START <= p < CODE_END) or p & 3 or p in known:
            continue
        if looks_like_start(img, p):
            found.setdefault(p, src)
    for p in sorted(found):
        i = bisect_right(reg, p) - 1
        print(f"0x{p:08X} ref@0x{found[p]:08X} after sub_{reg[i]:08X}")
    print(f"# {len(found)} candidates", file=sys.stderr)


if __name__ == "__main__":
    main()
