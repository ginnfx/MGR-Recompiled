#!/bin/bash
# Play build: visible window with the Metal performance HUD. Runs from any
# directory and passes extra cvars through to scripts/run.sh.
#   scripts/play.sh [extra cvars...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export MTL_HUD_ENABLED=1
export MTL_HUD_ELEMENTS="${MTL_HUD_ELEMENTS:-device,fps,frameinterval,gputime,frameintervalgraph,memory}"
exec "$ROOT/scripts/run.sh" "$@"
