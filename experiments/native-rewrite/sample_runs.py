#!/usr/bin/env python3
"""Run an ABBA boot/attract comparison, controlling only apps launched here.

No user input is synthesized. Visual matching is an explicit evidence gap.
Metal HUD batches and guest present samples measure different frame boundaries.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--seconds', type=int, default=80)
p.add_argument('--start', type=int, default=30)
p.add_argument('--duration', type=int, default=30)
p.add_argument('--tag', default='final')
p.add_argument('--comparison', choices=['guest', 'resolve'], default='guest')
a = p.parse_args()
if a.seconds < a.start + a.duration + 5:
    p.error('run must extend beyond the entire sample window')
root = Path(__file__).resolve().parents[2]
app = root/'private/native-rewrite/MGR Native Rewrite.app/Contents/MacOS'
results = []
for leg, native in [('A1',False),('B1',True),('B2',True),('A2',False)]:
    mode = 'sample-native' if a.comparison == 'resolve' or native else 'sample-original'
    label = f'{a.tag}-{leg}'
    out = root/'private/native-rewrite'/label
    if out.exists():
        raise SystemExit(f'Refusing to overwrite evidence: {out}')
    shutil.copyfile(root/'private/baseline/mgr.toml', app/'mgr.toml')
    command = [str(root/'experiments/native-rewrite/run-app.sh'), label, mode]
    print(f'START {label} {mode}', flush=True)
    env = dict(os.environ, MGR_NATIVE_RESOLVE=str(int(a.comparison == 'resolve' and native)))
    process = subprocess.Popen(command, cwd=root, env=env)
    started = time.monotonic()
    pid = None
    try:
        while time.monotonic()-started < a.seconds:
            if process.poll() is not None:
                raise RuntimeError(f'App exited early: {label}, code={process.returncode}')
            log = out/'stderr.log'
            if pid is None and log.exists():
                match = re.search(r' mgr\[(\d+):', log.read_text(errors='replace'))
                if match:
                    candidate = int(match[1])
                    argv = subprocess.check_output(['ps','-p',str(candidate),'-o','command='],text=True)
                    # Bind cleanup to this run, not to a name shared by other game processes.
                    if str(out/'game.log') not in argv or str(app/'mgr') not in argv:
                        raise RuntimeError('PID does not belong to this measurement run')
                    pid = candidate
            time.sleep(1)
    finally:
        if pid is not None:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                raise RuntimeError('App did not stop; refusing to start a competing run')
        elif process.poll() is None:
            raise RuntimeError('Cannot identify owned app; stop before another run')
    command = ['python3',str(root/'tools/experiment/summarize_frames.py'),str(out/'frames.csv'),
               '--start',str(a.start),'--duration',str(a.duration),
               '--metal-log',str(out/'stderr.log')]
    summary = json.loads(subprocess.check_output(command,text=True))
    summary['gpu_commands_whole_run'] = json.loads(subprocess.check_output([
        'python3', str(root/'tools/experiment/summarize_gpu_stats.py'),
        str(out/'stderr.log')], text=True))
    summary.update(label=label, mode=mode,
        comparison=a.comparison, native_resolve=env['MGR_NATIVE_RESOLVE'],
        scene='unattended boot/automatic attract; visual matching unverified',
        validation=False, shadow_checks=False)
    pipeline = out/'pipeline.csv'
    if pipeline.exists():
        summary['pipeline'] = json.loads(subprocess.check_output([
            'python3', str(root/'tools/experiment/summarize_pipeline.py'),
            str(out/'frames.csv'), str(pipeline), '--start', str(a.start),
            '--duration', str(a.duration)], text=True))
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    results.append(summary)
    print(json.dumps(summary),flush=True)
report = root/'private/native-rewrite'/f'{a.tag}-comparison.json'
report.write_text(json.dumps(results,indent=2)+'\n')
print(f'COMPLETE {report}',flush=True)
