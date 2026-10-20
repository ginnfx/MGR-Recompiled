#!/usr/bin/env python3
"""Whole-run command statistics; these do not share the stable frame window."""
import argparse
import json
from pathlib import Path
import re


def summarize(path):
    text = Path(path).read_text(errors='replace')
    batches = re.findall(
        r'\[mgr-gpu\] CP work ([\d.]+) ms/frame \+ GPU fence wait ([\d.]+) ms/frame, frame ([\d.]+) ms\n'
        r'\[mgr-gpu\] per frame over (\d+) frames:([^\n]+)', text)
    result = {'scope': 'entire run including warmup; different from 30-second frame window'}
    frames = sum(int(b[3]) for b in batches)
    if frames:
        result['cp_frames'] = frames
        for i, key in enumerate(['cp_work_wall_ms', 'cp_gpu_fence_wait_ms', 'cp_frame_wall_ms']):
            result[key] = sum(float(b[i])*int(b[3]) for b in batches)/frames
        commands = {}
        for batch in batches:
            for name, value in re.findall(r'(\w+)=([\d.]+)', batch[4]):
                commands[name] = commands.get(name, 0)+float(value)*int(batch[3])
        result['commands_per_cp_frame'] = {k: v/frames for k, v in commands.items()}
    resolves = re.findall(
        r'\[mgr-resolve\] native=(\d) resolves=120 tiles requested=(\d+) skipped=(\d+) dispatched=(\d+) '
        r'dispatches requested=(\d+) skipped=(\d+) dispatched=(\d+)', text)
    if resolves:
        result['resolve_batches'] = len(resolves)
        for i, key in enumerate(['tiles_requested', 'tiles_skipped', 'tiles_dispatched',
                                  'dispatches_requested', 'dispatches_skipped', 'dispatches_dispatched'], 1):
            result[key] = sum(int(r[i]) for r in resolves)
        result['resolve_tile_skip_percent'] = 100*result['tiles_skipped']/max(1,result['tiles_requested'])
        result['resolve_dispatch_skip_percent'] = 100*result['dispatches_skipped']/max(1,result['dispatches_requested'])
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    a = p.parse_args()
    print(json.dumps(summarize(a.log), indent=2))
