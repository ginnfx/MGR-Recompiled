#pragma once

#include <cstddef>
#include <cstdint>

// Which button glyphs the game should show. Follows the device that most
// recently produced real input unless the mgr_prompt_style cvar forces one.
enum class PromptFamily : uint8_t { kKeyboard, kXbox, kPlayStation, kPlayStation3 };
inline constexpr size_t kPromptFamilyCount = 4;

// Registers the SDL event watch. Call once from the UI thread after setup.
void MgrPromptDeviceInstall();
void MgrPromptDeviceUninstall();
PromptFamily MgrPromptFamily();
