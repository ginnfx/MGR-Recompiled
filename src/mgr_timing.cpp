// Opt-in timing experiment for the 4B4E080A / 0.0.0.4 guest image.
// Strong symbols override the generated weak entry points; __imp__sub_* is
// always the original generated implementation, including synchronization.
#include "mgr_timing.h"

#include <rex/chrono/clock.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

REX_EXTERN(__imp__sub_828A35B0);
REX_EXTERN(__imp__sub_826B3050);
REX_EXTERN(__imp__sub_826B1C88);

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kState = 0x8375DC38;
constexpr uint32_t kStartTicks = 0x836DAFE8;
constexpr uint32_t kFrameCounter = 0x836DAFF4;
constexpr uint32_t kFlags = 0x836D2910;
// Graphics device: integer refresh rate used by the flip phase calculation in
// 0x826B26E8 (0 means 60). Nothing in the game writes it.
constexpr uint32_t kDeviceRefreshHz = 21964;
constexpr uint32_t kDeviceSwapInterval = 14000;

// Publish mode, refresh and a transition generation together. The generation
// discards stale deltas even if 120 -> 60 -> 120 occurs between guest updates.
std::atomic<uint64_t>& Configuration() {
  static std::atomic<uint64_t> configuration{[]() -> uint64_t {
    const char* value = std::getenv("MGR_EXPERIMENTAL_FPS");
    return value && std::strcmp(value, "120") == 0
               ? (uint64_t(120) << 8) | uint64_t(MgrTimingMode::kCap120)
               : 0;
  }()};
  return configuration;
}

MgrTimingMode CurrentMode() {
  return MgrTimingMode(Configuration().load(std::memory_order_relaxed) & 0xFF);
}

bool Enabled() { return CurrentMode() != MgrTimingMode::kOriginal; }

bool Option(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  return value ? std::strcmp(value, "1") == 0 : fallback;
}

uint32_t U32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

uint64_t U64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap64(value);
}

float F32(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(U32(base, address));
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

void StoreF32(uint8_t* base, uint32_t address, float value) {
  StoreU32(base, address, std::bit_cast<uint32_t>(value));
}

struct Samples {
  Clock::time_point report_start = Clock::now();
  uint64_t timing_calls = 0;
  uint64_t presents = 0;
  uint64_t forced_calls = 0;
  uint64_t fractional_calls = 0;
  uint64_t interval_twos = 0;
  uint64_t polls = 0;
  uint64_t yields = 0;
  uint64_t sleeps = 0;
  double scale_sum = 0;
  float min_scale = 1000;
  float max_scale = 0;
  uint32_t counter_start = 0;
  bool counter_valid = false;
  uint32_t flags0 = 0;
  uint32_t flags4 = 0;
};

// Timing producer, present, and guest fence helper run on the render thread
// on the traced path. Per-thread samples also avoid racing any indirect users.
thread_local Samples samples;

struct FrameClock {
  uint64_t configuration = UINT64_MAX;
  uint64_t previous_tick = 0;
  uint64_t origin = 0;
  Clock::time_point next{};
  bool valid = false;

  void Pace(MgrTimingMode mode) {
    static const bool pace = Option("MGR_TIMING_PACE", true);
    // Unlocked mode is limited only by the guest vblank and the GPU.
    if (!pace || mode != MgrTimingMode::kCap120) return;
    constexpr auto period = std::chrono::nanoseconds(8333333);
    auto now = Clock::now();
    if (!valid || now - next > period) next = now;
    if (valid && now < next) std::this_thread::sleep_until(next);
    // Advance the deadline rather than adding a period to the wake-up time:
    // scheduler oversleep must not accumulate into a lower target frequency.
    next += period;
    now = Clock::now();
    if (next <= now) next = now + period;
  }
};
thread_local FrameClock frame_clock;
}  // namespace

