#!/usr/bin/env python3
"""Reapply experiment patches to the preserved, already MGR-patched vendor copy.

Refuse unknown source versions. Does not download or touch SDK input links.
"""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
vendor = root/'vendor/rexglue-sdk'
for name in ['native-pipeline-metrics', 'native-resolve-coherence']:
    hashes = json.loads((root/'experiments/native-rewrite'/(name+'-hashes.json')).read_text())
    actual = {f: hashlib.sha256((vendor/f).read_bytes()).hexdigest() for f in hashes}
    if all(actual[f] == h['after'] for f, h in hashes.items()):
        print(name + ': already applied')
        continue
    if not all(actual[f] == h['before'] for f, h in hashes.items()):
        raise SystemExit(name + ': source differs from preserved preimage; refusing partial application')
    patch = (root/'patches'/(name+'.patch')).read_bytes()
    command = ['patch', '--batch', '--forward', '-p1', '-d', str(vendor)]
    subprocess.run(command+['--dry-run'], input=patch, check=True)
    subprocess.run(command, input=patch, check=True)
    if any(hashlib.sha256((vendor/f).read_bytes()).hexdigest() != h['after']
           for f, h in hashes.items()):
        raise SystemExit(name + ': post-apply checksum mismatch')
    print(name + ': applied and checksummed')
