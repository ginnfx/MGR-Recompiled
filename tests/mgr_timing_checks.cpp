// Deterministic hook checks, not a substitute for gameplay verification.
// Run separately with: mgr_timing_checks enabled|disabled|forced
#include "mgr_timing.h"
#include <rex/chrono/clock.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <sys/mman.h>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

REX_EXTERN(sub_828A35B0);
REX_EXTERN(sub_826B3050);
REX_EXTERN(sub_826B1C88);

namespace {
uint64_t tick = 1000000;
constexpr uint32_t state = 0x8375DC38;
uint32_t seen_interval = 0;
uint32_t poll_result = 1;
unsigned poll_calls = 0;

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void Put32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}
uint32_t Get32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}
void PutFloat(uint8_t* base, uint32_t address, float value) {
  Put32(base, address, std::bit_cast<uint32_t>(value));
}
float Scale(const uint8_t* base) {
  return std::bit_cast<float>(Get32(base, state + 124));
}
}  // namespace

// Supply deterministic guest time; no wall-clock timing assumptions.
uint64_t rex::chrono::Clock::QueryGuestTickCount() { return tick; }
uint64_t rex::chrono::Clock::guest_tick_frequency() { return 50000000; }

// Original-entry stubs verify forwarding and preservation of original outputs.
extern "C" REX_FUNC(__imp__sub_828A35B0) {
  PutFloat(base, state + 124, ctx.r3.u32 ? 2.0f : float(ctx.f1.f64));
  PutFloat(base, state + 128, 100.0f);
  Put32(base, state + 144, 99);
  ctx.r3.u64 = 77;
  ctx.lr = 0x12345678;
}
extern "C" REX_FUNC(__imp__sub_826B3050) {
  seen_interval = Get32(base, ctx.r3.u32 + 14000);
  ctx.r3.u64 = 88;
}
extern "C" REX_FUNC(__imp__sub_826B1C88) {
  ++poll_calls;
  ctx.r3.u64 = poll_result;
}

int main(int argc, char** argv) {
  Check(argc == 2, "expected enabled, disabled, or forced");
  const bool enabled = std::strcmp(argv[1], "disabled") != 0;
  const bool forced = std::strcmp(argv[1], "forced") == 0;
  setenv("MGR_EXPERIMENTAL_FPS", enabled ? "120" : "0", 1);
  setenv("MGR_TIMING_PACE", "0", 1);
  setenv("MGR_FRAME_SCALE_FORCE_ELAPSED", forced ? "1" : "0", 1);
  auto* base = static_cast<uint8_t*>(mmap(nullptr, size_t{1} << 32,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  Check(base != MAP_FAILED, "sparse guest address space allocation");
  PutFloat(base, state + 136, 0.06f);
  PPCContext ctx{};
  auto step = [&](uint64_t ticks, bool compensate) {
    tick += ticks;
    ctx.r3.u64 = compensate;
    ctx.f1.f64 = 1;
    sub_828A35B0(ctx, base);
    Check(ctx.r3.u64 == 77 && ctx.lr == 0x12345678,
          "original register outputs preserved");
    Check(Get32(base, state + 144) == 99, "original accumulator state preserved");
    return Scale(base);
  };
  Check(step(0, true) == 2, "first sample preserves original");
  Check(std::abs(step(416667, true) - (enabled ? 0.5f : 2.0f)) < 0.00001f,
        "120-Hz fractional scale");
  Check(std::abs(step(833333, true) - (enabled ? 1.0f : 2.0f)) < 0.00001f,
        "60-Hz scale");
  Check(step(50000000, true) == 2, "upper catch-up bound retained");
  Check(std::abs(step(416667, false) - (enabled && forced ? 0.5f : 1.0f)) < 0.00001f,
        "forced-1 mode respected unless explicitly overridden");
  Check(step(0, true) == 2, "duplicate timestamp preserves original");
  PutFloat(base, state + 128, 0);
  Check(step(416667, true) == 2, "guest frame-state reset discards stale delta");
  Put32(base, 0x836DAFEC, 1234);  // change low word of guest clock origin
  Check(step(416667, true) == 2, "guest clock-origin reset discards stale delta");

  // Exercise a countdown consuming the shared scale at two update rates.
  for (int rate : {60, 120}) {
    double total = 0;
    const uint64_t start = tick;
    for (int i = 1; i <= rate; ++i) {
      const uint64_t next = start + uint64_t(i) * 50000000 / rate;
      total += step(next - tick, true);
    }
    Check(std::abs(total - (enabled ? 60.0 : 2.0 * rate)) < 0.0001,
          "one elapsed second accumulates 60 frame units with fractional scaling");
  }

  constexpr uint32_t device = 0x10000;
  for (uint32_t interval : {1u, 2u, 3u, 4u}) {
    Put32(base, device + 14000, interval);
    ctx.r3.u64 = device;
    sub_826B3050(ctx, base);
    Check(seen_interval == (enabled && interval == 2 ? 1 : interval),
          "only interval 2 is reduced; original present always executes");
    Check(ctx.r3.u64 == 88, "original present return preserved");
  }
  constexpr uint32_t wait = 0x20000;
  for (int i = 0; i < 300; ++i) {
    ctx.r3.u64 = wait;
    sub_826B1C88(ctx, base);
    Check(ctx.r3.u64 == 1, "poll result preserved through backoff");
  }
  poll_result = 0;
  ctx.r3.u64 = wait;
  sub_826B1C88(ctx, base);
  Check(ctx.r3.u64 == 0 && poll_calls == 301, "poll failure and call count preserved");
  munmap(base, size_t{1} << 32);
  std::printf("PASS: %s timing hook checks\n", argv[1]);
}
