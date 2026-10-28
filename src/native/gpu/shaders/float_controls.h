#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace mgrn::gpu {

// Marks every entry point SignedZeroInfNanPreserve for 32-bit floats, as the
// Xenos SPIR-V translator does. Without it SPIRV-Cross reports full fast-math
// flags and MoltenVK compiles the Metal library in fast mode, which assumes no
// Inf/NaN: the ±FLT_MAX clamps around rcp/rsq of zero fold away and the Inf
// reaches skinned/effect positions as long streaks (sword, eye trails). With
// the mode set MoltenVK selects MTLMathModeSafe.
// Returns true when the module was changed.
inline bool AddSignedZeroInfNanPreserve(std::vector<std::uint32_t>& words) {
  using U = std::uint32_t;
  constexpr U kMagic = 0x07230203, kHeaderWords = 5;
  constexpr U kOpExtension = 10, kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr U kOpCapability = 17;
  constexpr U kCapability = 4466, kMode = 4461, kFloatWidth = 32;
  constexpr U kVersion14 = 0x00010400;
  constexpr char kExtension[] = "SPV_KHR_float_controls";
  if (words.size() < kHeaderWords || words[0] != kMagic) return false;

  std::vector<U> entry_points;
  std::size_t capabilities_end = kHeaderWords, preamble_end = kHeaderWords;
  std::size_t modes_end = 0;
  bool has_capability = false, has_extension = false;
  for (std::size_t at = kHeaderWords; at < words.size();) {
    const U length = words[at] >> 16, op = words[at] & 0xFFFF;
    if (!length || length > words.size() - at) return false;
    if (op == kOpCapability) {
      has_capability |= length == 2 && words[at + 1] == kCapability;
      capabilities_end = at + length;
    } else if (op == kOpExtension) {
      has_extension |= (length - 1) * 4 >= sizeof(kExtension) &&
                       std::memcmp(&words[at + 1], kExtension, sizeof(kExtension)) == 0;
    } else if (op == kOpEntryPoint && length >= 3) {
      entry_points.push_back(words[at + 2]);
      modes_end = at + length;
    } else if (op == kOpExecutionMode) {
      // An entry point already preserving 32-bit Inf/NaN needs nothing.
      if (length == 4 && words[at + 2] == kMode && words[at + 3] == kFloatWidth)
        std::erase(entry_points, words[at + 1]);
      modes_end = at + length;
    }
    if (op == kOpCapability || op == kOpExtension) preamble_end = at + length;
    at += length;
  }
  if (entry_points.empty() || !modes_end) return false;

  std::vector<U> extension;
  if (!has_extension && words[1] < kVersion14) {
    const U name_words = U((sizeof(kExtension) + 3) / 4);
    extension.assign(1 + name_words, 0);
    extension[0] = ((1 + name_words) << 16) | kOpExtension;
    std::memcpy(&extension[1], kExtension, sizeof(kExtension));
  }
  std::vector<U> modes;
  for (const U id : entry_points)
    modes.insert(modes.end(), {(4u << 16) | kOpExecutionMode, id, kMode, kFloatWidth});

  // Insert back to front so the earlier offsets stay valid.
  words.insert(words.begin() + modes_end, modes.begin(), modes.end());
  words.insert(words.begin() + preamble_end, extension.begin(), extension.end());
  if (!has_capability)
    words.insert(words.begin() + capabilities_end, {(2u << 16) | kOpCapability, kCapability});
  return true;
}

} // namespace mgrn::gpu
