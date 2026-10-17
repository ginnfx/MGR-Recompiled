"""Summarize [mgr-fps] lines: first 45 lines ~ boot/title, last 30 ~ gameplay."""
import re
import statistics
import sys

TITLE = slice(35, 50)
GAMEPLAY = slice(-30, None)

rows = [(float(m[1]), float(m[2])) for m in re.finditer(r"\[mgr-fps\] ([\d.]+) worst ([\d.]+) ms", open(sys.argv[1]).read())]
if not rows:
    sys.exit(f"no fps lines in {sys.argv[1]}")
for name, sel in (("title", TITLE), ("gameplay", GAMEPLAY)):
    part = rows[sel]
    if not part:
        continue
    fps = [r[0] for r in part]
    worst = [r[1] for r in part]
    print(f"{name:9s} median {statistics.median(fps):5.1f} fps  min {min(fps):5.1f}  worst-frame max {max(worst):6.1f} ms  median {statistics.median(worst):5.1f} ms")
print(f"crash: {'FATAL' in open(sys.argv[1]).read() or 'Abort' in open(sys.argv[1]).read()}")
