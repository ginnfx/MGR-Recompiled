Metal Gear Rising: Revengeance recompiled from the Xbox 360 version using [ReXGlue](https://github.com/rexglue/rexglue-sdk)
Usa version of game i think
WIP, don't expect good performance . No Win11 build planned since that one works fine on steam

## needed

- Your own extracted Xbox 360 copy of the game
- Clang with C++23 support
- CMake 3⸱25 or newer
- Ninja
- Python 3
- Git
- ReXGlue SDK build `nightly-20261002-bd833a2a`
- The patched ReXGlue source tree and glslang headers

SDL3, Vulkan and the other runtime libraries come from the SDK

## Setup

Clone repo

```sh
git clone https://github.com/ginnfx/MGR-Recompiled.git
cd MGR-Recompiled
```

Put your extracted game files in `private/game`

```text
private/game/
├── default.xex
└── media/
```

Download the SDK from the [ReXGlue release page](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20261002-bd833a2a) and extract it under `tools/sdk`

The build expects these paths

```text
tools/sdk/mac-arm64/
vendor/rexglue-sdk/
vendor/glslang/
```

The vendor setup still needs the project runtime and renderer changes
A fresh clone alone isn't enough yet

## Build and run

Once the SDK and vendor sources are set up

```sh
tools/sdk/mac-arm64/bin/rexglue codegen mgr_manifest.toml
scripts/build-optimized.sh
scripts/run.sh
```

F2 opens the settings menu

Saves go in `private/user`
Logs go in `private/logs`

No game files included
