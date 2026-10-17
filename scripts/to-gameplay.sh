#!/bin/bash
# Drive a running mgr from boot to the VR tutorial (timings measured on M2, run-07).
D="$(dirname "$0")"
sleep 55
"$D/autopress.sh" 36 1 6     # PRESS START
"$D/autopress.sh" 49 12 5    # confirm save / new game / difficulty
for _ in 1 2 3 4; do "$D/autopress.sh" 36 1 10; "$D/autopress.sh" 49 1 10; done  # skip codec/cutscenes
