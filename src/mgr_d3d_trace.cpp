// Opt-in timing bridge for the statically linked XDK D3D entry points
// identified in research/native-renderer-mgr.md. Calls always reach the guest
// implementation; this gathers per-present call and inclusive-time evidence
// before any native renderer starts replacing them.

#include "mgr_native_math.h"
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

REX_EXTERN(__imp__sub_826CC0E0);
REX_EXTERN(__imp__sub_826AB228);
REX_EXTERN(__imp__sub_826AA720);
REX_EXTERN(__imp__sub_826B69D8);
REX_EXTERN(__imp__sub_826CC068);
REX_EXTERN(__imp__sub_826B6A18);
REX_EXTERN(__imp__sub_826CC428);
REX_EXTERN(__imp__sub_826B6310);

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kEntryCount = 8;
constexpr uint64_t kReportEveryPresents = 120;
constexpr const char* kNames[kEntryCount] = {
    "826CC0E0", "826AB228", "826AA720", "826B69D8",
    "826CC068", "826B6A18", "826CC428", "826B6310"};

struct Totals {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> inclusive_ns{0};
};
std::array<Totals, kEntryCount> totals{};

bool TraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_D3D_TRACE");
    return value && value[0] == '1';
  }();
  return enabled;
}

void AddTime(size_t entry, Clock::time_point start) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - start).count();
  totals[entry].calls.fetch_add(1, std::memory_order_relaxed);
  totals[entry].inclusive_ns.fetch_add(static_cast<uint64_t>(elapsed),
                                       std::memory_order_relaxed);
}

uint64_t present_count = 0;  // VdSwap runs on the guest render thread.

}  // namespace

// Called by the existing VdSwap wrapper, so report windows follow guest frames
// rather than assuming a D3D helper is called exactly once per present.
void MgrD3DTraceOnPresent() {
  MgrNativeMathOnPresent();
  if (!TraceEnabled() || ++present_count % kReportEveryPresents) return;

  std::fprintf(stderr, "[mgr-d3d] average calls/frame over %llu presents:",
               static_cast<unsigned long long>(kReportEveryPresents));
  for (size_t i = 0; i < kEntryCount; ++i) {
    const uint64_t calls = totals[i].calls.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = totals[i].inclusive_ns.exchange(0, std::memory_order_relaxed);
    const double average_us = calls ? double(ns) / double(calls) / 1000.0 : 0.0;
    const double calls_per_present = double(calls) / double(kReportEveryPresents);
    std::fprintf(stderr, " %s=%.2f/%.2fus", kNames[i], calls_per_present,
                 average_us);
  }
  std::fputc('\n', stderr);
}

extern "C" REX_FUNC(sub_826CC0E0) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    MgrDispatchNativeMath(MgrNativeMathOp::TransformNormal, ctx, base, __imp__sub_826CC0E0);
    AddTime(0, start);
  } else {
    MgrDispatchNativeMath(MgrNativeMathOp::TransformNormal, ctx, base, __imp__sub_826CC0E0);
  }
}

extern "C" REX_FUNC(sub_826AB228) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826AB228(ctx, base);
    AddTime(1, start);
  } else {
    __imp__sub_826AB228(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826AA720) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826AA720(ctx, base);
    AddTime(2, start);
  } else {
    __imp__sub_826AA720(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826B69D8) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826B69D8(ctx, base);
    AddTime(3, start);
  } else {
    __imp__sub_826B69D8(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826CC068) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826CC068(ctx, base);
    AddTime(4, start);
  } else {
    __imp__sub_826CC068(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826B6A18) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826B6A18(ctx, base);
    AddTime(5, start);
  } else {
    __imp__sub_826B6A18(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826CC428) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826CC428(ctx, base);
    AddTime(6, start);
  } else {
    __imp__sub_826CC428(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826B6310) {
  if (TraceEnabled()) {
    const auto start = Clock::now();
    __imp__sub_826B6310(ctx, base);
    AddTime(7, start);
  } else {
    __imp__sub_826B6310(ctx, base);
  }
}
