// Guest-memory accessors shared by the Options menu modules. Guest memory is big
// endian; REX_RAW_ADDR expands to `base + ...`, so `base` is always used.
#pragma once

#include "generated/default/mgr_pch.h"

#include <cstdint>
#include <cstring>

namespace mgr_options_guest {

// Node table of a UI owner: node array at ui+120 (1024 bytes each), count at ui+124.
constexpr uint32_t kNodeStride = 1024;

inline uint8_t* Guest(uint8_t* base, uint32_t a) { return REX_RAW_ADDR(a); }
inline const uint8_t* Guest(const uint8_t* base, uint32_t a) { return REX_RAW_ADDR(a); }
inline uint32_t U32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Guest(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint16_t U16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Guest(base, a), 2);
  return __builtin_bswap16(v);
}
inline void W32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Guest(base, a), &v, 4);
}
inline void W16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(Guest(base, a), &v, 2);
}

// Calls a recompiled guest function with up to three integer arguments.
inline uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t a = 0, uint32_t b = 0,
                     uint32_t c = 0) {
  PPCContext call = ctx;
  call.r3.u64 = a;
  call.r4.u64 = b;
  call.r5.u64 = c;
  fn(call, base);
  return call.r3.u32;
}

// Guest address of node `id` of `owner`'s UI, or 0 when the UI is not loaded.
inline uint32_t Node(uint8_t* base, uint32_t owner, uint32_t id) {
  const uint32_t ui = U32(base, owner + 24);
  if (!ui || id >= U32(base, ui + 124)) return 0;
  return U32(base, ui + 120) + id * kNodeStride;
}

}  // namespace mgr_options_guest
