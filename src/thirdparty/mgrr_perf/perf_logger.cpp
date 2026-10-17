#include "perf_logger.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <psapi.h>
#endif
#include <random>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace mgrr_perf {
namespace {
using Steady = std::chrono::steady_clock;
using Wall = std::chrono::system_clock;

std::string IsoUtc(Wall::time_point t) {
  const std::time_t tt = Wall::to_time_t(t);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &tt);
#else
  gmtime_r(&tt, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string FileTimestamp(Wall::time_point t) {
  const std::time_t tt = Wall::to_time_t(t);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &tt);
#else
  gmtime_r(&tt, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y%m%d_%H%M%S");
  return out.str();
}

std::string JsonEscape(const std::string& s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(c) << std::dec << std::setfill(' ');
        } else out << c;
    }
  }
  return out.str();
}

std::string CsvEscape(const std::string& s) {
  std::string out = "\"";
  for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
  return out + '"';
}

std::filesystem::path Home() {
#ifdef _WIN32
  if (const char* p = std::getenv("USERPROFILE")) return std::filesystem::u8path(p);
#else
  if (const char* p = std::getenv("HOME")) return p;
#endif
  return std::filesystem::current_path();
}

std::filesystem::path DefaultDesktop() {
#ifdef _WIN32
  PWSTR desktop = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
    std::filesystem::path p(desktop);
    CoTaskMemFree(desktop);
    return p;
  }
#endif
  // macOS and conventional Linux desktops. Override if your Linux desktop
  // uses a different location (e.g. XDG user-dirs configuration).
  return Home() / "Desktop";
}

std::string RandomSuffix() {
  std::random_device rd;
  const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) | rd();
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << v;
  return out.str();
}

void AtomicMax(std::atomic<std::uint64_t>& field, std::uint64_t value) {
  auto current = field.load(std::memory_order_relaxed);
  while (value > current &&
         !field.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

constexpr const char* kMetricHeaders[] = {
 "guest_cpu_ms", "xenia_gpu_commands_ms", "shader_translation_ms",
 "pipeline_creation_ms", "queue_submit_ms", "queue_present_ms",
 "acquire_image_ms", "fence_wait_ms", "other_wait_ms",
 "resource_upload_cpu_ms", "gpu_readback_cpu_ms", "rt_resolve_cpu_ms"
};

// Sample on the *logger thread*, never the renderer.
double ProcessCpuSeconds() {
#ifdef _WIN32
  FILETIME create, exit, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return -1;
  ULARGE_INTEGER k{}, u{};
  k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
  return static_cast<double>(k.QuadPart + u.QuadPart) / 1e7;
#else
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
  return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
         usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#endif
}

double ProcessRssMib() {
#ifdef __APPLE__
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return -1;
  return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return -1;
  return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  unsigned long long size_pages = 0, resident_pages = 0;
  if (!(statm >> size_pages >> resident_pages)) return -1;
  return static_cast<double>(resident_pages) * sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0);
#else
  return -1;
#endif
}
}

Profiler& Profiler::Get() {
  static Profiler profiler;
  return profiler;
}

Profiler::~Profiler() { Stop(); }

