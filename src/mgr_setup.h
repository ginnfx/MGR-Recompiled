// First-run setup for the packaged app: asks for the player's own disc image
// (or extracted folder), verifies it and installs the game files.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

// True when Option is held at launch, which reopens setup.
bool MgrSetupRequested();

enum class MgrSetupReason {
  kFirstRun,   // no game files yet
  kDamaged,    // files missing, damaged or from another manifest
  kRequested,  // Option held over a good install; the player may skip
};

// macOS file pickers (modal, UI thread). The game copy is an .iso or, with
// folder set, a directory holding default.xex; DLC packages are any files.
std::optional<std::filesystem::path> MgrPickGameCopy(bool folder);
std::vector<std::filesystem::path> MgrPickDlcPackages();

// AppKit fallback (the Xenos renderer has no pre-game ImGui). Runs on the UI
// thread without blocking it. Calls done(true) once
// data_root/game holds a verified install, or done(false) if the player quits
// (or skips, for kRequested).
void MgrRunSetup(const std::filesystem::path& data_root, MgrSetupReason reason,
                 std::function<void(bool installed)> done);
