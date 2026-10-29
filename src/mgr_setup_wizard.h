// Full-window first-run setup drawn with Dear ImGui over the game window
// (native renderer). Same pages as Unleashed Recompiled's installer: intro,
// game, optional DLC, space check, install, done.
#pragma once

#include <filesystem>
#include <functional>

#include "mgr_setup.h"

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

// Shows the setup as an ImGui dialog. done(true) runs once data_root/game is
// installed, done(false) when the player quits (or skips, for kRequested).
// done is called from inside the dialog's draw; defer anything that tears down
// the drawing path.
void MgrShowSetupWizard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                        const std::filesystem::path& data_root, MgrSetupReason reason,
                        std::function<void(bool installed)> done);
