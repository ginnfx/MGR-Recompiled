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

// Traced guest functions, in report order. 8278E640 is the engine busy-wait
// until the GPU has consumed all submitted work; 82791828 is the engine
// function that waits on it and then presents.
#define MGR_D3D_TRACED(X)                                                      \
  X(826CC0E0) X(826AB228) X(826AA720) X(826B69D8) X(826CC068) X(826B6A18)    \
  X(826CC428) X(826B6310) X(8278E640) X(82791828)

#define MGR_D3D_EXTERN(address) REX_EXTERN(__imp__sub_##address);
MGR_D3D_TRACED(MGR_D3D_EXTERN)
#undef MGR_D3D_EXTERN

namespace {

using Clock = std::chrono::steady_clock;
enum Entry : size_t {
#define MGR_D3D_ENTRY(address) kEntry##address,
  MGR_D3D_TRACED(MGR_D3D_ENTRY)
#undef MGR_D3D_ENTRY
  kEntryCount
};
constexpr uint64_t kReportEveryPresents = 120;
constexpr const char* kNames[kEntryCount] = {
#define MGR_D3D_NAME(address) #address,
    MGR_D3D_TRACED(MGR_D3D_NAME)
#undef MGR_D3D_NAME
};

struct Totals {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> inclusive_ns{0};
  std::atomic<uint64_t> max_ns{0};
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
  uint64_t max = totals[entry].max_ns.load(std::memory_order_relaxed);
  while (uint64_t(elapsed) > max &&
         !totals[entry].max_ns.compare_exchange_weak(max, uint64_t(elapsed),
                                                     std::memory_order_relaxed)) {
  }
}

// Runs `call`, adding its inclusive time to `entry` when tracing is enabled.
template <class Call>
void Timed(Entry entry, Call&& call) {
  if (!TraceEnabled()) {
    call();
    return;
  }
  const auto start = Clock::now();
  call();
  AddTime(entry, start);
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
    const uint64_t max_ns = totals[i].max_ns.exchange(0, std::memory_order_relaxed);
    const double average_us = calls ? double(ns) / double(calls) / 1000.0 : 0.0;
    const double calls_per_present = double(calls) / double(kReportEveryPresents);
    std::fprintf(stderr, " %s=%.2f/%.2fus/max%.1fms", kNames[i], calls_per_present,
                 average_us, double(max_ns) / 1e6);
  }
  std::fputc('\n', stderr);
}

extern "C" REX_FUNC(sub_826CC0E0) {
  Timed(kEntry826CC0E0, [&] {
    MgrDispatchNativeMath(MgrNativeMathOp::TransformNormal, ctx, base, __imp__sub_826CC0E0);
  });
}

#define MGR_D3D_WRAP(address)                                                  \
  extern "C" REX_FUNC(sub_##address) {                                         \
    Timed(kEntry##address, [&] { __imp__sub_##address(ctx, base); });          \
  }
MGR_D3D_WRAP(826AB228)
MGR_D3D_WRAP(826AA720)
MGR_D3D_WRAP(826B69D8)
MGR_D3D_WRAP(826CC068)
MGR_D3D_WRAP(826B6A18)
MGR_D3D_WRAP(826CC428)
MGR_D3D_WRAP(826B6310)
MGR_D3D_WRAP(8278E640)
MGR_D3D_WRAP(82791828)
#undef MGR_D3D_WRAP
