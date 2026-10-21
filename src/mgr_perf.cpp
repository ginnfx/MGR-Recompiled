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
#include <ctime>
#include <mach/mach.h>

namespace {
std::atomic<float> guest_fps{0};

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

// Opt-in arrival intervals and CPU consumed by the guest render thread. CPU
// duration excludes scheduled waits but does not include other guest threads.
// This is deliberately distinct from GPU execution time.
struct FrameCapture {
  FILE* file = nullptr;
  Clock::time_point start = Clock::now(), previous = start;
  uint64_t frame = 0, previous_cpu_ns = 0;
  double rss_mib = 0;
  FrameCapture() {
    const char* path = std::getenv("MGR_FRAME_CSV");
    if (path) file = std::fopen(path, "w");
    if (file) std::fprintf(file, "frame,elapsed_ms,interval_ms,render_thread_cpu_ms,rss_mib,unix_ms,"
                                 "hooks_cpu_ms,vdswap_cpu_ms\n");
  }
  ~FrameCapture() { if (file) std::fclose(file); }
  void OnFrame() {
    if (!file) return;
    const auto now = Clock::now();
    timespec cpu{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    const uint64_t cpu_ns = uint64_t(cpu.tv_sec)*1000000000 + cpu.tv_nsec;
    if (frame % 60 == 0) {
      mach_task_basic_info_data_t info{};
      mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
      if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        rss_mib = double(info.resident_size) / (1024*1024);
    }
    // unix_ms lines frames up with log timestamps and the GPU timing CSV.
    const double unix_ms = std::chrono::duration<double, std::milli>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // hooks/vdswap: CPU spent at the start of this interval (previous present)
    // in the MGR present hooks and in the SDK VdSwap.
    if (frame) std::fprintf(file, "%llu,%.6f,%.6f,%.6f,%.2f,%.3f,%.4f,%.4f\n",
        static_cast<unsigned long long>(frame),
        std::chrono::duration<double,std::milli>(now-start).count(),
        std::chrono::duration<double,std::milli>(now-previous).count(),
        double(cpu_ns-previous_cpu_ns)/1e6, rss_mib, unix_ms,
        double(hooks_ns)/1e6, double(vdswap_ns)/1e6);
    if (++frame % 60 == 0) std::fflush(file);
    previous = now; previous_cpu_ns = cpu_ns;
    mark_cpu_ns = cpu_ns;
  }
  // Called after the present hooks and after the SDK VdSwap respectively.
  void HooksDone() {
    if (!file) return;
    const uint64_t cpu_ns = ThreadCpuNs();
    hooks_ns = cpu_ns - mark_cpu_ns;
    mark_cpu_ns = cpu_ns;
  }
  void VdSwapDone() {
    if (!file) return;
    vdswap_ns = ThreadCpuNs() - mark_cpu_ns;
  }
  static uint64_t ThreadCpuNs() {
    timespec cpu{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    return uint64_t(cpu.tv_sec) * 1000000000 + cpu.tv_nsec;
  }
  uint64_t mark_cpu_ns = 0, hooks_ns = 0, vdswap_ns = 0;
};

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
    guest_fps.store(float(frames / secs), std::memory_order_relaxed);
    if (IsReportingEnabled())
      std::fprintf(stderr, "[mgr-fps] %.1f worst %.1f ms\n", frames / secs, worst_ms);
    window_start = now;
    worst = {};
    frames = 0;
  }
};

}  // namespace

void MgrD3DTraceOnPresent();  // mgr_d3d_trace.cpp

float MgrGuestFPS() { return guest_fps.load(std::memory_order_relaxed); }

extern "C" REX_FUNC(__imp__VdSwap) {
  static PPCFunc* const sdk_vdswap = ResolveSdkVdSwap();
  static FrameCapture capture;
  capture.OnFrame();
  static FrameMeter meter;  // VdSwap is only called from the guest render thread
  meter.OnFrame();
  MgrD3DTraceOnPresent();
  MgrTimingOnPresent(base);
  MgrPromptsOnPresent();
  mgrr_perf::Profiler::Get().EndFrame();  // no-op unless MgrStartSessionLog ran
  capture.HooksDone();
  sdk_vdswap(ctx, base);
  capture.VdSwapDone();
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
