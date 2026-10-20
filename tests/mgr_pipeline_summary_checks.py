#!/usr/bin/env python3
import csv
import importlib.util
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pipeline', root/'tools/experiment/summarize_pipeline.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d/'frames.csv').write_text('steady_ns,elapsed_ms\n100100000000,100\n')
    (d/'pipeline.csv').write_text(
        'steady_ns,focus,ui,nonblocking,width,height,outcome,attempt_ns,reuse_ns,acquire_ns,submit_ns,present_ns,input_calls,input_ns\n'
        '101000000000,1,0,1,1280,720,present_requested,5000000,1000000,2000000,1000000,1000000,5,900000\n'
        '101100000000,0,1,0,1280,720,acquire_busy,6000000,1000000,3000000,0,0,3,800000\n'
        '103000000000,0,0,1,1280,720,present_requested,0,0,0,0,0,0,0\n')
    result = module.summarize(d/'frames.csv', d/'pipeline.csv', 1, 1)
    assert result['focus_groups']['1']['outcomes'] == {'present_requested': 1}
    assert result['focus_groups']['0']['outcomes'] == {'acquire_busy': 1}
    assert result['focus_groups']['1']['mean_acquire_ms'] == 2
    assert result['focus_groups']['0']['input_calls'] == 3
    assert result['displayed_frames'] is None
print('pipeline summary: clock join, focus groups, exclusion and units passed')
