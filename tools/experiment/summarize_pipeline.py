#!/usr/bin/env python3
"""Join runtime CPU telemetry to the guest frame window by steady-clock time.

Queue present success counts requests, not displayed frames. Use Metal HUD
separately for host presentation intervals and GPU execution samples.
"""
import argparse
import collections
import csv
import json
import statistics
from pathlib import Path


def summarize(frames, pipeline, start, duration):
    with Path(frames).open() as f:
        guest = list(csv.DictReader(f))
    origin = int(guest[0]['steady_ns']) - round(float(guest[0]['elapsed_ms'])*1e6)
    lo, hi = origin + int(start*1e9), origin + int((start+duration)*1e9)
    with Path(pipeline).open() as f:
        rows = [r for r in csv.DictReader(f) if lo <= int(r['steady_ns']) < hi]
    groups = collections.defaultdict(list)
    for r in rows:
        groups[r['focus']].append(r)
    result = {'window_start_s': start, 'window_duration_s': duration,
              'displayed_frames': None, 'gpu_execution_ms': None, 'focus_groups': {}}
    for focus, group in groups.items():
        result['focus_groups'][focus] = {
            'attempts': len(group),
            'outcomes': dict(collections.Counter(r['outcome'] for r in group)),
            'extents': sorted(set((int(r['width']), int(r['height'])) for r in group)),
            'ui_attempts': sum(int(r['ui']) for r in group),
            'input_calls': sum(int(r['input_calls']) for r in group),
            'input_total_ms': sum(int(r['input_ns']) for r in group)/1e6,
            **{f'mean_{key}_ms': statistics.mean(int(r[key+'_ns']) for r in group)/1e6
               for key in ['attempt', 'reuse', 'acquire', 'submit', 'present']}}
        if 'submit_lock_ns' in group[0]:
            for key in ['submit', 'present']:
                result['focus_groups'][focus][f'mean_{key}_lock_ms'] = statistics.mean(
                    int(r[key+'_lock_ns']) for r in group)/1e6
                result['focus_groups'][focus][f'mean_{key}_excluding_lock_ms'] = statistics.mean(
                    int(r[key+'_ns'])-int(r[key+'_lock_ns']) for r in group)/1e6
    result['limitations'] = [
        'focus -1 means no focus notification observed; it is not unfocused',
        'focus is sampled at attempt completion; transition attempts may straddle states',
        'Vulkan acquire timing is a drawable-acquisition proxy, not direct nextDrawable timing',
        'submit/present include host queue locking; they are not GPU execution times',
        'input wall time covers guest GetState, not all event handling',
        'visible window area and scene identity require external verification']
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('frames', type=Path)
    p.add_argument('pipeline', type=Path)
    p.add_argument('--start', type=float, default=30)
    p.add_argument('--duration', type=float, default=30)
    a = p.parse_args()
    print(json.dumps(summarize(a.frames, a.pipeline, a.start, a.duration), indent=2))
