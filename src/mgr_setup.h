// First-run setup for the packaged app: asks for the player's own disc image
// (or extracted folder), verifies it and installs the game files.
#pragma once

#include <filesystem>
#include <functional>

// True when Option is held at launch, which reopens setup.
bool MgrSetupRequested();

// Runs on the UI thread without blocking it. Calls done(true) once
// data_root/game holds a verified install, or done(false) if the player quits.
void MgrRunSetup(const std::filesystem::path& data_root, bool reinstall,
                 std::function<void(bool installed)> done);
