// Rows of the custom Options pages (Video, Misc): labels, help and the choices
// each cvar cycles through.
#pragma once

#include <span>

namespace mgr_options_rows {

struct Choice {
  const char* label;
  const char* value;
};
// Renderer an option applies to; rows for the other renderer are hidden.
enum class Renderer { Any, Native, Xenos };
struct Option {
  const char* key;
  const char* label;
  const char* help;
  std::span<const Choice> choices;
  Renderer renderer = Renderer::Any;
};

extern const Option kInternalResolution;
// Internal resolution scale choices (also used to name the active resolution).
extern const std::span<const Choice> kScale;

// The rows that apply to this run's renderer (fixed at launch), computed once.
std::span<const Option> VideoRows();
std::span<const Option> MiscRows();

}  // namespace mgr_options_rows
