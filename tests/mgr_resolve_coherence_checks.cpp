#include "mgr_resolve_coherence.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>

int main() {
  mgr::ResolveCoherence<32> cache;
  assert(!cache.IsCurrent(1, 0, 4, 2, 8));
  cache.MarkCurrent(1, 0, 4, 2, 8);
  assert(cache.IsCurrent(1, 0, 4, 2, 8));
  assert(!cache.IsCurrent(2, 0, 4, 2, 8));
  assert(!cache.IsCurrent(1, 0, 5, 2, 8));
  cache.InvalidateOwner(2);
  assert(cache.IsCurrent(1, 0, 4, 2, 8));
  cache.InvalidateOwner(1);
  assert(!cache.IsCurrent(1, 0, 4, 2, 8));
  cache.MarkCurrent(1, 30, 4, 2, 8);
  assert(cache.IsCurrent(1, 30, 4, 2, 8));
  // Same wrapped EDRAM address, different nonwrapped source-image period.
  assert(!cache.IsCurrent(1, 62, 4, 2, 8));
  cache.MarkCurrent(2, 0, 1, 1, 8);
  assert(!cache.IsCurrent(1, 30, 4, 2, 8));
  cache.MarkCurrent(1, UINT32_MAX, 2, 2, 8);
  assert(cache.IsCurrent(1, UINT32_MAX, 2, 2, 8));
  assert(!cache.IsCurrent(1, 31, 2, 2, 8));
  cache.InvalidateAll();
  assert(!cache.IsCurrent(1, 31, 2, 2, 8));
  cache.MarkCurrent(1, 31, 2, 2, 8);
  cache.Reset();
  assert(!cache.IsCurrent(1, 31, 2, 2, 8));
  for (auto dims : {std::array<uint32_t, 3>{0, 1, 8}, {1, 0, 8},
                    {1, 1, 0}, {9, 1, 8}, {33, 1, 40}, {1, 33, 8}, {1, 5, 8}}) {
    cache.MarkCurrent(1, 0, dims[0], dims[1], dims[2]);
    assert(!cache.IsCurrent(1, 0, dims[0], dims[1], dims[2]));
  }
  cache.MarkCurrent(0, 0, 1, 1, 8);
  assert(!cache.IsCurrent(0, 0, 1, 1, 8));

  // Exercise rollover cheaply; all owners must lose old stamps.
  mgr::ResolveCoherence<32, uint8_t> small;
  small.MarkCurrent(2, 5, 1, 1, 8);
  for (unsigned i = 0; i < 255; ++i) small.InvalidateOwner(1);
  assert(!small.IsCurrent(2, 5, 1, 1, 8));
  small.MarkCurrent(1, 0, 1, 1, 8);
  assert(small.IsCurrent(1, 0, 1, 1, 8));

  // Differential model: reference always copies every source tile; optimized
  // copies only on a miss. Owners may change, alias EDRAM and write unrelated
  // tiles, and arbitrary external EDRAM writers invalidate all metadata.
  cache.Reset();
  std::array<std::array<uint64_t, 32>, 5> source{};
  std::array<uint64_t, 32> baseline{}, optimized{};
  std::mt19937 random(0x360);
  uint64_t serial = 1, skips = 0, dispatches = 0;
  for (auto& owner : source) for (auto& tile : owner) tile = serial++;
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    unsigned owner = random() % source.size();
    if (random() % 7 == 0) {
      cache.InvalidateOwner(owner + 1);
      for (auto& tile : source[owner]) if (random() & 1) tile = serial++;
    }
    if (random() % 53 == 0) {
      cache.InvalidateAll();
      unsigned tile = random() % 32;
      baseline[tile] = optimized[tile] = serial++;
    }
    unsigned base = random() % 64, pitch = 1 + random() % 16;
    unsigned width = 1 + random() % pitch, height = 1 + random() % 8;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      bool skip = cache.IsCurrent(owner + 1, base, width, height, pitch);
      for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        unsigned tile = (base + y * pitch + x) & 31;
        // Distinct nonwrapped periods address distinct source-image rows.
        const uint64_t source_value = source[owner][tile] ^
            (uint64_t((base + y * pitch + x) / 32) * 0x9E3779B97F4A7C15ULL);
        baseline[tile] = source_value;
        if (!skip) optimized[tile] = source_value;
      }
      if (!skip) cache.MarkCurrent(owner + 1, base, width, height, pitch);
      skips += skip;
      dispatches += !skip;
      assert(baseline == optimized);
    }
  }
  std::cout << "resolve coherence: 200000 differential dispatches passed; "
            << skips << " skipped, " << dispatches << " issued; invalidation, "
            << "owner aliasing, wrap, bounds and rollover passed\n";
}
