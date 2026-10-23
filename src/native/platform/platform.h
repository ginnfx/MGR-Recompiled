/**
 * @file    platform/platform.h
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR: only the window and fatal-error helpers the renderer uses.
 */
#pragma once

#include <string_view>

#include "platform/native_window.h"

namespace mgrn::platform {

// Logs and shows a blocking error box (rex::ShowSimpleMessageBox).
void ShowFatalError(std::string_view title, std::string_view body);

} // namespace mgrn::platform
