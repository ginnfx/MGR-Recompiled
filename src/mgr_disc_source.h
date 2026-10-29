// Reads the game files straight out of an Xbox 360 disc image (XGD2/XGD3 .iso)
// through the SDK's disc image device, for the first-run installer.
#pragma once

#include <filesystem>
#include <memory>

#include "mgr_game_install.h"

namespace mgr_install {

// Null when the file is not a readable Xbox 360 disc image.
std::unique_ptr<GameSource> OpenDiscImage(const std::filesystem::path& iso);

}  // namespace mgr_install
