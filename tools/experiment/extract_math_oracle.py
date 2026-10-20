#!/usr/bin/env python3
"""Extract unchanged generated bodies for differential tests, never a second spec."""
import argparse
import pathlib
import re

p = argparse.ArgumentParser()
p.add_argument('--generated', type=pathlib.Path, required=True)
p.add_argument('--memory-only', action='store_true')
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
names = {'sub_826CBFB8', 'sub_826CC010', 'sub_826CC0E0', 'sub_826CC160',
         '__savegprlr_25', '__restgprlr_25'}
if a.memory_only:
    names = {'sub_822F67E0'}
found = {}
for source in sorted(a.generated.glob('mgr_recomp.*.cpp')):
    text = source.read_text()
    for match in re.finditer(r'DEFINE_REX_FUNC\(([^)]+)\) \{', text):
        name = match[1]
        if name not in names:
            continue
        end = text.find('\nDEFINE_REX_FUNC(', match.end())
        found[name] = text[match.start():end if end >= 0 else len(text)]
if set(found) != names:
    raise SystemExit(f'Missing oracle bodies: {names - set(found)}')
a.output.parent.mkdir(parents=True, exist_ok=True)
text = '#include "mgr_pch.h"\n'
text += '\n'.join(f'DECLARE_REX_FUNC({name});' for name in sorted(names)) + '\n'
text += '\n'.join(found[name] for name in sorted(names))
if not a.output.exists() or a.output.read_text() != text:
    a.output.write_text(text)
