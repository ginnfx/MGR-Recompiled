#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_stats',root/'tools/experiment/summarize_gpu_stats.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)/'stderr.log'
 p.write_text('[mgr-gpu] CP work 3.00 ms/frame + GPU fence wait 2.00 ms/frame, frame 6.00 ms\n'
  '[mgr-gpu] per frame over 10 frames: Dispatch=8.0 Draw=50.0\n'
  '[mgr-gpu] CP work 6.00 ms/frame + GPU fence wait 5.00 ms/frame, frame 12.00 ms\n'
  '[mgr-gpu] per frame over 20 frames: Dispatch=5.0 Draw=80.0\n'
  '[mgr-resolve] native=1 resolves=120 tiles requested=100 skipped=25 dispatched=75 dispatches requested=8 skipped=2 dispatched=6\n')
 r=m.summarize(p)
 assert r['cp_frames']==30 and r['cp_work_wall_ms']==5 and r['cp_gpu_fence_wait_ms']==4
 assert r['commands_per_cp_frame']['Draw']==70
 assert r['resolve_tile_skip_percent']==25 and r['resolve_dispatch_skip_percent']==25
 print('GPU stats summary: frame weighting, counters and units passed')
