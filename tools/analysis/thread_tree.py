"""Print heavy frames of one thread from a macOS `sample` report.

usage: thread_tree.py sample.txt <thread-name-substring> [min_samples] [min_depth]
"""
import re
import sys

def main():
    path, name = sys.argv[1], sys.argv[2]
    min_samples = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    min_depth = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    on = False
    for line in open(path):
        if re.match(r"^\s+\d+ Thread_", line):
            on = name in line
            continue
        if line.startswith(("Total number", "Sort by top")):
            break
        if not on:
            continue
        m = re.match(r"^([\s+!:|]*)(\d+) (.*)", line)
        if m and int(m.group(2)) >= min_samples and len(m.group(1)) >= min_depth:
            sym = re.sub(r"\(in [^)]*\)", "", m.group(3))
            sym = re.sub(r"\s+\+ \d+\s+\[0x[0-9a-f]+\].*", "", sym)
            print(f"{len(m.group(1)):3d} {m.group(2):>5} {sym[:150]}")

if __name__ == "__main__":
    main()
