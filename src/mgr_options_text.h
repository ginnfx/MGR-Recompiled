// Private MCD messages for the Options UI text nodes. They use the loaded game's
// own glyphs and texture bindings; the retail context pointers are put back by
// Restore() before the retail handlers or destructors run.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace mgr_options_text {

constexpr uint16_t kValueFont = 3;
constexpr uint16_t kLabelFont = 8;

struct NativeText {
  uint32_t widget = 0, original_context = 0, original_index = 0, original_line = 0, original_alignment = 0;
  uint32_t allocation = 0, mcd = 0, paragraph = 0, line = 0, letters = 0;
  std::unordered_map<char, uint16_t> glyphs;
  uint16_t font = kValueFont;
  std::string shown;
};

class TextOverrides {
 public:
  // Shows `value` in node `id` of `owner`. `menu_object` is the cOptionMenu (its
  // legend node supplies the core font context). Returns false when the node is
  // not a text widget or the UI resources are not loaded yet.
  bool Set(uint8_t* base, uint32_t menu_object, uint32_t owner, uint32_t id, const std::string& value,
           uint16_t font = kValueFont);
  // Points every overridden widget back at its retail text.
  void Restore(uint8_t* base);
  // Releases the guest allocations without touching widgets (the session ended).
  void Free();

 private:
  bool Attach(uint8_t* base, uint32_t menu_object, uint32_t widget, uint16_t font);
  static void Fill(uint8_t* base, NativeText& text, const std::string& value);

  std::unordered_map<uint32_t, NativeText> text_;
};

}  // namespace mgr_options_text
