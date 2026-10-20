#!/usr/bin/env python3
"""Snapshot an experiment build as an inspectable macOS app (no installation)."""
import argparse
import pathlib
import plistlib
import shutil
p = argparse.ArgumentParser()
p.add_argument('build', type=pathlib.Path)
p.add_argument('app', type=pathlib.Path)
a = p.parse_args()
out = a.app / 'Contents' / 'MacOS'
out.mkdir(parents=True, exist_ok=True)
for source in a.build.iterdir():
    if source.name in {'mgr', 'mgr.toml', 'mgr_ui_font.bin', 'mgr_native_shaders.xrnb'} or source.suffix == '.dylib':
        shutil.copy2(source, out / source.name)
with (a.app / 'Contents' / 'Info.plist').open('wb') as f:
    plistlib.dump(dict(CFBundleDisplayName='MGR Native Rewrite', CFBundleName='MGR Native Rewrite',
        CFBundleExecutable='mgr', CFBundleIdentifier='org.macgrpc.native-rewrite',
        CFBundlePackageType='APPL', CFBundleVersion='1', NSHighResolutionCapable=True), f)
print(a.app.resolve())
