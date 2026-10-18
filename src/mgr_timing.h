#pragma once

#include <cstdint>

enum class MgrTimingMode : uint8_t {
  kOriginal,  // guest timing untouched (60 Hz)
  kCap120,    // fractional frame scale, host cap at 120 Hz
  kUnlocked,  // fractional frame scale, no host cap
};

// No-op in original mode. The legacy MGR_EXPERIMENTAL_FPS=120 environment
// selects the initial mode; live settings can subsequently change it.
void MgrTimingOnPresent(const uint8_t* base);
// May change at any time; the hooks read it every frame. refresh_hz is the
// guest vblank rate the mode runs at (written to the graphics device).
void MgrTimingSetMode(MgrTimingMode mode, uint32_t refresh_hz);
