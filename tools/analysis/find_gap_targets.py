"""Find addresses in gaps between known functions that the guest can reach indirectly.

Sources: (a) any aligned data word pointing into a gap, (b) lis rX,hi + addi/ori rY,rX,lo
pairs in code that build a gap address. A gap target must start at a non-zero word.
A lis/addi address that feeds lwzx + bctr is a switch jump table (data inside its
function), never an entry point, so it is skipped.

usage: find_gap_targets.py image.bin extents.txt
"""
import struct
import sys
from bisect import bisect_right

IMAGE_BASE = 0x82000000
CODE_START, CODE_END = 0x82270000, 0x831FD75C
LIS_WINDOW = 8  # instructions after lis to look for the low half
TABLE_WINDOW = 12  # instructions after the low half to look for lwzx ... bctr
BCTR = 0x4E800420


def load_extents(path):
    ext = [tuple(int(x, 16) for x in line.split()) for line in open(path)]
    return [s for s, _ in ext], ext


def in_gap(starts, ext, p):
    i = bisect_right(starts, p) - 1
    return i < 0 or not (ext[i][0] <= p < ext[i][1])


def is_lwzx_from(w, reg):
    return w >> 26 == 31 and (w >> 1) & 0x3FF == 23 and reg in ((w >> 16) & 0x1F, (w >> 11) & 0x1F)


def feeds_jump_table(words, reg):
    loads = [i for i, w in enumerate(words) if is_lwzx_from(w, reg)]
    return bool(loads) and BCTR in words[loads[0] + 1 :]


def code_built_addresses(img):
    n = (CODE_END - CODE_START) // 4
    words = struct.unpack_from(f">{n}I", img, CODE_START - IMAGE_BASE)
    for i, w in enumerate(words):
        if w >> 26 != 15 or (w >> 16) & 0x1F != 0:  # addis rD,0,imm == lis
            continue
        rd, hi = (w >> 21) & 0x1F, w & 0xFFFF
        for j in range(i + 1, min(i + 1 + LIS_WINDOW, n)):
            w2 = words[j]
            op, ra = w2 >> 26, (w2 >> 16) & 0x1F
            if ra != rd or op not in (14, 24):  # addi / ori
                continue
            if op == 14 and feeds_jump_table(words[j + 1 : j + 1 + TABLE_WINDOW], (w2 >> 21) & 0x1F):
                break
            lo = w2 & 0xFFFF
            if op == 14 and lo & 0x8000:
                lo -= 0x10000
            yield ((hi << 16) + lo) & 0xFFFFFFFF, CODE_START + 4 * i
            break


def main():
    img = open(sys.argv[1], "rb").read()
    starts, ext = load_extents(sys.argv[2])
    known = set(starts)
    found = {}

    def consider(p, src):
        if CODE_START <= p < CODE_END and not p & 3 and p not in known and in_gap(starts, ext, p):
            if struct.unpack_from(">I", img, p - IMAGE_BASE)[0] != 0:
                found.setdefault(p, src)

    for off in range(0, len(img) - 4, 4):
        src = IMAGE_BASE + off
        if not (CODE_START <= src < CODE_END):
            consider(struct.unpack_from(">I", img, off)[0], src)
    for p, src in code_built_addresses(img):
        consider(p, src)
    for p in sorted(found):
        print(f"0x{p:08X} ref@0x{found[p]:08X}")
    print(f"# {len(found)} gap targets", file=sys.stderr)


if __name__ == "__main__":
    main()
