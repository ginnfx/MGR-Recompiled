#!/usr/bin/env python3
"""Capture one frame from an owned experiment app; never a benchmark run."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('label')
p.add_argument('--resolve', choices=['0', '1'], default='0')
p.add_argument('--writes', choices=['0', '1'], default='0')
p.add_argument('--validate-only', action='store_true', help='run validation without capture')
p.add_argument('--warmup', type=int, default=40)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = root/'private/native-rewrite'/a.label
if out.exists():
    raise SystemExit('Refusing to overwrite capture evidence')
app = root/'private/native-rewrite/MGR Native Rewrite.app/Contents/MacOS/mgr'
env = dict(os.environ, MGR_CAPTURE='0' if a.validate_only else '1',
           MGR_NATIVE_RESOLVE=a.resolve, MGR_NATIVE_RESOLVE_WRITES=a.writes)
process = subprocess.Popen([str(root/'experiments/native-rewrite/run-app.sh'),
    a.label, 'verify'], cwd=root, env=env)
pid = None
try:
    started = time.monotonic()
    while time.monotonic()-started < a.warmup:
        if process.poll() is not None:
            raise RuntimeError('Capture app exited during warmup')
        log = out/'stderr.log'
        if pid is None and log.exists():
            match = re.search(r' mgr\[(\d+):', log.read_text(errors='replace'))
            if match:
                candidate = int(match[1])
                argv = subprocess.check_output(['ps','-p',str(candidate),'-o','command='],text=True)
                if str(app) not in argv or str(out/'game.log') not in argv:
                    raise RuntimeError('Capture PID ownership check failed')
                pid = candidate
        time.sleep(1)
    if pid is None:
        raise RuntimeError('No owned capture process found')
    if a.validate_only:
        print('Validation-only run completed: ' + str(out))
        raise SystemExit(0)
    for name, command in [
        ('boundaries', ['gpucapture','boundaries','--pid',str(pid)]),
        ('capture', ['gpucapture','start','--pid',str(pid),'--count','1',
                     '--output',str(out/'frame.gputrace')])]:
        with (out/(name+'.txt')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=60, check=True)
    trace = out/'frame.gputrace'
    if not trace.exists():
        raise RuntimeError('Capture returned without a trace')
    print(trace)
finally:
    if pid is not None:
        # Targeted stop is harmless if no capture is active; no global --all.
        try:
            subprocess.run(['gpucapture','stop','--pid',str(pid)], capture_output=True, timeout=15)
        finally:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.kill(pid, signal.SIGKILL)
                process.wait(timeout=10)
    elif process.poll() is None:
        raise RuntimeError('Unknown app PID; refusing broad process cleanup')