extern "C" REX_FUNC(sub_828A35B0) {
  const uint64_t configuration = Configuration().load(std::memory_order_relaxed);
  const auto mode = MgrTimingMode(configuration & 0xFF);
  if (frame_clock.configuration != configuration) {
    frame_clock = FrameClock{};
    frame_clock.configuration = configuration;
    samples = Samples{};
  }
  if (mode == MgrTimingMode::kOriginal) {
    __imp__sub_828A35B0(ctx, base);
    return;
  }
  const bool compensate = ctx.r3.u32 != 0;
  static const bool force_elapsed = Option("MGR_FRAME_SCALE_FORCE_ELAPSED", false);
  frame_clock.Pace(mode);
  const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
  const uint64_t origin = U64(base, kStartTicks);
  const bool have_delta = frame_clock.valid && frame_clock.origin == origin &&
                          F32(base, kState + 128) > 0 &&
                          now > frame_clock.previous_tick;
  const double delta_seconds = have_delta
      ? double(now - frame_clock.previous_tick) /
            double(rex::chrono::Clock::guest_tick_frequency())
      : 0;
  frame_clock.previous_tick = now;
  frame_clock.origin = origin;
  frame_clock.valid = true;

  // Preserve original elapsed/accumulator fields, guest register effects,
  // forced-mode behavior, and the upper catch-up bound. Only replace the
  // shared result when a valid fractional sample is available.
  __imp__sub_828A35B0(ctx, base);
  const double units_per_second = double(F32(base, kState + 136)) * 1000.0;
  if ((compensate || force_elapsed) && have_delta &&
      std::isfinite(delta_seconds) && std::isfinite(units_per_second) &&
      units_per_second > 0) {
    StoreF32(base, kState + 124,
             float(std::clamp(delta_seconds * units_per_second, 0.0, 2.0)));
  }

  auto& s = samples;
  const float scale = F32(base, kState + 124);
  ++s.timing_calls;
  s.forced_calls += !compensate;
  s.fractional_calls += scale < 1;
  s.scale_sum += scale;
  s.min_scale = std::min(s.min_scale, scale);
  s.max_scale = std::max(s.max_scale, scale);
  s.flags0 = U32(base, kFlags);
  s.flags4 = U32(base, kFlags + 4);
}

extern "C" REX_FUNC(sub_826B3050) {
  const uint64_t configuration = Configuration().load(std::memory_order_relaxed);
  const bool enabled = MgrTimingMode(configuration & 0xFF) != MgrTimingMode::kOriginal;
  const uint32_t device = ctx.r3.u32;
  const uint32_t interval = U32(base, device + kDeviceSwapInterval);
  bool restore_interval = false;
  if (enabled) {
    static const bool interval_one = Option("MGR_FRAME_INTERVAL_ONE", true);
    // Original present helper packs this setting into the retrace interval.
    // Reduce the two-retrace path to one at 120-Hz guest refresh. Retain all
    // packets, callbacks, flip accounting, and fence waits in the original.
    if (interval_one && interval == 2) {
      StoreU32(base, device + kDeviceSwapInterval, 1);
      restore_interval = true;
      ++samples.interval_twos;
    }
  }
  // Track the device too: a recreated device needs its own override. Preserve
  // its original refresh and restore it only if the value is still ours.
  struct RefreshOverride {
    uint32_t device = 0;
    uint32_t original = 0;
    uint32_t written = 0;
    bool active = false;
  };
  thread_local RefreshOverride refresh;
  const uint32_t current_hz = U32(base, device + kDeviceRefreshHz);
  if (enabled) {
    if (!refresh.active || refresh.device != device || current_hz != refresh.written) {
      refresh = {device, current_hz, 0, true};
    }
    refresh.written = uint32_t((configuration >> 8) & 0xFFFF);
    StoreU32(base, device + kDeviceRefreshHz, refresh.written);
  } else if (refresh.active) {
    if (refresh.device == device && current_hz == refresh.written) {
      StoreU32(base, device + kDeviceRefreshHz, refresh.original);
    }
    refresh = {};
  }
  __imp__sub_826B3050(ctx, base);
  // The interval change is scoped to this present, so switching back to 60
  // cannot leave a modified guest setting behind. Retain any guest-side write.
  if (restore_interval && U32(base, device + kDeviceSwapInterval) == 1) {
    StoreU32(base, device + kDeviceSwapInterval, interval);
  }
}

