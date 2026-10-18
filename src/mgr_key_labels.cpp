// Keybind strings (rex/ui/keybinds.cpp names) to compact icon labels.
#include "mgr_key_labels.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace {
constexpr size_t kMaxLabelBytes = 6;

constexpr std::pair<std::string_view, std::string_view> kNames[] = {
    {"Up", "↑"},          {"Down", "↓"},        {"Left", "←"},       {"Right", "→"},
    {"Shift", "⇧"},       {"Control", "⌃"},     {"Alt", "⌥"},        {"Return", "ENTER"},
    {"Space", "SPACE"},   {"Backspace", "BKSP"}, {"Escape", "ESC"},  {"Tab", "TAB"},
    {"Semicolon", ";"},   {"Quote", "'"},       {"Comma", ","},      {"Period", "."},
    {"Slash", "/"},       {"Backslash", "\\"},  {"Minus", "-"},      {"Plus", "="},
    {"Backtick", "`"},    {"LBracket", "["},    {"RBracket", "]"},   {"Delete", "DEL"},
    {"Insert", "INS"},    {"Home", "HOME"},     {"End", "END"},      {"PageUp", "PGUP"},
    {"PageDown", "PGDN"}, {"NumpadEnter", "NENT"}, {"NumpadPlus", "N+"},
    {"NumpadMinus", "N-"}, {"NumpadStar", "N*"}, {"NumpadSlash", "N/"},
    {"CapsLock", "CAPS"}, {"PrintScreen", "PRSC"}, {"ScrollLock", "SCRL"},
    {"NumLock", "NUML"},  {"Pause", "PAUSE"},
};

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string Upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string KeyName(std::string_view key) {
  for (const auto& [name, label] : kNames) {
    if (key == name) return std::string(label);
  }
  if (key.starts_with("Numpad") && key.size() == 7) return "N" + std::string(key.substr(6));
  std::string upper = Upper(key);
  // Bound the label for unknown names; the renderer still scales to fit.
  return upper.size() <= kMaxLabelBytes ? upper : upper.substr(0, kMaxLabelBytes);
}

bool IsLonePunctuation(const std::string& label) {
  return label.size() == 1 && std::ispunct(static_cast<unsigned char>(label[0]));
}

// The comma-separated alternative to show: the first one that reads well at
// icon size ("Semicolon,Space" -> "Space"), else the first.
std::string_view Primary(std::string_view value) {
  std::string_view first;
  while (!value.empty()) {
    const size_t comma = std::min(value.find(','), value.size());
    const std::string_view bind = Trim(value.substr(0, comma));
    value.remove_prefix(std::min(comma + 1, value.size()));
    if (bind.empty()) continue;
    if (first.empty()) first = bind;
    if (!IsLonePunctuation(KeyLabel(bind))) return bind;
  }
  return first;
}

// Labels made of single symbols concatenate ("↑↓"); others need a separator.
bool IsSymbol(const std::string& label) {
  return label.size() == 1 || label == "↑" || label == "↓" || label == "←" || label == "→";
}

std::string_view ModifierPrefix(std::string_view bind) {
  size_t plus = bind.rfind('+');
  return plus == std::string_view::npos ? std::string_view() : bind.substr(0, plus + 1);
}

std::string Group(std::initializer_list<std::string_view> binds) {
  // A modifier prefix shared by every bind ("Shift+") is shown once: "⇧↑↓".
  const std::string_view prefix = ModifierPrefix(*binds.begin());
  const bool shared = !prefix.empty() &&
      std::all_of(binds.begin(), binds.end(),
                  [&](std::string_view bind) { return ModifierPrefix(bind) == prefix; });
  std::string out = shared ? KeyLabel(prefix.substr(0, prefix.size() - 1)) : "";
  bool first = true;
  for (std::string_view bind : binds) {
    std::string label = KeyLabel(shared ? bind.substr(prefix.size()) : bind);
    if (label.empty()) return "";
    if (!first && !IsSymbol(label)) out += "/";
    out += label;
    first = false;
  }
  return out;
}
}  // namespace

