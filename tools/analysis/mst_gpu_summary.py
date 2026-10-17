"""Summarize an xctrace 'metal-gpu-intervals' export: GPU busy time by channel and encoder sizes.

usage: mst_gpu_summary.py gpu-intervals.xml [process-substring]
"""
import collections
import statistics
import sys
import xml.etree.ElementTree as ET

COLS = ["start", "duration", "channel", "frame", "latency", "depth", "label", "state",
        "uuid", "color", "process", "gpu", "subtitle", "iosurface", "bytes", "cmdbuf",
        "encoder", "submission"]


def rows(path):
    ids = {}
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag != "row":
            continue
        vals = []
        for child in el:
            ref = child.get("ref")
            if ref is not None:
                vals.append(ids.get(ref))
                continue
            text = child.get("fmt") or (child.text or "")
            raw = child.text
            if child.get("id"):
                ids[child.get("id")] = (text, raw)
                for sub in child.iter():
                    if sub.get("id") and sub is not child:
                        ids[sub.get("id")] = (sub.get("fmt") or sub.text or "", sub.text)
            vals.append((text, raw))
        el.clear()
        yield dict(zip(COLS, vals))


def main():
    proc = sys.argv[2] if len(sys.argv) > 2 else "mgr"
    by_chan = collections.defaultdict(list)
    t0 = t1 = None
    for r in rows(sys.argv[1]):
        if not r.get("process") or proc not in (r["process"][0] or ""):
            continue
        if r.get("depth") and r["depth"][1] not in ("0", None):
            continue
        start, dur = int(r["start"][1]), int(r["duration"][1])
        t0 = start if t0 is None else min(t0, start)
        t1 = start + dur if t1 is None else max(t1, start + dur)
        by_chan[r["channel"][0]].append(dur)
    span = (t1 - t0) / 1e9
    print(f"span {span:.2f}s")
    for chan, durs in sorted(by_chan.items(), key=lambda kv: -sum(kv[1])):
        tot = sum(durs) / 1e9
        print(f"{chan:12s} busy {100 * tot / span:5.1f}%  n={len(durs):6d} ({len(durs) / span:7.0f}/s)  "
              f"median {statistics.median(durs) / 1e3:7.1f}us  p99 {sorted(durs)[int(len(durs) * .99)] / 1e3:8.1f}us  max {max(durs) / 1e6:6.1f}ms")


if __name__ == "__main__":
    main()
