// Opt-in call census of the whole statically linked XDK D3D library (built
// only with -DMGR_D3D_CENSUS=ON). Every wrapped function still runs its guest
// body. With MGR_D3D_CENSUS=1 it counts calls per guest frame, the distinct
// call sites (LR) and the first non-zero first argument (r3), and writes one
// TSV per window of kReportEveryPresents presents (<out>.<frame>.tsv), so
// title, menu and gameplay phases can be told apart. Native renderer bring-up uses it to name
// the entry points by matching per-frame counts against what Xenos executes.

#include "mgr_d3d_census_list.inc"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#define MGR_CENSUS_EXTERN(address) REX_EXTERN(__imp__sub_##address);
MGR_D3D_CENSUS_LIST(MGR_CENSUS_EXTERN)
#undef MGR_CENSUS_EXTERN

namespace {

enum Entry : size_t {
#define MGR_CENSUS_ENTRY(address) kEntry##address,
  MGR_D3D_CENSUS_LIST(MGR_CENSUS_ENTRY)
#undef MGR_CENSUS_ENTRY
  kEntryCount
};
constexpr uint32_t kAddresses[kEntryCount] = {
#define MGR_CENSUS_ADDRESS(address) 0x##address,
    MGR_D3D_CENSUS_LIST(MGR_CENSUS_ADDRESS)
#undef MGR_CENSUS_ADDRESS
};
constexpr uint64_t kReportEveryPresents = 300;
constexpr size_t kCallSites = 6;

struct Stats {
  std::atomic<uint64_t> calls{0};
  std::array<std::atomic<uint32_t>, kCallSites> call_sites{};
  std::atomic<uint32_t> first_r3{0};
  std::atomic<uint64_t> r3_matches{0};  // calls whose r3 equals first_r3
};
std::array<Stats, kEntryCount> stats{};
std::atomic<uint64_t> presents{0};

bool CensusEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_D3D_CENSUS");
    return value && value[0] == '1';
  }();
  return enabled;
}

void RecordCallSite(Stats& s, uint32_t lr) {
  for (auto& site : s.call_sites) {
    uint32_t current = site.load(std::memory_order_relaxed);
    // A failed exchange reloads `current`, which may be this same call site.
    if (current == 0 && site.compare_exchange_strong(current, lr, std::memory_order_relaxed)) return;
    if (current == lr) return;
  }
}

void Record(Entry entry, const PPCContext& ctx) {
  Stats& s = stats[entry];
  s.calls.fetch_add(1, std::memory_order_relaxed);
  RecordCallSite(s, static_cast<uint32_t>(ctx.lr));
  const uint32_t r3 = ctx.r3.u32;
  uint32_t first = s.first_r3.load(std::memory_order_relaxed);
  if (first == 0 && r3 != 0) {
    s.first_r3.compare_exchange_strong(first, r3, std::memory_order_relaxed);
    first = s.first_r3.load(std::memory_order_relaxed);
  }
  if (r3 == first) s.r3_matches.fetch_add(1, std::memory_order_relaxed);
}

void WriteReport(uint64_t frames) {
  const char* path = std::getenv("MGR_D3D_CENSUS_OUT");
  const std::string out = std::string(path && *path ? path : "mgr_d3d_census") + "." +
                          std::to_string(frames) + ".tsv";
  FILE* file = std::fopen(out.c_str(), "w");
  if (!file) {
    std::fprintf(stderr, "[mgr-d3d-census] cannot write %s\n", out.c_str());
    return;
  }
  std::fprintf(file, "addr\tcalls\tframes\tper_frame\tfirst_r3\tr3_same_pct\tcall_sites\n");
  for (size_t i = 0; i < kEntryCount; ++i) {
    const uint64_t calls = stats[i].calls.exchange(0, std::memory_order_relaxed);
    if (!calls) continue;
    const uint64_t same = stats[i].r3_matches.exchange(0, std::memory_order_relaxed);
    std::fprintf(file, "%08X\t%llu\t%llu\t%.2f\t%08X\t%.0f\t", kAddresses[i],
                 static_cast<unsigned long long>(calls),
                 static_cast<unsigned long long>(kReportEveryPresents),
                 double(calls) / double(kReportEveryPresents),
                 stats[i].first_r3.load(std::memory_order_relaxed),
                 100.0 * double(same) / double(calls));
    for (const auto& site : stats[i].call_sites) {
      const uint32_t lr = site.load(std::memory_order_relaxed);
      if (lr) std::fprintf(file, "%08X ", lr);
    }
    std::fputc('\n', file);
  }
  std::fclose(file);
  std::fprintf(stderr, "[mgr-d3d-census] wrote %s after %llu presents\n", out.c_str(),
               static_cast<unsigned long long>(frames));
}

}  // namespace

// Called from the VdSwap wrapper on the guest render thread.
void MgrD3DCensusOnPresent() {
  if (!CensusEnabled()) return;
  const uint64_t frames = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frames % kReportEveryPresents == 0) WriteReport(frames);
}

#define MGR_CENSUS_WRAP(address)                                               \
  extern "C" REX_FUNC(sub_##address) {                                         \
    if (CensusEnabled()) Record(kEntry##address, ctx);                         \
    __imp__sub_##address(ctx, base);                                           \
  }
MGR_D3D_CENSUS_LIST(MGR_CENSUS_WRAP)
#undef MGR_CENSUS_WRAP