std::string KeyLabel(std::string_view bind) {
  bind = Trim(bind);
  if (bind.empty()) return "";
  std::string out;
  // Modifier prefixes as in mnk_input_driver: "Shift+Up".
  size_t plus;
  while ((plus = bind.find('+')) != std::string_view::npos && plus + 1 < bind.size()) {
    out += KeyName(Trim(bind.substr(0, plus)));
    bind = Trim(bind.substr(plus + 1));
  }
  return out + KeyName(bind);
}

ButtonLabels BuildButtonLabels(const CvarLookup& lookup) {
  std::string values[19];
  const char* names[19] = {"keybind_b",          "keybind_a",           "keybind_y",
                           "keybind_x",          "keybind_right_shoulder",
                           "keybind_right_trigger", "keybind_left_shoulder",
                           "keybind_left_trigger", "keybind_dpad_up",   "keybind_dpad_down",
                           "keybind_dpad_left",  "keybind_dpad_right",  "keybind_rstick_press",
                           "keybind_lstick_press", "keybind_start",     "keybind_back",
                           "keybind_lstick_up",  "keybind_lstick_down", "keybind_lstick_left"};
  for (int i = 0; i < 19; ++i) values[i] = std::string(Primary(lookup(names[i])));
  const std::string lr = std::string(Primary(lookup("keybind_lstick_right")));
  const std::string ru = std::string(Primary(lookup("keybind_rstick_up")));
  const std::string rd = std::string(Primary(lookup("keybind_rstick_down")));
  const std::string rl = std::string(Primary(lookup("keybind_rstick_left")));
  const std::string rr = std::string(Primary(lookup("keybind_rstick_right")));
  const bool mouse_camera = lookup("mnk_mouse") == "true";
  const auto& [b, a, y, x, rb, rt, lb, lt, du, dd, dl, dr, rs, ls, start, back, lu, ld, ll] =
      values;

  ButtonLabels labels;
  labels[0] = KeyLabel(b);
  labels[1] = KeyLabel(a);
  labels[2] = KeyLabel(y);
  labels[3] = KeyLabel(x);
  labels[4] = KeyLabel(rb);
  labels[5] = KeyLabel(rt);
  labels[6] = KeyLabel(lb);
  labels[7] = KeyLabel(lt);
  labels[8] = Group({du, dl, dd, dr});
  labels[9] = Group({du, dd});
  labels[10] = Group({dl, dr});
  labels[11] = KeyLabel(du);
  labels[12] = KeyLabel(dd);
  labels[13] = KeyLabel(dl);
  labels[14] = KeyLabel(dr);
  labels[15] = mouse_camera ? "MOUSE" : Group({ru, rl, rd, rr});
  labels[16] = KeyLabel(rs);
  labels[17] = lu == "W" && ll == "A" && ld == "S" && lr == "D" ? "WASD" : Group({lu, ll, ld, lr});
  labels[18] = KeyLabel(ls);
  labels[19] = KeyLabel(start);
  labels[20] = KeyLabel(back);
  labels[21] = KeyLabel(lu);
  labels[22] = KeyLabel(ld);
  labels[23] = KeyLabel(ll);
  labels[24] = KeyLabel(lr);
  return labels;
}

ButtonLabels PlayStationLabels(bool ps3) {
  ButtonLabels labels;
  labels[0] = "○";  // B
  labels[1] = "✕";  // A
  labels[2] = "△";  // Y
  labels[3] = "□";  // X
  labels[4] = "R1";
  labels[5] = "R2";
  labels[6] = "L1";
  labels[7] = "L2";
  labels[15] = "R";
  labels[16] = "R3";
  labels[17] = "L";
  labels[18] = "L3";
  labels[19] = ps3 ? "START" : "OPTIONS";
  labels[20] = ps3 ? "SELECT" : "SHARE";
  return labels;
}
