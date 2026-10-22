// Ordering-table (render work list) sort keys of 4B4E080A / 0.0.0.4.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <unordered_set>

namespace mgr_ot {

struct Key {
  uint32_t layer;     // 0..3
  uint32_t priority;  // 0..127
  uint32_t sub;       // 0..7 (the registrar accepts 0..2)
  uint32_t user;      // 20 bits
  bool operator==(const Key&) const = default;
};

// Key word as sub_82794780 builds it (three rlwimi steps):
// user bits 0..19, sub-priority 20..22, priority 23..29, layer 30..31.
constexpr uint32_t Pack(const Key& key) {
  const uint32_t layer_priority = ((key.layer << 7) & 0xFFFFFF80u) | (key.priority & 0x7Fu);
  const uint32_t order = ((layer_priority << 3) & 0xFFFFFFF8u) | (key.sub & 0x7u);
  return ((order << 20) & 0xFFF00000u) | (key.user & 0xFFFFFu);
}

// The fields the executor (sub_82791D88) hands its callbacks, plus `sub`.
constexpr Key Unpack(uint32_t word) {
  return {word >> 30, (word >> 23) & 0x7Fu, (word >> 20) & 0x7u, word & 0xFFFFFu};
}

// "82BEB018,828559D0" (one separator character between entries) -> addresses;
// stops at the first token that is not hexadecimal. Null means none.
inline std::unordered_set<uint32_t> ParseAddressList(const char* text) {
  std::unordered_set<uint32_t> addresses;
  while (text && *text) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (end == text) break;
    addresses.insert(uint32_t(value));
    text = *end ? end + 1 : end;
  }
  return addresses;
}

}  // namespace mgr_ot