bool Profiler::Start(Config config) {
  if (running_.load(std::memory_order_acquire)) return false;
  config_ = std::move(config);
  const auto time = Wall::now();
  const std::string suffix = FileTimestamp(time) + "_" + RandomSuffix();
  std::error_code error;
  std::filesystem::path base = config_.directory_override.empty()
      ? DefaultDesktop() / "MGRR-Recomp-Perf" : config_.directory_override;
  session_dir_ = base / suffix;
  std::filesystem::create_directories(session_dir_, error);
  if (error) {
    error.clear();
    session_dir_ = Home() / "MGRR-Recomp-Perf" / suffix;
    std::filesystem::create_directories(session_dir_, error);
    if (error) return false;
  }

  // Ensure failure to open a log file is visible immediately.
  std::ofstream probe(session_dir_ / "frames.csv", std::ios::trunc);
  if (!probe) return false;
  probe.close();
  start_wall_ = time;
  start_mono_ = Steady::now();
  last_frame_ = {};
  next_frame_id_ = 0;
  ring_read_ = 0;
  ring_write_ = 0;
  total_frames_ = 0;
  sum_frame_us_ = 0;
  max_frame_us_ = 0;
  large_frame_count_ = 0;
  dropped_frames_ = 0;
  dropped_events_ = 0;
  latest_gpu_us_ = -1;
  latest_gpu_frame_ = 0;
  draw_calls_ = 0;
  dispatches_ = 0;
  pipelines_created_ = 0;
  shaders_translated_ = 0;
  rt_resolves_ = 0; pipeline_cache_hits_ = 0; pipeline_cache_misses_ = 0;
  upload_bytes_ = 0; readback_bytes_ = 0;
  process_rss_kib_ = -1; process_cpu_millicore_percent_ = -1;
  for (auto& v : durations_) v = 0;
  {
    std::lock_guard<std::mutex> lock(hud_mutex_);
    latest_frame_ = {};
    history_count_ = history_cursor_ = 0;
    snapshot_at_ = {};
    cached_snapshot_ = {};
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.clear();
  }
  {
    std::ofstream meta(session_dir_ / "session.json");
    meta << "{\n  \"status\": \"recording\",\n"
         << "  \"start_utc\": \"" << IsoUtc(start_wall_) << "\",\n"
         << "  \"game\": \"" << JsonEscape(config_.game) << "\",\n"
         << "  \"build\": \"" << JsonEscape(config_.build) << "\",\n"
         << "  \"gpu\": \"" << JsonEscape(config_.gpu) << "\",\n"
         << "  \"graphics_backend\": \""
         << JsonEscape(config_.graphics_backend) << "\"\n}\n";
    if (!meta) return false;
  }
  stop_writer_ = false;
  running_ = true;
  try { writer_ = std::thread([this] { WriterLoop(); }); }
  catch (...) { running_ = false; return false; }
  return true;
}

void Profiler::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stop_writer_ = true;
  if (writer_.joinable()) writer_.join();
}

std::uint64_t Profiler::ElapsedUs() const {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      Steady::now() - start_mono_).count());
}