extern "C" REX_FUNC(sub_826B1C88) {
  // Poll backoff is useful at the original 60 Hz too; it changes neither the
  // wait predicate nor any game clock, and is independently disableable.
  const uint32_t wait_state = ctx.r3.u32;
  __imp__sub_826B1C88(ctx, base);
  static const bool backoff = Option("MGR_GPU_WAIT_BACKOFF", true);
  if (!backoff) return;
  ++samples.polls;
  struct PollState {
    uint32_t address = 0;
    uint32_t progress = 0;
    uint32_t repeats = 0;
  };
  thread_local PollState poll;
  const uint32_t progress = U32(base, wait_state + 8);
  if (!ctx.r3.u32 || poll.address != wait_state || poll.progress != progress) {
    poll = {wait_state, progress, 0};
    return;
  }
  if (++poll.repeats % 32) return;
#if defined(__APPLE__) && defined(__aarch64__)
  // Block instead of repeatedly yielding to the scheduler on Apple silicon.
  // This is a minimum sleep request, not a precise 50-us wake-up guarantee.
  constexpr bool block = true;
#else
  const bool block = poll.repeats >= 256;
#endif
  if (block) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    ++samples.sleeps;
  } else {
    std::this_thread::yield();
    ++samples.yields;
  }
}

void MgrTimingOnPresent(const uint8_t* base) {
  if (!Enabled()) return;
  auto& s = samples;
  const auto now = Clock::now();
  const uint32_t counter = U32(base, kFrameCounter);
  if (!s.counter_valid) {
    s.counter_start = counter;
    s.counter_valid = true;
  }
  ++s.presents;
  const double seconds = std::chrono::duration<double>(now - s.report_start).count();
  if (seconds < 1) return;
  std::fprintf(stderr,
      "[mgr-timing] seconds=%.3f presents=%llu calls=%llu frames=%u "
      "scale=%.5f..%.5f mean=%.5f nominal_sim_s=%.5f "
      "forced=%llu fractional=%llu flags=%08x/%08x interval2=%llu "
      "polls=%llu yields=%llu sleeps=%llu\n",
      seconds, static_cast<unsigned long long>(s.presents),
      static_cast<unsigned long long>(s.timing_calls), counter - s.counter_start,
      s.timing_calls ? s.min_scale : 0, s.max_scale,
      s.timing_calls ? s.scale_sum / double(s.timing_calls) : 0,
      s.scale_sum / 60, static_cast<unsigned long long>(s.forced_calls),
      static_cast<unsigned long long>(s.fractional_calls), s.flags0, s.flags4,
      static_cast<unsigned long long>(s.interval_twos),
      static_cast<unsigned long long>(s.polls),
      static_cast<unsigned long long>(s.yields),
      static_cast<unsigned long long>(s.sleeps));
  s = Samples{};
  s.report_start = now;
  s.counter_start = counter;
  s.counter_valid = true;
}

void MgrTimingSetMode(MgrTimingMode new_mode, uint32_t refresh_hz) {
  const uint32_t hz = new_mode == MgrTimingMode::kOriginal ? 0 : std::clamp(refresh_hz, 1u, 240u);
  const uint64_t settings = (uint64_t(hz) << 8) | uint64_t(new_mode);
  auto& configuration = Configuration();
  uint64_t previous = configuration.load(std::memory_order_relaxed);
  while (!configuration.compare_exchange_weak(
      previous, ((previous + (uint64_t(1) << 32)) & 0xFFFFFFFF00000000ull) | settings,
      std::memory_order_relaxed)) {}
}
