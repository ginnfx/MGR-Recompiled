#!/bin/bash
# Send key presses to the running mgr window. Usage: autopress.sh <keycode> <count> <interval>
# keycodes: 36 Return(Start)  49 Space(A)
for _ in $(seq "$2"); do
  osascript -e 'tell application "System Events" to set frontmost of (first process whose name is "mgr") to true' \
            -e "tell application \"System Events\" to key code $1" >/dev/null 2>&1
  sleep "$3"
done
