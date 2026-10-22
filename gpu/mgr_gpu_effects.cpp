// MGR port diagnostic: MGR_GPU_TIMING=2 attribution of GPU zones and
// command-processor CPU time to guest shaders ("effects"), see mgr_gpu_timing.h.

#include <rex/graphics/vulkan/mgr_gpu_timing.h>

#include <rex/graphics/vulkan/mgr_cmd_stats.h>

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::graphics::vulkan::mgr_timing {
namespace {

constexpr size_t kReportEffects = 32;

// Work categories of a zone, by kind and recording site.
enum Category : size_t { kPass, kResolve, kTextureLoad, kRtTransfer, kOtherGpu, kCategoryCount };
constexpr const char* kCategoryNames[kCategoryCount] = {"pass", "resolve", "texload", "rt-xfer",
                                                        "other"};

Category CategoryOf(int kind, const void* site) {
  if (kind == int(ZoneKind::kRenderPass)) return kPass;
  if (kind != int(ZoneKind::kDispatch)) return kOtherGpu;
  static std::unordered_map<const void*, Category> cache;
  const auto cached = cache.find(site);
  if (cached != cache.end()) return cached->second;
  const std::string symbol = mgr_stats::Symbolize(site);
  Category category = kOtherGpu;
  if (symbol.find("LoadTexture") != std::string::npos) {
    category = kTextureLoad;
  } else if (symbol.find("Resolve") != std::string::npos) {
    category = kResolve;
  } else if (symbol.find("Transfer") != std::string::npos ||
             symbol.find("Dump") != std::string::npos) {
    category = kRtTransfer;
  }
  cache.emplace(site, category);
  return category;
}

// MGR_SHADER_NAMES: "<16 hex digits><TAB><name>" per line; '#' comments.
const std::unordered_map<uint64_t, std::string>& ShaderNames() {
  static const std::unordered_map<uint64_t, std::string> names = [] {
    std::unordered_map<uint64_t, std::string> loaded;
    const char* path = std::getenv("MGR_SHADER_NAMES");
    if (!path) return loaded;
    FILE* file = std::fopen(path, "r");
    if (!file) {
      std::fprintf(stderr, "[mgr-gpu-effect] cannot open MGR_SHADER_NAMES=%s\n", path);
      return loaded;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), file)) {
      if (line[0] == '#') continue;
      char* tab = std::strchr(line, '\t');
      if (!tab) continue;
      *tab = '\0';
      char* end = nullptr;
      const uint64_t hash = std::strtoull(line, &end, 16);
      if (end == line) continue;
      std::string name(tab + 1);
      while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
      loaded.emplace(hash, std::move(name));
    }
    std::fclose(file);
    std::fprintf(stderr, "[mgr-gpu-effect] %zu shader names from %s\n", loaded.size(), path);
    return loaded;
  }();
  return names;
}

std::string EffectName(uint64_t tag) {
  if (tag == 0) return "(untagged)";
  const auto& names = ShaderNames();
  const auto found = names.find(tag);
  if (found != names.end()) return found->second;
  char text[40];
  std::snprintf(text, sizeof(text), "%s%016" PRIX64, (tag & kVertexShaderTagBit) ? "vs:" : "ps:",
                tag & ~kVertexShaderTagBit);
  return text;
}

struct EffectRow {
  uint64_t tag = 0;
  double ns[kCategoryCount] = {};
  double count[kCategoryCount] = {};
  double draws = 0;
  double cpu_ns = 0, cpu_draws = 0, cpu_copies = 0;
  uint8_t msaa_mask = 0;
  double gpu_ns() const {
    double total = 0;
    for (double value : ns) total += value;
    return total;
  }
};

}  // namespace

void GpuTimer::NoteDraw() {
  if (!zone_open_) return;
  for (size_t i = 0; i < zone_tag_count_; ++i) {
    if (zone_tags_[i].tag == effect_tag_) {
      ++zone_tags_[i].draws;
      return;
    }
  }
  if (zone_tag_count_ < kZoneTagSlots) zone_tags_[zone_tag_count_++] = {effect_tag_, 1};
}

void GpuTimer::AddCpu(uint64_t tag, uint64_t ns, bool copy) {
  CpuTotals& totals = window_cpu_[tag];
  if (copy) {
    totals.copy_ns += double(ns);
    ++totals.copies;
  } else {
    totals.draw_ns += double(ns);
    ++totals.draws;
  }
}

void GpuTimer::ReportEffects(double frames) {
  if (frames <= 0) return;
  std::unordered_map<uint64_t, EffectRow> rows;
  for (const auto& [key, totals] : window_effects_) {
    const auto [tag, kind, site] = key;
    EffectRow& row = rows[tag];
    row.tag = tag;
    const Category category = CategoryOf(kind, site);
    row.ns[category] += totals.ns;
    row.count[category] += double(totals.count);
    row.draws += double(totals.draws);
  }
  for (const auto& [tag, totals] : window_cpu_) {
    EffectRow& row = rows[tag];
    row.tag = tag;
    row.cpu_ns += totals.draw_ns + totals.copy_ns;
    row.cpu_draws += double(totals.draws);
    row.cpu_copies += double(totals.copies);
    row.msaa_mask |= totals.msaa_mask;
  }
  std::vector<const EffectRow*> ranked;
  ranked.reserve(rows.size());
  for (const auto& [tag, row] : rows) ranked.push_back(&row);
  std::sort(ranked.begin(), ranked.end(), [](const EffectRow* x, const EffectRow* y) {
    return x->gpu_ns() + x->cpu_ns > y->gpu_ns() + y->cpu_ns;
  });
  std::fprintf(stderr,
               "[mgr-gpu-effect] per frame by guest shader: GPU ms total | pass ms/n | resolve "
               "ms/n | texload ms/n | rt-xfer ms/n | other ms/n | draws | CP ms (draws, "
               "resolves) | MSAA sample counts\n");
  for (size_t i = 0; i < std::min(ranked.size(), kReportEffects); ++i) {
    const EffectRow& row = *ranked[i];
    char cells[kCategoryCount][32];
    for (size_t c = 0; c < kCategoryCount; ++c) {
      std::snprintf(cells[c], sizeof(cells[c]), "%5.2f/%-5.1f", row.ns[c] / 1e6 / frames,
                    row.count[c] / frames);
    }
    char msaa[16] = "-";  // e.g. "1x2x": 1x and 2x draws
    for (uint32_t log2 = 0, n = 0; log2 < 3; ++log2) {
      if (row.msaa_mask & (1u << log2)) {
        n += std::snprintf(msaa + n, sizeof(msaa) - n, "%ux", 1u << log2);
      }
    }
    std::fprintf(stderr,
                 "[mgr-gpu-effect]   %6.2f | %s | %s | %s | %s | %s | %6.1f | %5.2f (%5.1f, %4.1f)"
                 " | %-6s  %s\n",
                 row.gpu_ns() / 1e6 / frames, cells[kPass], cells[kResolve], cells[kTextureLoad],
                 cells[kRtTransfer], cells[kOtherGpu], row.draws / frames,
                 row.cpu_ns / 1e6 / frames, row.cpu_draws / frames, row.cpu_copies / frames, msaa,
                 EffectName(row.tag).c_str());
  }
}

}  // namespace rex::graphics::vulkan::mgr_timing
