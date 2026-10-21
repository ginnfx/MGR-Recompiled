// MGR port diagnostic: GPU execution time from timestamps (see mgr_gpu_timing.h).

#include <rex/graphics/vulkan/mgr_gpu_timing.h>

#include <rex/graphics/vulkan/mgr_cmd_stats.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

namespace rex::graphics::vulkan::mgr_timing {
namespace {

constexpr double kReportIntervalNs = 5e9;
constexpr size_t kReportZones = 24;
constexpr uint32_t kMaxRenderPassIndex = 255;
constexpr const char* kZoneNames[] = {"pass", "dispatch", "copy", "upload"};

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0;
  const size_t index = std::min(values.size() - 1, size_t(fraction * values.size()));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

}  // namespace

int Level() {
  static const int level = [] {
    const char* v = std::getenv("MGR_GPU_TIMING");
    return v ? std::clamp(std::atoi(v), 0, 2) : 0;
  }();
  return level;
}

bool GpuTimer::Initialize(const ui::vulkan::VulkanDevice* device) {
  if (!Level()) return true;
  const auto& ifn = device->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
  uint32_t family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count,
                                               families.data());
  const uint32_t family = device->queue_family_graphics_compute();
  const uint32_t valid_bits = family < family_count ? families[family].timestampValidBits : 0;
  if (!valid_bits || properties.limits.timestampPeriod <= 0) {
    std::fprintf(stderr, "[mgr-gpu-time] timestamps unsupported on this queue; disabled\n");
    return true;
  }
  device_ = device->device();
  auto load = [&](const char* name) { return ifn.vkGetDeviceProcAddr(device_, name); };
  auto create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(load("vkCreateQueryPool"));
  destroy_query_pool_ = reinterpret_cast<PFN_vkDestroyQueryPool>(load("vkDestroyQueryPool"));
  cmd_reset_query_pool_ = reinterpret_cast<PFN_vkCmdResetQueryPool>(load("vkCmdResetQueryPool"));
  cmd_write_timestamp_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(load("vkCmdWriteTimestamp"));
  get_query_pool_results_ =
      reinterpret_cast<PFN_vkGetQueryPoolResults>(load("vkGetQueryPoolResults"));
  if (!create_query_pool || !destroy_query_pool_ || !cmd_reset_query_pool_ ||
      !cmd_write_timestamp_ || !get_query_pool_results_) {
    std::fprintf(stderr, "[mgr-gpu-time] missing query entry points; disabled\n");
    device_ = VK_NULL_HANDLE;
    return true;
  }
  VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kSlotCount * 2;
  if (create_query_pool(device_, &info, nullptr, &query_pool_) != VK_SUCCESS) {
    std::fprintf(stderr, "[mgr-gpu-time] vkCreateQueryPool failed; disabled\n");
    device_ = VK_NULL_HANDLE;
    return true;
  }
  if (Level() >= 2) {
    info.queryCount = kZoneQueryCount;
    if (create_query_pool(device_, &info, nullptr, &zone_pool_) != VK_SUCCESS) {
      std::fprintf(stderr, "[mgr-gpu-time] zone query pool failed; submissions only\n");
      zone_pool_ = VK_NULL_HANDLE;
    }
  }
  ns_per_tick_ = properties.limits.timestampPeriod;
  tick_mask_ = valid_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << valid_bits) - 1;
  if (const char* path = std::getenv("MGR_GPU_TIMING_CSV")) {
    static bool csv_started = false;  // live scale changes re-initialize; keep earlier rows
    csv_ = std::fopen(path, csv_started ? "a" : "w");
    if (csv_ && csv_started) std::fprintf(csv_, "# context re-initialized\n");
    if (csv_ && !csv_started) std::fprintf(csv_, "frame,gpu_busy_ms,gpu_span_ms,gpu_gap_ms,unix_ms,variant\n");
    csv_started = csv_started || csv_;
  }
  std::fprintf(stderr,
               "[mgr-gpu-time] enabled: level %d, %.3f ns/tick, %u valid bits, "
               "graphics+compute %d\n",
               zones_enabled() ? 2 : 1, ns_per_tick_, valid_bits,
               int(properties.limits.timestampComputeAndGraphics));
  return true;
}

