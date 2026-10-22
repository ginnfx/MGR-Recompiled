#!/usr/bin/env python3
"""Summarize identical stable windows; 1% low = reciprocal of slowest 1% mean."""
import argparse
import csv
import json
import math
import statistics

p = argparse.ArgumentParser()
p.add_argument('csv')
p.add_argument('--start', type=float, default=30, help='seconds after first guest present')
p.add_argument('--duration', type=float, default=30)
a = p.parse_args()
with open(a.csv) as f:
    rows = [{k:float(v) for k,v in row.items()} for row in csv.DictReader(f)]
rows = [r for r in rows if a.start*1000 <= r['elapsed_ms'] < (a.start+a.duration)*1000]
if not rows:
    raise SystemExit('No guest frames in selected window')
dt = [r['interval_ms'] for r in rows]
slow = sorted(dt, reverse=True)[:max(1, math.ceil(len(dt)*0.01))]
print(json.dumps(dict(frames=len(dt), requested_start_s=a.start, requested_duration_s=a.duration,
    measured_duration_s=sum(dt)/1000, average_fps=1000/statistics.mean(dt),
    one_percent_low_fps=1000/statistics.mean(slow),
    mean_guest_present_interval_ms=statistics.mean(dt),
    mean_render_thread_cpu_ms=statistics.mean(r['render_thread_cpu_ms'] for r in rows),
    mean_rss_mib=statistics.mean(r['rss_mib'] for r in rows),
    max_rss_mib=max(r['rss_mib'] for r in rows),
    gpu_execution_time_ms=None),indent=2))
