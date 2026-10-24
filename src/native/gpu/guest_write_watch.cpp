// Native renderer: guest memory write tracking.

#include "gpu/guest_write_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"

namespace mgrn::gpu::write_watch {

namespace {

constexpr u32 kPhysicalSize = 512u << 20;
constexpr u32 kPageShift = 12;
constexpr u32 kPageCount = kPhysicalSize >> kPageShift;
// Pages are also stamped per 1 MiB region, so a check over a large range only
// walks the pages of regions written since its stamp.
constexpr u32 kRegionShift = 20;
constexpr u32 kRegionCount = kPhysicalSize >> kRegionShift;

struct Generations {
  std::array<std::atomic<u32>, kPageCount> page{};
  std::array<std::atomic<u32>, kRegionCount> region{};
  // Advanced by every trapped write. Starts above kUnwatched.
  std::atomic<u32> current{1};
};

Generations& generations() {
  static auto* g = new Generations();
  return *g;
}

std::once_flag g_register_once;

// Runs on the writing guest thread inside the access violation handler, with
// the runtime's global critical region held: atomics only.
std::pair<u32, u32> OnGuestWrite(void*, u32 physical_start, u32 length, bool) {
  auto& g = generations();
  const u32 stamp = g.current.fetch_add(1, std::memory_order_acq_rel) + 1;
  const u32 end = std::min<u64>(u64(physical_start) + std::max(length, 1u), kPhysicalSize);
  for (u32 p = physical_start >> kPageShift; p < ((end - 1) >> kPageShift) + 1; ++p)
    g.page[p].store(stamp, std::memory_order_release);
  for (u32 r = physical_start >> kRegionShift; r < ((end - 1) >> kRegionShift) + 1; ++r)
    g.region[r].store(stamp, std::memory_order_release);
  // Unwatch only what was written: the rest stays armed for its owners.
  return {physical_start, length};
}

rex::memory::Memory* RuntimeMemory() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return nullptr;
  std::call_once(g_register_once, [memory] {
    memory->RegisterPhysicalMemoryInvalidationCallback(OnGuestWrite, nullptr);
  });
  return memory;
}

// The physical range behind a guest VA range, or false when it is not in a
// physical heap.
bool PhysicalRange(rex::memory::Memory* memory, u32 va, u32 size, u32& start, u32& end) {
  if (!size) return false;
  start = memory->GetPhysicalAddress(va);
  if (start == UINT32_MAX || start >= kPhysicalSize) return false;
  end = u32(std::min<u64>(u64(start) + size, kPhysicalSize));
  return true;
}

}  // namespace

Stamp Arm(u32 va, u32 size) {
  auto* memory = RuntimeMemory();
  u32 start, end;
  if (!memory || !PhysicalRange(memory, va, size, start, end)) return kUnwatched;
  // Stamp first: a write trapped between protecting and returning must carry a
  // newer generation than the stamp, or it is never seen (its page is left
  // unprotected, so later writes do not trap either).
  const Stamp stamp = generations().current.load(std::memory_order_acquire);
  memory->EnablePhysicalMemoryAccessCallbacks(start, end - start, true, false);
  return stamp;
}

bool WrittenSince(u32 va, u32 size, Stamp stamp) {
  if (stamp == kUnwatched) return false;
  auto& g = generations();
  if (g.current.load(std::memory_order_acquire) == stamp) return false;
  auto* memory = REX_KERNEL_MEMORY();
  u32 start, end;
  if (!memory || !PhysicalRange(memory, va, size, start, end)) return false;
  for (u32 r = start >> kRegionShift; r <= (end - 1) >> kRegionShift; ++r) {
    if (g.region[r].load(std::memory_order_acquire) <= stamp) continue;
    const u32 first = std::max(start, r << kRegionShift) >> kPageShift;
    const u32 last = (std::min(end, (r + 1) << kRegionShift) - 1) >> kPageShift;
    for (u32 p = first; p <= last; ++p)
      if (g.page[p].load(std::memory_order_acquire) > stamp) return true;
  }
  return false;
}

}  // namespace mgrn::gpu::write_watch