void GpuTimer::Shutdown() {
  if (query_pool_ != VK_NULL_HANDLE) destroy_query_pool_(device_, query_pool_, nullptr);
  if (zone_pool_ != VK_NULL_HANDLE) destroy_query_pool_(device_, zone_pool_, nullptr);
  if (csv_) std::fclose(csv_);
  *this = GpuTimer{};  // submission numbering restarts with the next context
}

uint64_t GpuTimer::TicksToNs(uint64_t ticks) const {
  return uint64_t(double(ticks & tick_mask_) * ns_per_tick_);
}

void GpuTimer::BeginCommandBuffer(VkCommandBuffer command_buffer, uint64_t submission) {
  recording_slot_ = UINT32_MAX;
  recording_submission_ = submission;
  // Zones of a command buffer that was never ended (failed submission).
  zone_queries_in_use_ -= uint32_t(recording_zones_.size() * 2);
  recording_zones_.clear();
  if (query_pool_ == VK_NULL_HANDLE) return;
  const uint32_t slot = uint32_t(submission % kSlotCount);
  if (slots_[slot].pending) {  // ring full of unread results: skip, never overwrite
    ++untimed_submissions_;
    return;
  }
  cmd_reset_query_pool_(command_buffer, query_pool_, slot * 2, 2);
  cmd_write_timestamp_(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool_, slot * 2);
  slots_[slot] = {submission, true, recording_variant_};
  recording_slot_ = slot;
}

void GpuTimer::EndCommandBuffer(VkCommandBuffer command_buffer) {
  if (!recording_zones_.empty()) {
    zone_submissions_.push_back({recording_submission_, std::move(recording_zones_)});
    recording_zones_ = {};
  }
  if (recording_slot_ == UINT32_MAX) return;
  cmd_write_timestamp_(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_,
                       recording_slot_ * 2 + 1);
  recording_slot_ = UINT32_MAX;
}

void GpuTimer::ZoneBegin(VkCommandBuffer command_buffer, ZoneKind kind, const void* site,
                         uint32_t a, uint32_t b) {
  zone_open_ = false;
  if (zone_pool_ == VK_NULL_HANDLE) return;
  if (zone_queries_in_use_ + 2 > kZoneQueryCount) {
    ++zones_skipped_;
    return;
  }
  const uint32_t query = zone_query_next_;
  zone_query_next_ = (zone_query_next_ + 2) % kZoneQueryCount;
  zone_queries_in_use_ += 2;
  cmd_reset_query_pool_(command_buffer, zone_pool_, query, 2);
  cmd_write_timestamp_(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, zone_pool_, query);
  recording_zones_.push_back({kind, site, a, b, 0, query});
  zone_open_ = true;
}

void GpuTimer::ZoneEnd(VkCommandBuffer command_buffer, uint32_t draws) {
  if (!zone_open_) return;
  zone_open_ = false;
  Zone& zone = recording_zones_.back();
  zone.draws = draws;
  cmd_write_timestamp_(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, zone_pool_,
                       zone.query + 1);
}

void GpuTimer::FrameClosed(uint64_t last_submission) {
  if (query_pool_ != VK_NULL_HANDLE) frame_ends_.push_back(last_submission);
}

