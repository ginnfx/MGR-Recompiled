// Frame pacing meter.
//
// The SDK's perf counters (kFps / kFrameTimeUs) are never fed in this build,
// so count guest presents directly: this definition of __imp__VdSwap in the
// executable takes precedence for calls from recompiled code, and forwards to
// the SDK implementation found with dlsym(RTLD_NEXT).
//
// Prints once per second: "[mgr-fps] <fps> worst <ms> ms"; set MGR_FPS=0 to mute.
// Also feeds mgrr_perf session logs (~/Desktop/MGRR-Recomp-Perf) when MGR_PERF_LOG=1.

#include <rex/ppc/func.h>

#include "perf_logger.h"
#include "mgr_input_prompts.h"
#include "mgr_timing.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto REPORT_INTERVAL = std::chrono::seconds(1);

PPCFunc* ResolveSdkVdSwap() {
  auto* fn = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, "__imp__VdSwap"));
  if (!fn) {
    std::fprintf(stderr, "[mgr-fps] FATAL: SDK __imp__VdSwap not found: %s\n", dlerror());
    std::abort();
  }
  return fn;
}

bool IsReportingEnabled() {
  const char* v = std::getenv("MGR_FPS");
  return !v || v[0] != '0';
}

struct FrameMeter {
  Clock::time_point window_start = Clock::now();
  Clock::time_point last_frame = window_start;
  Clock::duration worst{};
  int frames = 0;

  void OnFrame() {
    auto now = Clock::now();
    auto dt = now - last_frame;
    last_frame = now;
    if (dt > worst) worst = dt;
    ++frames;
    if (now - window_start < REPORT_INTERVAL) return;
    double secs = std::chrono::duration<double>(now - window_start).count();
    double worst_ms = std::chrono::duration<double, std::milli>(worst).count();
    std::fprintf(stderr, "[mgr-fps] %.1f worst %.1f ms\n", frames / secs, worst_ms);
    window_start = now;
    worst = {};
    frames = 0;
  }
};

}  // namespace

extern "C" REX_FUNC(__imp__VdSwap) {
  static PPCFunc* const sdk_vdswap = ResolveSdkVdSwap();
  static const bool reporting = IsReportingEnabled();
  static FrameMeter meter;  // VdSwap is only called from the guest render thread
  if (reporting) meter.OnFrame();
  MgrTimingOnPresent(base);
  MgrPromptsOnPresent();
  mgrr_perf::Profiler::Get().EndFrame();  // no-op unless MgrStartSessionLog ran
  sdk_vdswap(ctx, base);
}

void MgrStartSessionLog(const char* build) {
  mgrr_perf::Config cfg;
  cfg.build = build;
  if (!mgrr_perf::Profiler::Get().Start(cfg)) {
    std::fprintf(stderr, "[mgr-perf] session logging disabled: cannot create directory\n");
    return;
  }
  std::fprintf(stderr, "[mgr-perf] session: %s\n",
               mgrr_perf::Profiler::Get().SessionDirectory().c_str());
}

void MgrStopSessionLog() { mgrr_perf::Profiler::Get().Stop(); }
