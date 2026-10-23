// Model LOD bias (mgr_lod) for 4B4E080A / 0.0.0.4. Strong symbols override the
// generated weak entry points; __imp__sub_* is the original implementation.
//
// sub_82BFCD88 runs per model per frame: it stores the size metric at lod+16
// and asks sub_82BF8758 for the level (lod+24). The block is set up by
// sub_82BF8848 from OBJ_LOD0-3 (lod.bxm via sub_82BC8A08 / sub_82C10F30).
// Child models copy their parent's level and never reach the chooser. Only
// the chooser's inputs change here, so the stored metric and the game's
// hysteresis (+-10%, cull 0.85 * OBJ_LOD3) are untouched.
//
// MGR_LOD_TRACE=1 (environment) logs, per kTraceCalls chooser calls, how many
// returned each level (culled, 0, 1, 2) and how many were for a model with
// more than one level (block +28, from model data +52).
#include "mgr_lod_bias.h"
#include "mgr_settings.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_82BF8758);

namespace {
// Read on the guest thread per model; written by the settings callback.
std::atomic<mgr_lod::Mode> lod_mode{mgr_lod::Mode::kGame};

constexpr uint32_t kMaxLevelOffset = 28;
constexpr uint32_t kTraceCalls = 20000;
constexpr int kTraceLevels = 3;

bool Tracing() {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_LOD_TRACE");
    return value && value[0] == '1';
  }();
  return enabled;
}

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

// Several guest threads choose levels. Slot 0 counts culls (-1); levels above
// 2 are clamped. Counts are approximate across a report boundary.
void TraceLevel(mgr_lod::Mode mode, int32_t level, uint32_t max_level) {
  static std::array<std::atomic<uint32_t>, kTraceLevels + 1> counts{};
  static std::atomic<uint32_t> multi_level{0};
  static std::atomic<uint32_t> calls{0};
  counts[level < 0 ? 0 : std::min(level, kTraceLevels - 1) + 1].fetch_add(1, std::memory_order_relaxed);
  if (max_level > 0) multi_level.fetch_add(1, std::memory_order_relaxed);
  if (calls.fetch_add(1, std::memory_order_relaxed) + 1 != kTraceCalls) return;
  std::fprintf(stderr,
               "[mgr-lod] mode %d: culled %u, level0 %u, level1 %u, level2 %u; multi-level %u\n",
               int(mode), counts[0].exchange(0), counts[1].exchange(0), counts[2].exchange(0),
               counts[3].exchange(0), multi_level.exchange(0));
  calls.store(0, std::memory_order_relaxed);
}
}  // namespace

void MgrApplyLod(std::string_view value) {
  lod_mode.store(mgr_lod::Parse(value), std::memory_order_relaxed);
}

// LOD level chooser: r3 = LOD block, f1 = camera distance, f2 = size metric,
// r6 = current level (-1 culled). Returns the level, or -1 to cull.
extern "C" REX_FUNC(sub_82BF8758) {
  const mgr_lod::Mode mode = lod_mode.load(std::memory_order_relaxed);
  const uint32_t block = ctx.r3.u32;
  if (mode != mgr_lod::Mode::kGame) {
    const mgr_lod::Inputs scaled =
        mgr_lod::Apply(mode, {float(ctx.f1.f64), float(ctx.f2.f64)});
    ctx.f1.f64 = scaled.distance;
    ctx.f2.f64 = scaled.metric;
  }
  __imp__sub_82BF8758(ctx, base);
  if (Tracing()) TraceLevel(mode, ctx.r3.s32, LoadU32(base, block + kMaxLevelOffset));
}
