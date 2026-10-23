Metal Gear Rising: Revengeance recompiled from the Xbox 360 version using [ReXGlue](https://github.com/rexglue/rexglue-sdk)
Built against one exact `default.xex` (title ID `4B4E080A`, media ID `6CC18291`, version 4, no title update), SHA-256:
`907c8ebe94f085924f230d815ffa0b89c65ce60adfd00184b047276954ddf443`
Other regions or updated builds won't match the hardcoded addresses
WIP, don't expect good performance . No Win11 build planned since that one works fine on steam
you will prolly also see some streaks from raiden's sword that idk how to fix yet ;-;

## needed

- macOS on Apple Silicon (arm64)
- Your own extracted Xbox 360 copy of the game (the build above)
- Xcode Command Line Tools (the scripts use Apple clang at `/usr/bin/clang`, C++23)
- CMake 3.25 or newer
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

Set up the vendor sources and apply the MGR patches

```sh
git clone https://github.com/rexglue/rexglue-sdk.git vendor/rexglue-sdk
git -C vendor/rexglue-sdk checkout bd833a2
git -C vendor/rexglue-sdk apply ../../patches/xenos-gpu-mgr.patch
git -C vendor/rexglue-sdk submodule update --init --depth 1 thirdparty/cli11 thirdparty/imgui thirdparty/FFmpeg thirdparty/libmspack
git clone https://github.com/KhronosGroup/glslang.git vendor/glslang
git -C vendor/glslang checkout f4f1d8a
```

`scripts/build-optimized.sh` (below) builds the patched runtime and GPU plugin
`patches/rexglue-pr382-fp-traps.patch` is an upstream proposal and isn't part of the tested build
`patches/xenosrecomp-mgr.patch` is only needed for the shader analysis tools

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
