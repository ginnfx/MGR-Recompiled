#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

// Keyboard text for MGR's 25 controller icons, in atlas order (btn_001..btn_025):
// B A Y X RB RT LB LT, D-pad (all, up/down, left/right, up, down, left, right),
// RS, RS press, LS, LS press, START, BACK, left-stick directions (up, down,
// left, right).
inline constexpr size_t kButtonCount = 25;
using ButtonLabels = std::array<std::string, kButtonCount>;

// Short display form of one keybind alternative: "Shift+Up" -> "⇧↑",
// "Return" -> "ENTER", "q" -> "Q". Empty for an empty bind.
std::string KeyLabel(std::string_view bind);

// Looks up keybind cvar values by name ("keybind_a", "mnk_mouse", ...).
using CvarLookup = std::function<std::string(const char* name)>;
ButtonLabels BuildButtonLabels(const CvarLookup& lookup);

// PlayStation glyphs for the face, shoulder, stick and menu buttons; the D-pad
// and stick-direction art is shared with Xbox and stays empty (original art).
// PS3 pads name the menu buttons START/SELECT rather than OPTIONS/SHARE.
ButtonLabels PlayStationLabels(bool ps3);
// Buttons drawn on round caps for PlayStation: face buttons and both sticks.
inline constexpr uint32_t kPlayStationRoundButtons = 0xFu | 0xFu << 15;
