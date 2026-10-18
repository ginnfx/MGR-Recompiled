#pragma once

#include "mgr_button_atlas.h"

#include <string>

// White keycap for one atlas icon (CoreText). The style follows the original
// art: outline ring, filled cap with knocked-out text, or the larger HUD cap.
// Round caps are circles centered in the cell (PlayStation buttons).
Image RenderKeyIcon(const IconRect& rect, const std::string& label, bool round);
