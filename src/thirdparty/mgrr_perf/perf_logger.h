#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mgrr_perf {

// CPU WALL-CLOCK DURATION for each instrumented section, not GPU execution time.
// Values on concurrently running threads may overlap and should not be summed.
enum class Metric : std::size_t {
  GuestCpu, XeniaGpuCommands, ShaderTranslation, PipelineCreation,
  QueueSubmit, QueuePresent, AcquireImage, FenceWait, OtherWait,
  ResourceUpload, GpuReadback, RenderTargetResolve, Count
};
constexpr std::size_t kMetricCount = static_cast<std::size_t>(Metric::Count);

struct Config {
  std::string game = "Metal Gear Rising: Revengeance (Xbox 360 recomp)";
  std::string build = "dev";
  std::string gpu = "Apple M2";
  std::string graphics_backend = "Xenia Vulkan -> MoltenVK -> Metal";
  std::filesystem::path directory_override; // Empty: ~/Desktop/MGRR-Recomp-Perf
  double stall_event_threshold_ms = 250.0; // Use 50 after reaching ~60 FPS
};

struct FrameRecord {
  std::uint64_t frame_id = 0, elapsed_us = 0, frame_interval_us = 0;
  std::array<std::uint64_t, kMetricCount> scope_us{};
  std::uint32_t draw_calls = 0, dispatches = 0, pipelines_created = 0;
  std::uint32_t shaders_translated = 0, rt_resolves = 0;
  std::uint32_t pipeline_cache_hits = 0, pipeline_cache_misses = 0;
  std::uint64_t upload_bytes = 0, readback_bytes = 0;
};

struct HudSnapshot {
  double fps = 0, rolling_fps = 0, fps_1pct_low = 0;
  double frame_ms = 0, p99_frame_ms = 0, latest_gpu_ms = -1;
  std::uint64_t latest_gpu_frame = 0, frame_id = 0;
  std::array<double, kMetricCount> scope_ms{};
  std::uint32_t draw_calls = 0, dispatches = 0, pipelines_created = 0;
  std::uint32_t shaders_translated = 0, rt_resolves = 0;
  std::uint32_t pipeline_cache_hits = 0, pipeline_cache_misses = 0;
  std::uint64_t upload_bytes = 0, readback_bytes = 0;
  std::uint64_t dropped_frames = 0, dropped_events = 0;
  std::array<float, 120> frame_history_ms{};
  std::size_t history_size = 0;
  double elapsed_seconds = 0;
  double process_rss_mib = -1, process_cpu_percent_one_core = -1;
};

class Profiler {
public:
  static Profiler& Get();
  bool Start(Config config = {});
  void Stop();
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }
  const std::filesystem::path& SessionDirectory() const { return session_dir_; }

  // Call once at the end of each submitted/presented game frame, on ONE thread.
  // Returns a monotonically increasing frame ID for delayed GPU timestamp results.
  std::uint64_t EndFrame();
  void AddDuration(Metric metric, std::uint64_t microseconds);
  void AddDrawCalls(std::uint32_t count = 1);
  void AddDispatches(std::uint32_t count = 1);
  void AddPipelinesCreated(std::uint32_t count = 1);
  void AddShadersTranslated(std::uint32_t count = 1);
  void AddRtResolves(std::uint32_t count = 1);
  void AddPipelineCacheHits(std::uint32_t count = 1);
  void AddPipelineCacheMisses(std::uint32_t count = 1);
  void AddUploadBytes(std::uint64_t bytes);
  void AddReadbackBytes(std::uint64_t bytes);

  // Call only when Vulkan timestamp query results are AVAILABLE (possibly
  // multiple frames later). gpu_ms is the duration for the specified frame.
  void ReportGpuTime(std::uint64_t frame_id, double gpu_ms);
  void Event(std::string category, double duration_ms, std::string details = {});
  HudSnapshot Snapshot(); // Safe to call from an ImGui/render thread.

private:
  Profiler() = default;
  ~Profiler();
  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;

  struct Pending {
    enum class Type { Event, GpuTime } type = Type::Event;
    std::uint64_t elapsed_us = 0, frame_id = 0;
    double duration_ms = 0;
    std::string category, details;
  };

  static constexpr std::size_t kQueueCapacity = 8192;
  std::array<FrameRecord, kQueueCapacity> ring_{};
  std::atomic<std::uint64_t> ring_write_{0}, ring_read_{0};
  std::mutex pending_mutex_;
  std::vector<Pending> pending_;
  std::thread writer_;
  std::atomic<bool> running_{false}, stop_writer_{false};
  Config config_;
  std::filesystem::path session_dir_;
  std::chrono::steady_clock::time_point start_mono_{}, last_frame_{};
  std::chrono::system_clock::time_point start_wall_{};
  std::array<std::atomic<std::uint64_t>, kMetricCount> durations_{};
  std::atomic<std::uint32_t> draw_calls_{0}, dispatches_{0};
  std::atomic<std::uint32_t> pipelines_created_{0}, shaders_translated_{0}, rt_resolves_{0};
  std::atomic<std::uint32_t> pipeline_cache_hits_{0}, pipeline_cache_misses_{0};
  std::atomic<std::uint64_t> upload_bytes_{0}, readback_bytes_{0};
  std::atomic<std::int64_t> process_rss_kib_{-1}, process_cpu_millicore_percent_{-1};
  std::atomic<std::uint64_t> dropped_frames_{0}, dropped_events_{0};
  std::atomic<std::uint64_t> total_frames_{0}, sum_frame_us_{0};
  std::atomic<std::uint64_t> max_frame_us_{0}, large_frame_count_{0};
  std::atomic<std::int64_t> latest_gpu_us_{-1};
  std::atomic<std::uint64_t> latest_gpu_frame_{0};
  std::uint64_t next_frame_id_ = 0;
  std::mutex hud_mutex_;
  FrameRecord latest_frame_{};
  std::array<float, 600> history_{};
  std::size_t history_count_ = 0, history_cursor_ = 0;
  std::chrono::steady_clock::time_point snapshot_at_{};
  HudSnapshot cached_snapshot_{};

  void WriterLoop();
  std::uint64_t ElapsedUs() const;
};

class ScopedTimer {
public:
  explicit ScopedTimer(Metric metric) : metric_(metric), active_(Profiler::Get().IsRunning()),
      start_(active_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~ScopedTimer() {
    if (!active_) return;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_).count();
    Profiler::Get().AddDuration(metric_, static_cast<std::uint64_t>(us));
  }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
  Metric metric_;
  bool active_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace mgrr_perf

#define MGRR_PERF_CONCAT_INNER(a, b) a##b
#define MGRR_PERF_CONCAT(a, b) MGRR_PERF_CONCAT_INNER(a, b)
#define MGRR_PERF_SCOPE(metric) \
  ::mgrr_perf::ScopedTimer MGRR_PERF_CONCAT(_mgrr_perf_scope_, __COUNTER__)(metric)
