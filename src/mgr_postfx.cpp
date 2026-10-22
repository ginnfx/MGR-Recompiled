// Guest post-processing experiments for 4B4E080A / 0.0.0.4 (see
// research/postfx-removal.md). Strong symbols override the generated weak
// entry points; __imp__sub_* is the original implementation.
//
// mgr_bloom (default on): when off, the per-scene filter draw (sub_828559D0,
// vtable 0x821DAD20 slot 1, the console counterpart of PC cFilter::draw) skips
// its four glare-level calls to sub_82853E68 (320x176, 80x48, 48x32, 48x32:
// a downsample, three blur passes and three resolves each) and its three direct
// sub_82853408 composites that read them. Skips are keyed by guest return
// address, so the trailing sub_828546F8 copy (which also calls sub_82853408)
// and all other callers are untouched. The glare targets have no other runtime
// reader (research/postfx-removal.md).
//
// The bright pass sub_828538C0 and the two quads (sub_82793BE0) that fill the
// glare source target must still run: skipping them was tried and turns the
// whole image red with white blocks (2026-10-22 captures), so that target has a
// reader beyond the glare chain.
//
// Ordering-table diagnostics (render thread, environment only):
//   MGR_OT_PROFILE=1      CPU time per render callback of the OT executor.
//   MGR_OT_SKIP=hex,...   do not call these callbacks. Research only: the
//                         skipped work's consumers are not preserved.
#include "mgr_ot_key.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

REX_EXTERN(__imp__sub_82791D88);
REX_EXTERN(__imp__sub_82853E68);
REX_EXTERN(__imp__sub_82853408);

REXCVAR_DEFINE_BOOL(mgr_bloom, true, "MGR/Graphics",
                    "Glare/bloom glow of the scene filter. Off skips the blur chain and its "
                    "composites (about 19 small passes and 12 resolves per frame); the image "
                    "loses the glow around lights and Blade Mode effects");

namespace {

using Clock = std::chrono::steady_clock;

// OT manager (sub_82794780 registers into it) and the executor's layout.
constexpr uint32_t kOtManager = 0x8352D6A8;
constexpr uint32_t kOtManagerEnabled = 100;
constexpr uint32_t kOtManagerFirst = 60;
constexpr uint32_t kOtEntryKey = 0;
constexpr uint32_t kOtEntryCallback = 8;
constexpr uint32_t kOtEntryArgument = 12;
constexpr uint32_t kOtEntryNext = 20;
constexpr uint32_t kOtFrameSize = 112;
constexpr uint32_t kOtKeyOffset = 80;  // callback r3: {u8 layer, u8 priority, u16 stale, u32 user}
constexpr uint32_t kOtCallReturn = 0x82791E38;
constexpr uint32_t kOtReportExecutions = 300;
constexpr size_t kOtReportCallbacks = 24;

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

const std::unordered_set<uint32_t>& OtSkipped() {
  static const std::unordered_set<uint32_t> skipped = [] {
    auto parsed = mgr_ot::ParseAddressList(std::getenv("MGR_OT_SKIP"));
    for (uint32_t address : parsed) {
      std::fprintf(stderr, "[mgr-ot] MGR_OT_SKIP: not calling render callback %08X\n", address);
    }
    return parsed;
  }();
  return skipped;
}

bool OtProfiling() {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_OT_PROFILE");
    return value && value[0] == '1';
  }();
  return enabled;
}

struct OtCallbackTotals {
  uint32_t key = 0;  // last sort key word: layer, priority, sub-priority
  uint64_t calls = 0;
  uint64_t ns = 0;
};

// Only the guest render thread runs the executor.
std::unordered_map<uint32_t, OtCallbackTotals> ot_totals;
uint64_t ot_executions = 0;
uint64_t ot_total_ns = 0;

