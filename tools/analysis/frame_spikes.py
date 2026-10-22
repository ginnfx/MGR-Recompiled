#!/usr/bin/env python3
"""Explain the slowest guest frames (the 1% low) from correlated timing.

Inputs: MGR_FRAME_CSV (guest present intervals, render-thread CPU), optional
MGR_GPU_TIMING_CSV (GPU busy per frame) and the run log (pipeline creation and
other GPU events). All three are aligned on wall-clock milliseconds.

Each slow frame is classified by what overlapped it:
  compile  pipeline creation logged during the frame
  gpu      GPU busy for the frame at or above the frame budget
  cpu      render-thread CPU at or above the frame budget (and above GPU busy)
  other    none of the above (guest waits, presentation, I/O, ...)
"""
import argparse
import bisect
import csv
import datetime
import math
import re
import statistics

LOG_TIME = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
PIPELINE_EVENT = 'Creating graphics pipeline state'


def read_csv(path):
    with open(path) as f:
        return [row for row in csv.DictReader(f) if not row[next(iter(row))].startswith('#')]


def log_events(path, needle):
    times = []
    with open(path, errors='replace') as f:
        for line in f:
            if needle not in line:
                continue
            m = LOG_TIME.match(line)
            if m:
                stamp = datetime.datetime.strptime(m[1], '%Y-%m-%d %H:%M:%S.%f')
                times.append(stamp.timestamp() * 1000.0)  # local time, like the log
    return times


def count_between(sorted_times, begin, end):
    return bisect.bisect_right(sorted_times, end) - bisect.bisect_left(sorted_times, begin)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('frames')
    p.add_argument('--gpu')
    p.add_argument('--log')
    p.add_argument('--start', type=float, default=30, help='seconds after first present')
    p.add_argument('--duration', type=float, default=30)
    p.add_argument('--budget-ms', type=float, default=1000 / 60)
    p.add_argument('--show', type=int, default=15)
    a = p.parse_args()

    frames = [{k: float(v) for k, v in r.items()} for r in read_csv(a.frames)]
    frames = [r for r in frames
              if a.start * 1000 <= r['elapsed_ms'] < (a.start + a.duration) * 1000]
    if not frames or 'unix_ms' not in frames[0]:
        raise SystemExit('no frames in window, or frame CSV lacks unix_ms')
    gpu = sorted((float(r['unix_ms']), float(r['gpu_busy_ms'])) for r in read_csv(a.gpu)) \
        if a.gpu else []
    gpu_times = [t for t, _ in gpu]
    pipelines = sorted(log_events(a.log, PIPELINE_EVENT)) if a.log else []

    intervals = sorted(r['interval_ms'] for r in frames)
    slow_count = max(1, math.ceil(len(frames) * 0.01))
    threshold = intervals[-slow_count]
    slow = sorted((r for r in frames if r['interval_ms'] >= threshold),
                  key=lambda r: -r['interval_ms'])[:slow_count]

    def gpu_busy_near(t, interval):
        # GPU rows are stamped when read back, up to a few frames after the present.
        i = bisect.bisect_left(gpu_times, t)
        window = [b for tt, b in gpu[max(0, i - 2):i + 4] if t - interval <= tt <= t + 4 * interval]
        return max(window) if window else None

    classes = {}
    rows = []
    for r in slow:
        end = r['unix_ms']
        begin = end - r['interval_ms']
        compiles = count_between(pipelines, begin, end)
        busy = gpu_busy_near(end, r['interval_ms'])
        cpu = r['render_thread_cpu_ms']
        if compiles:
            cls = 'compile'
        elif cpu >= a.budget_ms and (busy is None or cpu >= busy):
            cls = 'cpu'
        elif busy is not None and busy >= a.budget_ms:
            cls = 'gpu'
        else:
            cls = 'other'
        classes[cls] = classes.get(cls, 0) + 1
        rows.append((r, compiles, busy, cls))

    print(f'{len(frames)} frames, mean {1000 / statistics.mean(intervals):.1f} FPS, '
          f'1% low {1000 / statistics.mean(r["interval_ms"] for r in slow):.1f} FPS '
          f'({slow_count} frames >= {threshold:.1f} ms)')
    print(f'pipeline creations in window: '
          f'{count_between(pipelines, frames[0]["unix_ms"], frames[-1]["unix_ms"])}')
    print('slow frames by cause: ' + ', '.join(f'{k} {v}' for k, v in sorted(classes.items())))
    has_hooks = 'hooks_cpu_ms' in frames[0]
    print('interval_ms  cpu_ms  gpu_busy_ms  pipelines  cause   at_s' +
          ('   hooks_ms vdswap_ms' if has_hooks else ''))
    for r, compiles, busy, cls in rows[:a.show]:
        busy_text = f'{busy:11.2f}' if busy is not None else '          -'
        print(f'{r["interval_ms"]:11.2f} {r["render_thread_cpu_ms"]:7.2f} {busy_text} '
              f'{compiles:10d}  {cls:7s} {r["elapsed_ms"] / 1000:6.1f}' +
              (f' {r["hooks_cpu_ms"]:10.2f} {r["vdswap_cpu_ms"]:9.2f}' if has_hooks else ''))


if __name__ == '__main__':
    main()
