#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace mgr {

// CPU metadata for copies of host render-target contents into EDRAM. A tile is
// reusable only while both its source identity and source write generation
// remain unchanged. It never suppresses the guest resolve into shared memory.
// Command-processor thread only; GPU work remains ordered by existing barriers.
template <size_t TileCount, typename Generation = uint64_t>
class ResolveCoherence {
 public:
  using Owner = uintptr_t;
  static_assert(TileCount && !(TileCount & (TileCount - 1)));
  static_assert(std::is_unsigned_v<Generation>);

  void Reset() {
    InvalidateAll();
    generations_.clear();
  }

  void InvalidateAll() { stamps_.fill({}); }

  void InvalidateOwner(Owner owner) {
    if (!owner) return;
    auto [it, inserted] = generations_.try_emplace(owner, Generation(1));
    if (it->second == std::numeric_limits<Generation>::max()) {
      // No old stamp may become current again when a generation wraps.
      Reset();
      return;
    }
    ++it->second;
  }

  bool IsCurrent(Owner owner, uint32_t base, uint32_t width, uint32_t height,
                 uint32_t pitch) const {
    if (!Valid(owner, width, height, pitch)) return false;
    const Generation generation = CurrentGeneration(owner);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const Stamp& stamp = stamps_[Tile(base, x, y, pitch)];
        if (stamp.owner != owner || stamp.generation != generation ||
            stamp.source_tile != SourceTile(base, x, y, pitch)) return false;
      }
    }
    return true;
  }

  void MarkCurrent(Owner owner, uint32_t base, uint32_t width, uint32_t height,
                   uint32_t pitch) {
    if (!Valid(owner, width, height, pitch)) {
      // This is called after an original-path GPU write. An uncacheable
      // dispatch may still overwrite previously cached physical tiles.
      InvalidateAll();
      return;
    }
    const Generation generation = CurrentGeneration(owner);
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
        stamps_[Tile(base, x, y, pitch)] =
            Stamp{owner, generation, SourceTile(base, x, y, pitch)};
  }

 private:
  struct Stamp {
    Owner owner = 0;
    Generation generation = 0;
    // The existing shader wraps the destination address, but computes source
    // image coordinates from the nonwrapped tile minus the RT base. Preserve
    // that distinction rather than assuming two addressing periods alias.
    uint64_t source_tile = 0;
  };
  static bool Valid(Owner owner, uint32_t width, uint32_t height, uint32_t pitch) {
    return owner && width && height && pitch && width <= pitch &&
           width <= TileCount && height <= TileCount &&
           uint64_t(height - 1) * pitch + width <= TileCount;
  }
  static size_t Tile(uint32_t base, uint32_t x, uint32_t y, uint32_t pitch) {
    return SourceTile(base, x, y, pitch) & (TileCount - 1);
  }
  static uint64_t SourceTile(uint32_t base, uint32_t x, uint32_t y, uint32_t pitch) {
    return uint64_t(base) + uint64_t(y) * pitch + x;
  }
  Generation CurrentGeneration(Owner owner) const {
    auto it = generations_.find(owner);
    return it == generations_.end() ? Generation(1) : it->second;
  }
  std::array<Stamp, TileCount> stamps_{};
  std::unordered_map<Owner, Generation> generations_;
};

}  // namespace mgr
