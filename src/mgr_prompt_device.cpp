// Last-used input device for button prompts. SDL is owned by the runtime dylib
// (the app links its exported copy), so this watch sees the same events as the
// SDL and MnK input drivers.
#include "mgr_prompt_device.h"

#include <rex/cvar.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DEFINE_STRING(mgr_prompt_style, "auto", "MGR/Input",
                      "Button prompts: auto (last used device), keyboard, xbox, playstation")
    .allowed({"auto", "keyboard", "xbox", "playstation"});

namespace {
// Matches XInput's left-stick deadzone; idle drift must not switch glyphs.
constexpr int kAxisThreshold = 7849;
constexpr int kTriggerThreshold = 8000;

std::atomic<PromptFamily> last_used{PromptFamily::kKeyboard};
std::atomic<bool> installed{false};

PromptFamily FamilyForGamepad(SDL_JoystickID id) {
  switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3:
      return PromptFamily::kPlayStation3;
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return PromptFamily::kPlayStation;
    default:
      return PromptFamily::kXbox;
  }
}

bool IsDeliberateAxis(const SDL_GamepadAxisEvent& axis) {
  const bool trigger = axis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                       axis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
  return std::abs(int(axis.value)) > (trigger ? kTriggerThreshold : kAxisThreshold);
}

bool Watch(void*, SDL_Event* event) {
  if (!event) return true;
  switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (REXCVAR_GET(mnk_mode)) last_used = PromptFamily::kKeyboard;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (REXCVAR_GET(mnk_mode) && REXCVAR_GET(mnk_mouse) &&
          (event->motion.xrel != 0 || event->motion.yrel != 0)) {
        last_used = PromptFamily::kKeyboard;
      }
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      last_used = FamilyForGamepad(event->gbutton.which);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      if (IsDeliberateAxis(event->gaxis)) last_used = FamilyForGamepad(event->gaxis.which);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      // A newly connected pad is the likely next device when MnK is off.
      if (!REXCVAR_GET(mnk_mode)) last_used = FamilyForGamepad(event->gdevice.which);
      break;
    default:
      break;
  }
  return true;  // watches never filter
}
}  // namespace

void MgrPromptDeviceInstall() {
  if (installed.exchange(true)) return;
  last_used = REXCVAR_GET(mnk_mode) ? PromptFamily::kKeyboard : PromptFamily::kXbox;
  // Reference-counted; keeps the events subsystem alive for the watch.
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
    installed = false;
    return;
  }
  if (!SDL_AddEventWatch(Watch, nullptr)) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    installed = false;
  }
}

void MgrPromptDeviceUninstall() {
  if (!installed.exchange(false)) return;
  SDL_RemoveEventWatch(Watch, nullptr);
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

PromptFamily MgrPromptFamily() {
  // Locked copy: the guest render thread asks while the settings UI may be
  // assigning the string on another thread. Called once per 30 frames.
  const std::string style = rex::cvar::GetFlagByName("mgr_prompt_style");
  if (style == "keyboard") return PromptFamily::kKeyboard;
  if (style == "xbox") return PromptFamily::kXbox;
  if (style == "playstation") return PromptFamily::kPlayStation;
  return last_used.load(std::memory_order_relaxed);
}
