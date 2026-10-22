"""Compare post-processing A/B runs made by scripts/bench-postfx.sh.

usage: postfx_summary.py <base tag> <variant tag> [prof dir]

Uses the last WINDOWS report windows of each log (gameplay at the end of the
scripted route). Level 1 logs give guest fps and GPU busy; level 2 logs give
the per-frame zone split, per guest shader rows and OT render-callback CPU time.
Level 2 splits Metal encoders, so compare its numbers only with level 2.
"""
import collections
import os
import re
import statistics
import sys

WINDOWS = 4
GAMEPLAY_FPS_LINES = 30
TOP_EFFECTS = 16

FPS = re.compile(r"\[mgr-fps\] ([\d.]+) worst ([\d.]+) ms")
GPU_TIME = re.compile(r"\[mgr-gpu-time\] \d+ frames: GPU busy median ([\d.]+) / P95 ([\d.]+)")
PASS_SPLIT = re.compile(r"\[mgr-gpu-pass\] per frame: passes ([\d.]+) ms, dispatches ([\d.]+) ms, "
                        r"copies ([\d.]+) ms, uploads ([\d.]+) ms, outside zones ([\d.]+) ms")
EFFECT = re.compile(r"\[mgr-gpu-effect\]\s+([\d.]+)\s*\|(.*)\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*"
                    r"\(\s*([\d.]+),\s*([\d.]+)\)\s*\|\s*(\S+)\s+(\S.*)$")
OT_HEADER = re.compile(r"\[mgr-ot\] \d+ executions: ([\d.]+) ms")
OT_ROW = re.compile(r"\[mgr-ot\]\s+([\d.]+) ms\s+x\s*([\d.]+)\s+prio (\S+)\s+([0-9A-F]{8})")
CATEGORIES = ("pass", "resolve", "texload", "rt-xfer", "other")


def read(path):
    try:
        return open(path, errors="replace").read().splitlines()
    except OSError:
        sys.exit(f"missing {path}: run scripts/bench-postfx.sh first")


def level1(lines):
    fps = [float(m[1]) for m in map(FPS.search, lines) if m][-GAMEPLAY_FPS_LINES:]
    gpu = [(float(m[1]), float(m[2])) for m in map(GPU_TIME.search, lines) if m][-WINDOWS:]
    return {
        "fps median": statistics.median(fps) if fps else float("nan"),
        "GPU busy median ms": statistics.mean(g[0] for g in gpu) if gpu else float("nan"),
        "GPU busy P95 ms": statistics.mean(g[1] for g in gpu) if gpu else float("nan"),
    }


def level2(lines):
    """Per-window blocks: zone split, effect rows, OT rows."""
    splits, effect_windows, ot_windows, ot_totals = [], [], [], []
    current_effects = current_ot = None
    for line in lines:
        if m := PASS_SPLIT.search(line):
            splits.append(tuple(float(x) for x in m.groups()))
        elif "[mgr-gpu-effect] per frame" in line:
            current_effects = {}
            effect_windows.append(current_effects)
        elif current_effects is not None and (m := EFFECT.search(line)):
            cells = [c.strip() for c in m[2].split("|")]
            row = {"gpu": float(m[1]), "draws": float(m[3]), "cp": float(m[4]),
                   "msaa": m[7]}
            for name, cell in zip(CATEGORIES, cells):
                ms, count = cell.split("/")
                row[name + " ms"], row[name + " n"] = float(ms), float(count)
            current_effects[m[8]] = row
        elif m := OT_HEADER.search(line):
            ot_totals.append(float(m[1]))
            current_ot = {}
            ot_windows.append(current_ot)
        elif current_ot is not None and (m := OT_ROW.search(line)):
            current_ot[f"{m[4]} ({m[3]})"] = float(m[1])
    return splits[-WINDOWS:], effect_windows[-WINDOWS:], ot_windows[-WINDOWS:], ot_totals[-WINDOWS:]


def average_rows(windows):
    totals = collections.defaultdict(lambda: collections.defaultdict(float))
    for window in windows:
        for name, row in window.items():
            for key, value in row.items():
                if isinstance(value, str):
                    totals[name][key] = value
                else:
                    totals[name][key] += value / len(windows)
    return totals


def column_sum(rows, key):
    return sum(row.get(key, 0.0) for row in rows.values())


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    base, variant = sys.argv[1], sys.argv[2]
    prof = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        os.path.dirname(__file__), "..", "..", "private", "prof")
    runs = {}
    for tag in (base, variant):
        l1 = level1(read(os.path.join(prof, f"postfx-{tag}-l1.txt")))
        splits, effects, ot, ot_total = level2(read(os.path.join(prof, f"postfx-{tag}-l2.txt")))
        effect_rows = average_rows(effects) if effects else {}
        summary = dict(l1)
        if splits:
            for i, name in enumerate(("passes", "dispatches", "copies", "uploads", "outside")):
                summary[f"L2 {name} ms"] = statistics.mean(s[i] for s in splits)
        for category in CATEGORIES:
            summary[f"L2 {category} n/frame"] = column_sum(effect_rows, category + " n")
        summary["L2 CP draw+resolve ms"] = column_sum(effect_rows, "cp")
        summary["L2 OT callbacks ms"] = statistics.mean(ot_total) if ot_total else float("nan")
        runs[tag] = (summary, effect_rows, average_rows([{k: {"ms": v} for k, v in w.items()}
                                                          for w in ot]) if ot else {})
    print(f"{'metric':28s} {base:>12s} {variant:>12s} {'delta':>9s}")
    for key in runs[base][0]:
        a, b = runs[base][0][key], runs[variant][0].get(key, float("nan"))
        print(f"{key:28s} {a:12.2f} {b:12.2f} {b - a:+9.2f}")
    print(f"\nper guest shader (level 2, GPU ms / resolves per frame), top {TOP_EFFECTS} of {base}:")
    base_rows, variant_rows = runs[base][1], runs[variant][1]
    for name in sorted(base_rows, key=lambda n: -base_rows[n]["gpu"])[:TOP_EFFECTS]:
        a, b = base_rows[name], variant_rows.get(name, {})
        print(f"  {a['gpu']:6.2f} -> {b.get('gpu', 0.0):6.2f} ms  resolves {a['resolve n']:4.1f} -> "
              f"{b.get('resolve n', 0.0):4.1f}  CP {a['cp']:5.2f} -> {b.get('cp', 0.0):5.2f}  "
              f"msaa {a.get('msaa', '-'):5s} {name}")
    print("\nOT render callbacks (render-thread CPU ms per execution):")
    base_ot, variant_ot = runs[base][2], runs[variant][2]
    for name in sorted(base_ot, key=lambda n: -base_ot[n]["ms"])[:TOP_EFFECTS]:
        print(f"  {base_ot[name]['ms']:7.3f} -> {variant_ot.get(name, {}).get('ms', 0.0):7.3f}  {name}")


if __name__ == "__main__":
    main()
