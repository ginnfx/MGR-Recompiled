#!/bin/bash
# Play build: visible window with the Metal performance HUD. Runs from any
# directory and passes extra cvars through to scripts/run.sh.
#   scripts/play.sh [extra cvars...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export MTL_HUD_ENABLED=1
# Any MTL_HUD_ELEMENTS list hides the MetalFX rows (they have no token), so the
# HUD runs with its defaults, ignoring a list exported by the shell or launchd.
unset MTL_HUD_ELEMENTS
exec "$ROOT/scripts/run.sh" "$@"
