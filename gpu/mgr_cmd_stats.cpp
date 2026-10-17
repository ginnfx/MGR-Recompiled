// MGR port diagnostic: per-call-site Vulkan command counts (see mgr_cmd_stats.h).

#include <rex/graphics/vulkan/mgr_cmd_stats.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics::vulkan::mgr_stats {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto REPORT_INTERVAL = std::chrono::seconds(5);
constexpr size_t TOP_SITES = 12;
constexpr const char* KIND_NAMES[] = {"BeginRenderPass", "BeginRendering", "Dispatch",
                                      "CopyBuffer",      "CopyBufferToImage", "Barrier",
                                      "Draw",            "ClearAttachments",  "Submission"};

struct State {
  // Recorded only from the GPU command processor thread.
  std::map<std::pair<int, const void*>, unsigned long> sites;
  unsigned long totals[static_cast<int>(Kind::kCount)] = {};
  unsigned long frames = 0;
  long long busy_ns = 0;
  long long wait_ns = 0;
  Clock::time_point window_start = Clock::now();
};

State& GetState() {
  static State state;
  return state;
}

std::string Symbolize(const void* addr) {
  Dl_info info{};
  if (!dladdr(addr, &info)) return "?";
  if (!info.dli_sname) {  // hidden symbol: resolve offline with atos -o <dylib> <offset>
    const char* file = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s+0x%lx", file ? file + 1 : "?",
                  static_cast<unsigned long>(static_cast<const char*>(addr) -
                                             static_cast<const char*>(info.dli_fbase)));
    return buf;
  }
  std::string name = info.dli_sname;
  // Keep mangled names short; they are still unique enough to grep.
  if (name.size() > 110) name.resize(110);
  return name;
}

void Report(State& s) {
  if (!s.frames) return;
  const double n = static_cast<double>(s.frames);
  const double wall_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - s.window_start).count();
  std::fprintf(stderr,
               "[mgr-gpu] CP work %.2f ms/frame + GPU fence wait %.2f ms/frame, frame %.2f ms\n",
               (s.busy_ns - s.wait_ns) / 1e6 / n, s.wait_ns / 1e6 / n, wall_ms / n);
  std::fprintf(stderr, "[mgr-gpu] per frame over %lu frames:", s.frames);
  for (int k = 0; k < static_cast<int>(Kind::kCount); ++k)
    std::fprintf(stderr, " %s=%.1f", KIND_NAMES[k], s.totals[k] / n);
  std::fprintf(stderr, "\n");
  std::vector<std::pair<unsigned long, std::pair<int, const void*>>> top;
  for (auto& [key, count] : s.sites) {
    if (key.first == static_cast<int>(Kind::kDraw)) continue;
    top.emplace_back(count, key);
  }
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < std::min(top.size(), TOP_SITES); ++i) {
    std::fprintf(stderr, "[mgr-gpu]   %7.1f %-17s %s\n", top[i].first / n,
                 KIND_NAMES[top[i].second.first], Symbolize(top[i].second.second).c_str());
  }
}

}  // namespace

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("MGR_GPU_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}

long long NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch()).count();
}

void AddBusyNanoseconds(long long ns) { GetState().busy_ns += ns; }
void AddWaitNanoseconds(long long ns) { GetState().wait_ns += ns; }

void Record(Kind kind, const void* call_site) {
  State& s = GetState();
  ++s.totals[static_cast<int>(kind)];
  ++s.sites[{static_cast<int>(kind), call_site}];
}

void OnFrame() {
  if (!Enabled()) return;
  State& s = GetState();
  ++s.frames;
  if (Clock::now() - s.window_start < REPORT_INTERVAL) return;
  Report(s);
  s = State{};
}

}  // namespace rex::graphics::vulkan::mgr_stats
