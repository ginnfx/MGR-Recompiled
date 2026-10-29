// First-run setup for the packaged app: asks for the player's own disc image
// (or extracted folder), verifies it and installs the game files.
#pragma once

#include <filesystem>
#include <functional>

// True when Option is held at launch, which reopens setup.
bool MgrSetupRequested();

enum class MgrSetupReason {
  kFirstRun,   // no game files yet
  kDamaged,    // files missing, damaged or from another manifest
  kRequested,  // Option held over a good install; the player may skip
};

// Runs on the UI thread without blocking it. Calls done(true) once
// data_root/game holds a verified install, or done(false) if the player quits
// (or skips, for kRequested).
void MgrRunSetup(const std::filesystem::path& data_root, MgrSetupReason reason,
                 std::function<void(bool installed)> done);
