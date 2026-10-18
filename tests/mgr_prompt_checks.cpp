// Deterministic keyboard-prompt checks, not a substitute for in-game verification.
// Run: mgr_prompt_checks <core.wtb> [patched-atlas.ppm]
#include "mgr_button_atlas.h"
#include "mgr_icon_render.h"
#include "mgr_key_labels.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr size_t kWtbAtlasOffset = 0x41000;  // core.wtb texture 1 data

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void CheckLabel(std::string_view bind, std::string_view expected) {
  const std::string got = KeyLabel(bind);
  if (got != expected) {
    std::fprintf(stderr, "FAIL: KeyLabel(%.*s) = %s, want %.*s\n", int(bind.size()), bind.data(),
                 got.c_str(), int(expected.size()), expected.data());
    std::exit(1);
  }
}

// SDK defaults from mnk_input_driver.cpp.
std::map<std::string, std::string> DefaultBinds() {
  return {{"keybind_a", "Semicolon,Space"}, {"keybind_b", "Quote,Backspace"},
          {"keybind_x", "L"},                {"keybind_y", "P"},
          {"keybind_left_trigger", "Q,I"},   {"keybind_right_trigger", "E,O"},
          {"keybind_left_shoulder", "1"},    {"keybind_right_shoulder", "3"},
          {"keybind_lstick_up", "W"},        {"keybind_lstick_down", "S"},
          {"keybind_lstick_left", "A"},      {"keybind_lstick_right", "D"},
          {"keybind_lstick_press", "F"},     {"keybind_rstick_up", "Up"},
          {"keybind_rstick_down", "Down"},   {"keybind_rstick_left", "Left"},
          {"keybind_rstick_right", "Right"}, {"keybind_rstick_press", "K"},
          {"keybind_dpad_up", "Shift+Up"},   {"keybind_dpad_down", "Shift+Down"},
          {"keybind_dpad_left", "Shift+Left"}, {"keybind_dpad_right", "Shift+Right"},
          {"keybind_back", "Z,Tab"},         {"keybind_start", "X,Return"},
          {"mnk_mouse", "false"}};
}

ButtonLabels LabelsFor(const std::map<std::string, std::string>& binds) {
  return BuildButtonLabels([&](const char* name) {
    auto it = binds.find(name);
    return it == binds.end() ? std::string() : it->second;
  });
}

void CheckLabels() {
  CheckLabel("Q", "Q");
  CheckLabel("q", "Q");
  CheckLabel(" Space ", "SPACE");
  CheckLabel("Return", "ENTER");
  CheckLabel("Shift+Up", "⇧↑");
  CheckLabel("Control+Alt+Delete", "⌃⌥DEL");
  CheckLabel("Numpad7", "N7");
  CheckLabel("SomethingVeryLong", "SOMETH");
  CheckLabel("", "");

  auto binds = DefaultBinds();
  ButtonLabels labels = LabelsFor(binds);
  Check(labels[0] == "BKSP", "B prefers a readable alternative");
  Check(labels[1] == "SPACE", "A prefers a readable alternative");
  Check(labels[7] == "Q", "LT is Blade Mode's Q");
  Check(labels[5] == "E", "RT is Ninja Run's E");
  Check(labels[8] == "⇧↑←↓→", "D-pad group shares the Shift prefix once");
  Check(labels[9] == "⇧↑↓", "D-pad up/down group");
  Check(labels[11] == "⇧↑", "D-pad up");
  Check(labels[15] == "↑←↓→", "RS shows the arrow keys");
  Check(labels[17] == "WASD", "LS shows WASD");
  Check(labels[19] == "X", "START");
  Check(labels[21] == "W" && labels[24] == "D", "left-stick directions");

  const ButtonLabels ps = PlayStationLabels(false);
  Check(ps[1] == "✕" && ps[0] == "○" && ps[7] == "L2" && ps[19] == "OPTIONS", "PS4/PS5 glyphs");
  Check(PlayStationLabels(true)[20] == "SELECT", "PS3 names SELECT");
  Check(ps[8].empty() && ps[21].empty(), "D-pad and stick-direction art stays original");

  binds["mnk_mouse"] = "true";
  binds["keybind_lstick_up"] = "I";
  binds["keybind_dpad_up"] = "1";
  labels = LabelsFor(binds);
  Check(labels[15] == "MOUSE", "RS follows the mouse camera");
  Check(labels[17] == "IASD", "custom left stick is spelled out");
  Check(labels[9] == "1/⇧↓", "mixed modifiers are not factored out");

  binds["keybind_y"] = "Comma";
  Check(LabelsFor(binds)[2] == ",", "punctuation is kept when it is the only bind");
  binds["keybind_x"] = "";
  Check(LabelsFor(binds)[3].empty(), "unbound buttons keep the controller icon");
}