bool GpuTimer::ReadZones(const ZoneSubmission& submission) {
  // Every result must be available before any is consumed, so a retry sees
  // the same submission again.
  std::vector<uint64_t> ticks(submission.zones.size() * 2);
  for (size_t i = 0; i < submission.zones.size(); ++i) {
    if (get_query_pool_results_(device_, zone_pool_, submission.zones[i].query, 2,
                                2 * sizeof(uint64_t), &ticks[i * 2], sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      return false;
    }
  }
  for (size_t i = 0; i < submission.zones.size(); ++i) {
    const Zone& zone = submission.zones[i];
    const uint64_t begin = TicksToNs(ticks[i * 2]);
    const uint64_t end = std::max(begin, TicksToNs(ticks[i * 2 + 1]));
    uint32_t pass_index = 0;
    if (zone.kind == ZoneKind::kRenderPass) {
      pass_index = std::min(frame_render_pass_index_++, kMaxRenderPassIndex);
    }
    // Dispatches and copies vary in size from frame to frame; key them by site only.
    const bool keyed_by_size = zone.kind == ZoneKind::kRenderPass;
    ZoneTotals& totals = window_zones_[{int(zone.kind), zone.site, pass_index,
                                        keyed_by_size ? zone.a : 0, keyed_by_size ? zone.b : 0}];
    totals.ns += double(end - begin);
    ++totals.count;
    totals.draws += zone.draws;
    frame_zone_ns_ += double(end - begin);
  }
  zone_queries_in_use_ -= uint32_t(submission.zones.size() * 2);
  return true;
}

void GpuTimer::SubmissionsCompleted(uint64_t completed) {
  if (query_pool_ == VK_NULL_HANDLE) return;
  // Submissions complete in order; stop at the first result that is not ready.
  for (; next_collect_ <= completed; ++next_collect_) {
    Slot& slot = slots_[next_collect_ % kSlotCount];
    const bool timed = slot.pending && slot.submission == next_collect_;
    uint64_t ticks[2] = {};
    if (timed && get_query_pool_results_(device_, query_pool_,
                                         uint32_t(next_collect_ % kSlotCount) * 2, 2,
                                         sizeof(ticks), ticks, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      return;  // VK_NOT_READY: retry on the next completion update
    }
    while (!zone_submissions_.empty() && zone_submissions_.front().submission < next_collect_) {
      zone_queries_in_use_ -= uint32_t(zone_submissions_.front().zones.size() * 2);
      zone_submissions_.pop_front();
    }
    const bool has_zones =
        !zone_submissions_.empty() && zone_submissions_.front().submission == next_collect_;
    while (!frame_ends_.empty() && frame_ends_.front() < next_collect_) FinishFrame();
    if (has_zones) {
      if (!ReadZones(zone_submissions_.front())) return;
      zone_submissions_.pop_front();
    }
    if (timed) {
      slot.pending = false;
      frame_variant_ = slot.variant;
      const uint64_t begin = TicksToNs(ticks[0]);
      AddSubmission(next_collect_, begin, std::max(begin, TicksToNs(ticks[1])));
    }
    if (!frame_ends_.empty() && frame_ends_.front() == next_collect_) FinishFrame();
  }
}

void GpuTimer::AddSubmission(uint64_t submission, uint64_t begin_ns, uint64_t end_ns) {
  if (!frame_has_work_) frame_first_begin_ns_ = begin_ns;
  frame_has_work_ = true;
  frame_busy_ns_ += double(end_ns - begin_ns);
  frame_last_end_ns_ = std::max(frame_last_end_ns_, end_ns);
}

void GpuTimer::FinishFrame() {
  frame_ends_.pop_front();
  frame_render_pass_index_ = 0;
  if (!frame_has_work_) return;
  const double gap_ns = previous_frame_end_ns_ && frame_first_begin_ns_ > previous_frame_end_ns_
                            ? double(frame_first_begin_ns_ - previous_frame_end_ns_)
                            : 0.0;
  const FrameSample sample = {frame_busy_ns_ / 1e6,
                              double(frame_last_end_ns_ - frame_first_begin_ns_) / 1e6,
                              gap_ns / 1e6, frame_variant_};
  if (csv_) {
    // unix_ms is when the results were read (after the frame's fence), for
    // lining up with the frame CSV and log timestamps.
    const double unix_ms = std::chrono::duration<double, std::milli>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(csv_, "%llu,%.4f,%.4f,%.4f,%.3f,%u\n",
                 static_cast<unsigned long long>(frames_total_), sample.busy_ms, sample.span_ms,
                 sample.gap_ms, unix_ms, sample.variant);
    if (frames_total_ % 60 == 0) std::fflush(csv_);
  }
  ++frames_total_;
  if (window_.empty()) window_first_ns_ = frame_first_begin_ns_;
  window_.push_back(sample);
  window_zone_ns_ += frame_zone_ns_;
  window_last_ns_ = frame_last_end_ns_;
  previous_frame_end_ns_ = frame_last_end_ns_;
  frame_busy_ns_ = 0;
  frame_zone_ns_ = 0;
  frame_last_end_ns_ = 0;
  frame_has_work_ = false;
  if (double(window_last_ns_ - window_first_ns_) >= kReportIntervalNs) Report();
}

void GpuTimer::Report() {
  std::vector<double> busy;
  busy.reserve(window_.size());
  double busy_total = 0;
  for (const FrameSample& sample : window_) {
    busy.push_back(sample.busy_ms);
    busy_total += sample.busy_ms;
  }
  const double frames = double(window_.size());
  const double wall_ms = double(window_last_ns_ - window_first_ns_) / 1e6;
  std::fprintf(stderr,
               "[mgr-gpu-time] %zu frames: GPU busy median %.2f / P95 %.2f / P99 %.2f ms, "
               "mean %.2f ms, GPU occupancy %.0f%%, untimed submissions %llu\n",
               window_.size(), Percentile(busy, 0.5), Percentile(busy, 0.95),
               Percentile(busy, 0.99), busy_total / frames,
               wall_ms > 0 ? 100.0 * busy_total / wall_ms : 0.0,
               static_cast<unsigned long long>(untimed_submissions_));
  for (uint32_t variant = 0; variant < 2; ++variant) {
    std::vector<double> variant_busy;
    for (const FrameSample& sample : window_) {
      if (sample.variant == variant) variant_busy.push_back(sample.busy_ms);
    }
    // Only when the window mixes variants (an A/B run).
    if (variant_busy.empty() || variant_busy.size() == window_.size()) continue;
    std::fprintf(stderr,
                 "[mgr-gpu-time]   variant %u: %zu frames, median %.2f / P95 %.2f / P99 %.2f ms\n",
                 variant, variant_busy.size(), Percentile(variant_busy, 0.5),
                 Percentile(variant_busy, 0.95), Percentile(variant_busy, 0.99));
  }
  if (zones_enabled() && !window_zones_.empty()) {
    double kind_ms[4] = {};
    std::vector<std::pair<double, const ZoneKey*>> ranked;
    for (const auto& [key, totals] : window_zones_) {
      kind_ms[std::get<0>(key)] += totals.ns / 1e6 / frames;
      ranked.emplace_back(totals.ns, &key);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    if (window_zone_ns_ == 0) {
      std::fprintf(stderr, "[mgr-gpu-pass] all zones read 0 ns: timestamps look emulated "
                           "(check for an MTLCounterSampleBuffer error)\n");
    }
    std::fprintf(stderr,
                 "[mgr-gpu-pass] per frame: passes %.2f ms, dispatches %.2f ms, copies %.2f ms, "
                 "uploads %.2f ms, outside zones %.2f ms, zones skipped %llu\n",
                 kind_ms[0], kind_ms[1], kind_ms[2], kind_ms[3],
                 busy_total / frames - window_zone_ns_ / 1e6 / frames,
                 static_cast<unsigned long long>(zones_skipped_));
    for (size_t i = 0; i < std::min(ranked.size(), kReportZones); ++i) {
      const ZoneKey& key = *ranked[i].second;
      const ZoneTotals& totals = window_zones_.at(key);
      const auto [kind, site, pass_index, a, b] = key;
      char label[64];
      if (kind == int(ZoneKind::kRenderPass)) {
        std::snprintf(label, sizeof(label), "pass #%u %ux%u", pass_index, a, b);
      } else {
        std::snprintf(label, sizeof(label), "%s", kZoneNames[kind]);
      }
      std::fprintf(stderr, "[mgr-gpu-pass]   %6.2f ms  x%5.1f  draws %6.1f  %-20s %s\n",
                   totals.ns / 1e6 / frames, double(totals.count) / frames,
                   double(totals.draws) / frames, label,
                   mgr_stats::Symbolize(site).c_str());
    }
  }
  window_.clear();
  window_zones_.clear();
  window_zone_ns_ = 0;
  untimed_submissions_ = 0;
  zones_skipped_ = 0;
}

}  // namespace rex::graphics::vulkan::mgr_timing
