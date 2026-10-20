#!/usr/bin/env python3
"""Summarize identical stable windows; 1% low = reciprocal of slowest 1% mean."""
import argparse
import csv
import json
import datetime
import pathlib
import re
import math
import statistics

p = argparse.ArgumentParser()
p.add_argument('csv')
p.add_argument('--metal-log', type=pathlib.Path)
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
result = dict(frames=len(dt), requested_start_s=a.start, requested_duration_s=a.duration,
    measured_duration_s=sum(dt)/1000, average_fps=1000/statistics.mean(dt),
    one_percent_low_fps=1000/statistics.mean(slow),
    mean_guest_present_interval_ms=statistics.mean(dt),
    mean_render_thread_cpu_ms=statistics.mean(r['render_thread_cpu_ms'] for r in rows),
    mean_rss_mib=statistics.mean(r['rss_mib'] for r in rows),
    max_rss_mib=max(r['rss_mib'] for r in rows),
    gpu_execution_time_ms=None)
if a.metal_log:
    # Apple documents the first three values as frame number and memory usage,
    # followed by present-interval/GPU-time pairs (milliseconds). Select whole
    # log batches by wall time, so GPU and guest-frame windows are approximate.
    origin = pathlib.Path(a.csv).stat().st_birthtime
    gpu = []
    host_intervals = []
    for line in a.metal_log.read_text().splitlines():
        if 'metal-HUD: ' not in line:
            continue
        try:
            timestamp = datetime.datetime.fromisoformat(line[:23]).timestamp()
            values = [float(x) for x in line.split('metal-HUD: ',1)[1].split(',')]
        except ValueError:
            continue
        if a.start <= timestamp-origin < a.start+a.duration and len(values) >= 5:
            gpu.extend(values[4::2])
            host_intervals.extend(values[3::2])
    if gpu:
        result['gpu_execution_time_ms'] = statistics.mean(gpu)
        result['metal_host_frame_samples'] = len(gpu)
        result['metal_host_mean_present_interval_ms'] = statistics.mean(host_intervals)
        result['metal_host_presentation_fps'] = 1000/statistics.mean(host_intervals)
        result['gpu_window_alignment'] = 'whole HUD batches by wall timestamp, approximate guest-window alignment'
print(json.dumps(result,indent=2))
