// Guest address-map policy shared by the native fast paths that access guest
// memory directly with host loads, stores and memcpy.
#pragma once
#include <cstdint>

// True when [address, address + size) is ordinary virtual memory: below the
// physical RAM views at 0xa0000000 and up (multiple aliases, plus an ARM
// host-page offset at 0xe0000000) and outside the 0x7f000000 MMIO window
// (mgr_pch.h). Anything else stays on the generated memory-access path.
inline bool MgrIsOrdinaryGuestRange(uint32_t address, uint64_t size) {
  constexpr uint64_t kPhysicalViews = 0xa0000000ull;
  constexpr uint64_t kMmioBegin = 0x7f000000ull, kMmioEnd = 0x80000000ull;
  const uint64_t end = uint64_t(address) + size;
  return end <= kPhysicalViews && !(address < kMmioEnd && kMmioBegin < end);
}