void ReportOtProfile() {
  std::vector<std::pair<uint32_t, OtCallbackTotals>> ranked(ot_totals.begin(), ot_totals.end());
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& x, const auto& y) { return x.second.ns > y.second.ns; });
  const double executions = double(ot_executions);
  std::fprintf(stderr, "[mgr-ot] %llu executions: %.3f ms render callbacks per execution\n",
               static_cast<unsigned long long>(ot_executions), ot_total_ns / 1e6 / executions);
  for (size_t i = 0; i < std::min(ranked.size(), kOtReportCallbacks); ++i) {
    const auto& [callback, totals] = ranked[i];
    const mgr_ot::Key key = mgr_ot::Unpack(totals.key);
    std::fprintf(stderr, "[mgr-ot]   %7.3f ms  x%5.2f  prio %u.%u.%u  %08X\n",
                 totals.ns / 1e6 / executions, totals.calls / executions, key.layer,
                 key.priority, key.sub, callback);
  }
  ot_totals.clear();
  ot_executions = 0;
  ot_total_ns = 0;
}

// Return addresses (ctx.lr) of the calls inside sub_828559D0.
constexpr uint32_t kGlareLevelReturns[] = {0x82856324, 0x828563C0, 0x82856454, 0x828564DC};
constexpr uint32_t kGlareCompositeReturns[] = {0x828565FC, 0x82856628, 0x82856654};

template <size_t N>
bool ReturnsTo(const PPCContext& ctx, const uint32_t (&returns)[N]) {
  return std::find(std::begin(returns), std::end(returns), uint32_t(ctx.lr)) != std::end(returns);
}

template <size_t N>
bool SkipBloomCall(const PPCContext& ctx, const uint32_t (&returns)[N]) {
  return ReturnsTo(ctx, returns) && !REXCVAR_GET(mgr_bloom);
}

}  // namespace

// OT executor: walks the manager's sorted entry list and calls each
// callback(r3 = key, r4 = argument). Mirrors the guest loop exactly when a
// diagnostic is enabled; otherwise the original runs.
extern "C" REX_FUNC(sub_82791D88) {
  const bool profiling = OtProfiling();
  const auto& skipped = OtSkipped();
  if (!profiling && skipped.empty()) {
    __imp__sub_82791D88(ctx, base);
    return;
  }
  if (!LoadU32(base, kOtManager + kOtManagerEnabled)) return;
  uint32_t entry = LoadU32(base, kOtManager + kOtManagerFirst);
  if (!entry) return;
  const uint32_t caller_stack = ctx.r1.u32;
  const uint32_t frame = caller_stack - kOtFrameSize;
  StoreU32(base, frame, caller_stack);  // back chain, as stwu would write
  ctx.r1.u32 = frame;
  const uint32_t key = frame + kOtKeyOffset;
  const auto execution_start = Clock::now();
  while (entry) {
    const uint32_t current = entry;
    entry = LoadU32(base, current + kOtEntryNext);
    const uint32_t callback = LoadU32(base, current + kOtEntryCallback);
    if (!callback) continue;
    const uint32_t word = LoadU32(base, current + kOtEntryKey);
    const mgr_ot::Key fields = mgr_ot::Unpack(word);
    base[key + 0] = uint8_t(fields.layer);
    base[key + 1] = uint8_t(fields.priority);
    StoreU32(base, key + 4, fields.user);
    if (skipped.count(callback)) continue;
    ctx.last_indirect_target = callback;  // reported if the address is invalid
    PPCFunc* function = rex::runtime::ResolveIndirectFunction(callback);
    ctx.r3.u64 = key;
    ctx.r4.u64 = LoadU32(base, current + kOtEntryArgument);
    ctx.lr = kOtCallReturn;
    const auto start = Clock::now();
    function(ctx, base);
    if (profiling) {
      OtCallbackTotals& totals = ot_totals[callback];
      totals.key = word;
      ++totals.calls;
      totals.ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
  }
  ctx.r1.u32 = caller_stack;
  if (!profiling) return;
  ot_total_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - execution_start)
          .count());
  if (++ot_executions >= kOtReportExecutions) ReportOtProfile();
}

// Glare level: downsample, three blur passes and resolves (no return value).
extern "C" REX_FUNC(sub_82853E68) {
  if (SkipBloomCall(ctx, kGlareLevelReturns)) return;
  __imp__sub_82853E68(ctx, base);
}

// Filter08 composite of a glare level onto the scene (no return value).
extern "C" REX_FUNC(sub_82853408) {
  if (SkipBloomCall(ctx, kGlareCompositeReturns)) return;
  __imp__sub_82853408(ctx, base);
}

