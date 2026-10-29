Metal Gear Rising: Revengeance recompiled from the Xbox 360 version using [ReXGlue](https://github.com/rexglue/rexglue-sdk)
WIP, don't expect good performance . No Win11 build planned since that one works fine on steam

No game files are included. You need your own legally obtained copy of the Xbox 360 game
Not affiliated with Konami or PlatinumGames

## Play

Needs a Mac with Apple Silicon running macOS 14 or newer, and about 8 GB of free space

1. Download `MGR-Recompiled-<version>-macos-arm64.dmg` from the [releases page](https://github.com/ginnfx/MGR-Recompiled/releases)
2. Open it and drag **MGR Recompiled** into Applications
3. Open the app. The build isn't notarized, so macOS blocks the first launch
   - Click **Done** on the warning, then go to System Settings → Privacy & Security and click **Open Anyway** next to MGR Recompiled
   - Or run this once in Terminal: `xattr -dr com.apple.quarantine "/Applications/MGR Recompiled.app"`
4. Choose your copy of the game when asked. Either works:
   - a disc image (`.iso`) of the Xbox 360 disc
   - a folder with the extracted files (`default.xex` and `media/`)

The app checks the copy and installs the game files, which takes a minute or two. After that it starts straight into the game

Only the Xbox 360 USA/Europe disc is supported: title ID `4B4E080A`, media ID `6CC18291`, version 4, no title update. The app tells you if your copy is a different version

Everything the app writes lives in `~/Library/Application Support/MGR-Recompiled/`

```text
game/      installed game files
user/      saves
cache/     shader cache
logs/      logs (attach the latest one to bug reports)
mgr.toml   settings
```

Hold **Option** while opening the app to install the game files again. Delete the folder to reset everything

Graphics settings are on the Video page of the in-game Options menu

## Build from source

Built against one exact `default.xex`, SHA-256:
`907c8ebe94f085924f230d815ffa0b89c65ce60adfd00184b047276954ddf443`
Other regions or updated builds won't match the hardcoded addresses

### needed

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

### Setup

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
git -C vendor/rexglue-sdk apply ../../patches/rexglue-macos-message-box.patch
git -C vendor/rexglue-sdk submodule update --init --depth 1 thirdparty/cli11 thirdparty/imgui thirdparty/FFmpeg thirdparty/libmspack
git clone https://github.com/KhronosGroup/glslang.git vendor/glslang
git -C vendor/glslang checkout f4f1d8a
```

`scripts/build-optimized.sh` (below) builds the patched runtime and GPU plugin
`patches/rexglue-pr382-fp-traps.patch` is an upstream proposal and isn't part of the tested build
`patches/xenosrecomp-mgr.patch` is only needed for the shader tools that make `private/mgr_native_shaders.xrnb`. Without that file the Xenos renderer falls back to Xenia's shaders

### Build and run

Once the SDK and vendor sources are set up

```sh
tools/sdk/mac-arm64/bin/rexglue codegen mgr_manifest.toml
scripts/build-optimized.sh
scripts/run.sh
```

Saves go in `private/user`
Logs go in `private/logs`
`MGR_DEVELOPER_UI=1` enables the F2 developer settings window

### Package a release

```sh
scripts/package-macos.sh
```

This builds `build-release/` for macOS 14 and writes `MGR Recompiled.app`, the `.dmg` and its SHA-256 to `dist/`, with debug symbols in `dist/symbols-<version>/`. Set `MGR_VERSION` for the version number and `SIGN_IDENTITY` to sign with a Developer ID instead of ad-hoc

If the supported disc's files ever change, regenerate the install manifest:

```sh
python3 tools/release/hash_game_files.py private/game src/mgr_game_manifest.inc
```

## License

MGR Recompiled is licensed under the GNU General Public License v3.0, see `LICENSE`
Third-party licenses are listed in `THIRD_PARTY_NOTICES`