void CheckAtlas(const std::vector<uint8_t>& original, const char* dump_path) {
  Check(original.size() == kAtlasBytes, "atlas size");
  Check(Fnv1a64(original) == kAtlasHash, "atlas hash matches kAtlasHash");
  Check(std::equal(kAtlasSignature.begin(), kAtlasSignature.end(),
                   original.begin() + kAtlasSignatureOffset),
        "signature window matches");

  // The bounding-box encoder only re-encodes icon blocks; it should stay close
  // to the original on alpha and on clearly visible color.
  const Image decoded = DecodeAtlas(original);
  const Image round_trip = DecodeAtlas(EncodeAtlas(decoded));
  int alpha_error = 0, color_error = 0;
  for (size_t i = 0; i < decoded.rgba.size(); i += 4) {
    alpha_error = std::max(alpha_error, std::abs(decoded.rgba[i + 3] - round_trip.rgba[i + 3]));
    if (decoded.rgba[i + 3] < 128) continue;  // faint texels' color barely shows
    for (int c = 0; c < 3; ++c) {
      color_error = std::max(color_error, std::abs(decoded.rgba[i + c] - round_trip.rgba[i + c]));
    }
  }
  std::printf("round-trip max error: alpha %d, visible color %d\n", alpha_error, color_error);
  Check(alpha_error <= 18 && color_error <= 32, "decode/encode round trip stays close");

  const ButtonLabels labels = LabelsFor(DefaultBinds());
  const std::vector<uint8_t> patched = PatchAtlas(original, labels, RenderKeyIcon);
  Check(patched.size() == kAtlasBytes && patched != original, "atlas is patched");
  // act_btn_sign_02 ('+') at (0,772) 34x31 is not an icon and must be preserved.
  const Image after = DecodeAtlas(patched);
  for (uint32_t y = 772; y < 772 + 31; ++y) {
    for (uint32_t x = 4; x < 32; ++x) {
      const size_t at = (size_t(y) * kAtlasWidth + x) * 4;
      Check(std::equal(after.rgba.begin() + at, after.rgba.begin() + at + 4,
                       decoded.rgba.begin() + at),
            "'+' sign survives patching");
    }
  }
  const std::vector<uint8_t> playstation =
      PatchAtlas(original, PlayStationLabels(false), RenderKeyIcon, kPlayStationRoundButtons);
  Check(playstation != original && playstation != patched, "PlayStation atlas differs");
  const ButtonLabels none{};
  Check(PatchAtlas(original, none, RenderKeyIcon) == original, "no labels leaves atlas intact");

  if (!dump_path) return;
  std::ofstream out(dump_path, std::ios::binary);
  const Image ps_image = DecodeAtlas(playstation);
  out << "P6\n" << kAtlasWidth * 3 << " " << kAtlasHeight << "\n255\n";
  for (uint32_t y = 0; y < kAtlasHeight; ++y) {
    for (const Image* image : {&decoded, &after, &ps_image}) {
      for (uint32_t x = 0; x < kAtlasWidth; ++x) {
        const uint8_t* p = image->rgba.data() + (size_t(y) * kAtlasWidth + x) * 4;
        const uint8_t bg[3] = {40, 44, 70};  // composite over a dark background
        for (int c = 0; c < 3; ++c) out.put(char((p[c] * p[3] + bg[c] * (255 - p[3])) / 255));
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  CheckLabels();
  if (argc > 1) {
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> wtb((std::istreambuf_iterator<char>(in)), {});
    Check(wtb.size() >= kWtbAtlasOffset + kAtlasBytes, "core.wtb readable");
    CheckAtlas({wtb.begin() + kWtbAtlasOffset, wtb.begin() + kWtbAtlasOffset + kAtlasBytes},
               argc > 2 ? argv[2] : nullptr);
  }
  std::puts("mgr_prompt_checks: all checks passed");
  return 0;
}