void Profiler::AddDuration(Metric metric, std::uint64_t microseconds) {
  if (!IsRunning()) return;
  durations_[static_cast<std::size_t>(metric)].fetch_add(microseconds, std::memory_order_relaxed);
}
void Profiler::AddDrawCalls(std::uint32_t n) { if (IsRunning()) draw_calls_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddDispatches(std::uint32_t n) { if (IsRunning()) dispatches_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddPipelinesCreated(std::uint32_t n) { if (IsRunning()) pipelines_created_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddShadersTranslated(std::uint32_t n) { if (IsRunning()) shaders_translated_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddRtResolves(std::uint32_t n) { if (IsRunning()) rt_resolves_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddPipelineCacheHits(std::uint32_t n) { if (IsRunning()) pipeline_cache_hits_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddPipelineCacheMisses(std::uint32_t n) { if (IsRunning()) pipeline_cache_misses_.fetch_add(n, std::memory_order_relaxed); }
void Profiler::AddUploadBytes(std::uint64_t bytes) { if (IsRunning()) upload_bytes_.fetch_add(bytes, std::memory_order_relaxed); }
void Profiler::AddReadbackBytes(std::uint64_t bytes) { if (IsRunning()) readback_bytes_.fetch_add(bytes, std::memory_order_relaxed); }

std::uint64_t Profiler::EndFrame() {
  if (!IsRunning()) return 0;
  const auto now = Steady::now();
  if (last_frame_ == Steady::time_point{}) {
    last_frame_ = now;
    for (auto& v : durations_) v.exchange(0, std::memory_order_relaxed);
    draw_calls_.exchange(0); dispatches_.exchange(0);
    pipelines_created_.exchange(0); shaders_translated_.exchange(0);
    rt_resolves_.exchange(0); pipeline_cache_hits_.exchange(0); pipeline_cache_misses_.exchange(0);
    upload_bytes_.exchange(0); readback_bytes_.exchange(0);
    return 0; // No valid previous frame boundary yet.
  }
  FrameRecord f;
  f.frame_id = ++next_frame_id_;
  f.elapsed_us = ElapsedUs();
  f.frame_interval_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_).count());
  last_frame_ = now;
  for (std::size_t i = 0; i < kMetricCount; ++i)
    f.scope_us[i] = durations_[i].exchange(0, std::memory_order_relaxed);
  f.draw_calls = draw_calls_.exchange(0, std::memory_order_relaxed);
  f.dispatches = dispatches_.exchange(0, std::memory_order_relaxed);
  f.pipelines_created = pipelines_created_.exchange(0, std::memory_order_relaxed);
  f.shaders_translated = shaders_translated_.exchange(0, std::memory_order_relaxed);
  f.rt_resolves = rt_resolves_.exchange(0, std::memory_order_relaxed);
  f.pipeline_cache_hits = pipeline_cache_hits_.exchange(0, std::memory_order_relaxed);
  f.pipeline_cache_misses = pipeline_cache_misses_.exchange(0, std::memory_order_relaxed);
  f.upload_bytes = upload_bytes_.exchange(0, std::memory_order_relaxed);
  f.readback_bytes = readback_bytes_.exchange(0, std::memory_order_relaxed);

  total_frames_.fetch_add(1, std::memory_order_relaxed);
  sum_frame_us_.fetch_add(f.frame_interval_us, std::memory_order_relaxed);
  AtomicMax(max_frame_us_, f.frame_interval_us);
  if (f.frame_interval_us > 50000) large_frame_count_.fetch_add(1, std::memory_order_relaxed);

  const auto head = ring_write_.load(std::memory_order_relaxed);
  const auto tail = ring_read_.load(std::memory_order_acquire);
  if (head - tail < kQueueCapacity) {
    ring_[head % kQueueCapacity] = f;
    ring_write_.store(head + 1, std::memory_order_release);
  } else {
    dropped_frames_.fetch_add(1, std::memory_order_relaxed);
  }

  {
    std::lock_guard<std::mutex> lock(hud_mutex_);
    latest_frame_ = f;
    history_[history_cursor_] = static_cast<float>(f.frame_interval_us / 1000.0);
    history_cursor_ = (history_cursor_ + 1) % history_.size();
    if (history_count_ < history_.size()) ++history_count_;
  }
  if (f.frame_interval_us >= static_cast<std::uint64_t>(config_.stall_event_threshold_ms * 1000.0))
    Event("slow_frame", f.frame_interval_us / 1000.0, "frame_id=" + std::to_string(f.frame_id));
  return f.frame_id;
}

void Profiler::Event(std::string category, double duration_ms, std::string details) {
  if (!IsRunning()) return;
  std::lock_guard<std::mutex> lock(pending_mutex_);
  if (pending_.size() >= 4096) {
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pending_.push_back({Pending::Type::Event, ElapsedUs(), 0, duration_ms,
                      std::move(category), std::move(details)});
}

void Profiler::ReportGpuTime(std::uint64_t frame_id, double gpu_ms) {
  if (!IsRunning() || !frame_id || !std::isfinite(gpu_ms) || gpu_ms < 0) return;
  // Results may arrive out of order. Only show the most recent completed query.
  const auto old = latest_gpu_frame_.load(std::memory_order_relaxed);
  if (frame_id >= old) {
    latest_gpu_us_.store(static_cast<std::int64_t>(gpu_ms * 1000.0), std::memory_order_relaxed);
    latest_gpu_frame_.store(frame_id, std::memory_order_relaxed);
  }
  std::lock_guard<std::mutex> lock(pending_mutex_);
  if (pending_.size() >= 4096) {
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pending_.push_back({Pending::Type::GpuTime, ElapsedUs(), frame_id, gpu_ms, {}, {}});
}

HudSnapshot Profiler::Snapshot() {
  const auto now = Steady::now();
  std::lock_guard<std::mutex> lock(hud_mutex_);
  if (snapshot_at_ != Steady::time_point{} && now - snapshot_at_ < std::chrono::milliseconds(250))
    return cached_snapshot_;
  HudSnapshot s;
  const auto& f = latest_frame_;
  s.frame_id = f.frame_id;
  s.frame_ms = f.frame_interval_us / 1000.0;
  s.fps = s.frame_ms > 0 ? 1000.0 / s.frame_ms : 0;
  for (std::size_t i = 0; i < kMetricCount; ++i)
    s.scope_ms[i] = f.scope_us[i] / 1000.0;
  s.draw_calls = f.draw_calls;
  s.dispatches = f.dispatches;
  s.pipelines_created = f.pipelines_created;
  s.shaders_translated = f.shaders_translated;
  s.rt_resolves = f.rt_resolves;
  s.pipeline_cache_hits = f.pipeline_cache_hits;
  s.pipeline_cache_misses = f.pipeline_cache_misses;
  s.upload_bytes = f.upload_bytes;
  s.readback_bytes = f.readback_bytes;
  const auto rss = process_rss_kib_.load(std::memory_order_relaxed);
  const auto cpu = process_cpu_millicore_percent_.load(std::memory_order_relaxed);
  s.process_rss_mib = rss >= 0 ? rss / 1024.0 : -1;
  s.process_cpu_percent_one_core = cpu >= 0 ? cpu / 1000.0 : -1;
  s.latest_gpu_frame = latest_gpu_frame_.load(std::memory_order_relaxed);
  const auto last_gpu = latest_gpu_us_.load(std::memory_order_relaxed);
  s.latest_gpu_ms = last_gpu >= 0 ? last_gpu / 1000.0 : -1.0;
  s.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
  s.dropped_events = dropped_events_.load(std::memory_order_relaxed);
  if (IsRunning()) s.elapsed_seconds = ElapsedUs() / 1000000.0;

  if (history_count_) {
    std::vector<float> ordered;
    ordered.reserve(history_count_);
    const auto first = history_count_ == history_.size() ? history_cursor_ : 0;
    for (std::size_t i = 0; i < history_count_; ++i)
      ordered.push_back(history_[(first + i) % history_.size()]);
    const auto sum = std::accumulate(ordered.begin(), ordered.end(), 0.0);
    s.rolling_fps = sum > 0 ? ordered.size() * 1000.0 / sum : 0;
    std::sort(ordered.begin(), ordered.end());
    const std::size_t p99 = static_cast<std::size_t>(std::ceil(ordered.size() * 0.99)) - 1;
    s.p99_frame_ms = ordered[p99];
    // 1% low: 1000 / mean of the slowest ~1% frame times (600-frame window).
    const std::size_t worst = std::max<std::size_t>(1, (ordered.size() + 99) / 100);
    double sum_worst = 0;
    for (std::size_t i = ordered.size() - worst; i < ordered.size(); ++i) sum_worst += ordered[i];
    s.fps_1pct_low = sum_worst > 0 ? 1000.0 * worst / sum_worst : 0;
    s.history_size = std::min<std::size_t>(120, history_count_);
    const auto oldest = (history_cursor_ + history_.size() - s.history_size) % history_.size();
    for (std::size_t i = 0; i < s.history_size; ++i)
      s.frame_history_ms[i] = history_[(oldest + i) % history_.size()];
  }
  cached_snapshot_ = s;
  snapshot_at_ = now;
  return s;
}

void Profiler::WriterLoop() {
  std::ofstream frames(session_dir_ / "frames.csv", std::ios::app);
  std::ofstream events(session_dir_ / "events.csv", std::ios::trunc);
  std::ofstream gpu(session_dir_ / "gpu.csv", std::ios::trunc);
  std::ofstream process(session_dir_ / "process.csv", std::ios::trunc);
  frames << "frame_id,elapsed_ms,frame_interval_ms";
  for (const char* label : kMetricHeaders) frames << ',' << label;
  frames << ",draw_calls,dispatches,pipelines_created,shaders_translated,rt_resolves,"
            "pipeline_cache_hits,pipeline_cache_misses,upload_bytes,readback_bytes\n";
  events << "elapsed_ms,category,duration_ms,details\n";
  gpu << "reported_elapsed_ms,frame_id,gpu_time_ms\n";
  process << "elapsed_ms,process_rss_mib,process_cpu_percent_one_core\n";
  frames << std::fixed << std::setprecision(3);
  events << std::fixed << std::setprecision(3);
  gpu << std::fixed << std::setprecision(3);
  process << std::fixed << std::setprecision(3);

  auto last_flush = Steady::now(), last_state = Steady::now();
  auto last_process_sample = Steady::now();
  auto last_cpu_s = ProcessCpuSeconds();
  while (true) {
    bool did_work = false;
    std::uint64_t tail = ring_read_.load(std::memory_order_relaxed);
    const std::uint64_t head = ring_write_.load(std::memory_order_acquire);
    while (tail < head) {
      const auto& f = ring_[tail % kQueueCapacity];
      frames << f.frame_id << ',' << f.elapsed_us / 1000.0 << ','
             << f.frame_interval_us / 1000.0;
      for (auto value : f.scope_us) frames << ',' << value / 1000.0;
      frames << ',' << f.draw_calls << ',' << f.dispatches << ','
             << f.pipelines_created << ',' << f.shaders_translated << ','
             << f.rt_resolves << ',' << f.pipeline_cache_hits << ','
             << f.pipeline_cache_misses << ',' << f.upload_bytes << ','
             << f.readback_bytes << '\n';
      ++tail;
      did_work = true;
    }
    ring_read_.store(tail, std::memory_order_release);

    std::vector<Pending> items;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      items.swap(pending_);
    }
    for (const auto& item : items) {
      if (item.type == Pending::Type::GpuTime)
        gpu << item.elapsed_us / 1000.0 << ',' << item.frame_id << ',' << item.duration_ms << '\n';
      else
        events << item.elapsed_us / 1000.0 << ',' << CsvEscape(item.category)
               << ',' << item.duration_ms << ',' << CsvEscape(item.details) << '\n';
      did_work = true;
    }

    const auto now = Steady::now();
    if (now - last_process_sample >= std::chrono::seconds(1) || stop_writer_.load()) {
      const double elapsed = std::chrono::duration<double>(now - last_process_sample).count();
      const double cpu_s = ProcessCpuSeconds();
      const double rss_mib = ProcessRssMib();
      const double cpu_pct = elapsed > 0 && cpu_s >= 0 && last_cpu_s >= 0
          ? (cpu_s - last_cpu_s) / elapsed * 100.0 : -1.0;
      process << ElapsedUs() / 1000.0 << ',' << rss_mib << ',' << cpu_pct << '\n';
      if (rss_mib >= 0) process_rss_kib_.store(static_cast<std::int64_t>(rss_mib * 1024));
      if (cpu_pct >= 0) process_cpu_millicore_percent_.store(static_cast<std::int64_t>(cpu_pct * 1000));
      last_process_sample = now;
      last_cpu_s = cpu_s;
    }
    if (now - last_flush >= std::chrono::seconds(1) || stop_writer_.load()) {
      frames.flush(); events.flush(); gpu.flush(); process.flush();
      last_flush = now;
    }
    if (now - last_state >= std::chrono::seconds(5) || stop_writer_.load()) {
      std::ofstream state(session_dir_ / "state.json", std::ios::trunc);
      state << "{\n  \"status\": \"" << (stop_writer_.load() ? "completed" : "recording") << "\",\n"
            << "  \"updated_utc\": \"" << IsoUtc(Wall::now()) << "\",\n"
            << "  \"elapsed_seconds\": " << std::fixed << std::setprecision(3)
            << ElapsedUs() / 1000000.0 << ",\n"
            << "  \"frames\": " << total_frames_.load() << ",\n"
            << "  \"dropped_frame_rows\": " << dropped_frames_.load() << "\n}\n";
      last_state = now;
    }
    if (stop_writer_.load(std::memory_order_acquire) &&
        ring_read_.load(std::memory_order_relaxed) == ring_write_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      if (pending_.empty()) break;
    }
    if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  frames.flush(); events.flush(); gpu.flush(); process.flush();
  const auto end_wall = Wall::now();
  const auto total = total_frames_.load();
  const auto frame_time = sum_frame_us_.load();
  const double duration = ElapsedUs() / 1000000.0;
  std::ofstream summary(session_dir_ / "summary.json", std::ios::trunc);
  summary << "{\n  \"status\": \"completed\",\n"
          << "  \"start_utc\": \"" << IsoUtc(start_wall_) << "\",\n"
          << "  \"end_utc\": \"" << IsoUtc(end_wall) << "\",\n"
          << "  \"session_seconds\": " << std::fixed << std::setprecision(3) << duration << ",\n"
          << "  \"recorded_frames\": " << total << ",\n"
          << "  \"average_fps_from_frame_intervals\": " << (frame_time ? total * 1000000.0 / frame_time : 0) << ",\n"
          << "  \"max_frame_interval_ms\": " << max_frame_us_.load() / 1000.0 << ",\n"
          << "  \"frames_over_50ms\": " << large_frame_count_.load() << ",\n"
          << "  \"dropped_frame_rows\": " << dropped_frames_.load() << ",\n"
          << "  \"dropped_event_rows\": " << dropped_events_.load() << "\n}\n";
}

} // namespace mgrr_perf
