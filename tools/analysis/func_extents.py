"""Derive function extents from generated code (one '// asm' comment per PPC instruction).

usage: func_extents.py generated/default > extents.txt   (lines: start end)
"""
import glob
import re
import sys

DEF = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
ASM = re.compile(r"^\t// [a-z]")


def main():
    for path in sorted(glob.glob(f"{sys.argv[1]}/mgr_recomp.*.cpp")):
        start, count = None, 0
        for line in open(path):
            m = DEF.match(line)
            if m:
                if start is not None:
                    print(f"{start:08X} {start + 4 * count:08X}")
                start, count = int(m.group(1), 16), 0
            elif start is not None and ASM.match(line):
                count += 1
        if start is not None:
            print(f"{start:08X} {start + 4 * count:08X}")


if __name__ == "__main__":
    main()
