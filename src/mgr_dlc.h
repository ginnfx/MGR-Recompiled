// Xbox Live DLC packages (Jetstream, Blade Wolf, VR Missions). Setup copies a
// player's packages into a pending folder; on the next boot they're unpacked
// into the runtime's content folder, where the game finds them.
#pragma once

#include <filesystem>

// Unpacks every package in pending_dir with the SDK content manager, then
// deletes it. A package that fails stays for the next boot. Call after the
// xex is loaded (the title ID comes from it).
void MgrInstallPendingDlc(const std::filesystem::path& pending_dir);
