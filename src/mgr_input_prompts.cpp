// Device-matched button prompts (4B4E080A / 0.0.0.4): keyboard keycaps from the
// keybind cvars, PlayStation glyphs, or the original Xbox art.
// Every controller icon the game draws (HUD combat manual, menu footers, inline
// message glyphs) samples one texture: core.wtb texture 1. Once it is resident in
// guest physical memory, its icon cells are redrawn from the configured keybinds
// and the GPU's watched copy is invalidated, so no draw path needs patching.
#include "mgr_input_prompts.h"

#include "mgr_button_atlas.h"
#include "mgr_icon_render.h"
#include "mgr_key_labels.h"
#include "mgr_prompt_device.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>


namespace {
constexpr uint32_t kGuestPageSize = 4096;  // texture base addresses are 4 KB aligned
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kPollFrames = 30;
// Physical memory is searched incrementally: a full 512 MiB sweep in one poll
// cost 40-66 ms on the guest render thread (s2-sample frame CSV), which showed
// up as the worst frames. 32 MiB per poll sweeps everything in ~8 s at 60 FPS.
constexpr uint32_t kScanBytesPerPoll = 32 << 20;
// Physical memory views the GPU watches; E0000000 is offset by one page.
constexpr uint32_t kViewA0 = 0xA0000000, kViewC0 = 0xC0000000, kViewE0 = 0xE0000000;

struct AtlasCopy {
  uint32_t physical;
  std::vector<uint8_t> shown;  // bytes this patcher last wrote (or found)
};

class AtlasPatcher {
 public:
  void OnPresent(rex::memory::Memory& memory) {
    if (++frame_ % kPollFrames) return;
    const PromptFamily family = MgrPromptFamily();
    const bool original_art = family == PromptFamily::kXbox;
    Validate(memory);
    // Keep sweeping: the game may load another copy elsewhere.
    if (!original_art) ScanSome(memory, kScanBytesPerPoll);
    if (copies_.empty()) return;
    const std::vector<uint8_t>& target = original_art ? original_ : Patched(family);
    for (AtlasCopy& copy : copies_) {
      if (copy.shown != target) Write(memory, copy, target, !original_art);
    }
  }

 private:
  // Drops copies whose memory was reused; a reload of the original data
  // stays tracked and is patched again.
  void Validate(rex::memory::Memory& memory) {
    std::erase_if(copies_, [&](AtlasCopy& copy) {
      // Released pages keep their bytes, so check the allocation first.
      if (!IsReadableCommitted(memory, copy.physical)) {
        std::fprintf(stderr, "[mgr-prompts] atlas at 0x%08X released\n", copy.physical);
        return true;
      }
      const uint8_t* bytes = memory.TranslatePhysical(copy.physical);
      if (std::memcmp(bytes, copy.shown.data(), kAtlasBytes) == 0) return false;
      if (Fnv1a64({bytes, kAtlasBytes}) != kAtlasHash) {
        std::fprintf(stderr, "[mgr-prompts] atlas at 0x%08X released\n", copy.physical);
        return true;
      }
      copy.shown = original_;
      return false;
    });
  }

  // Walks committed physical pages only, from where the previous call stopped;
  // untouched shared-memory pages are never read, so the scan does not commit
  // host memory. `budget` counts readable bytes examined.
  void ScanSome(rex::memory::Memory& memory, uint32_t budget) {
    auto* heap = memory.GetPhysicalHeap();
    while (budget) {
      rex::memory::HeapAllocationInfo info{};
      if (scan_cursor_ >= kPhysicalSize || !heap->QueryRegionInfo(scan_cursor_, &info) ||
          !info.region_size) {
        scan_cursor_ = 0;  // wrapped: one full sweep done
        return;
      }
      const uint32_t region_end = info.base_address + info.region_size;
      if (!IsReadable(info)) {
        scan_cursor_ = region_end;
        continue;
      }
      const uint32_t end = region_end - scan_cursor_ > budget ? scan_cursor_ + budget : region_end;
      ScanRegion(memory, scan_cursor_, end, region_end);
      budget -= end - scan_cursor_;
      scan_cursor_ = end;
    }
  }

  static bool IsReadable(const rex::memory::HeapAllocationInfo& info) {
    return (info.state & rex::memory::kMemoryAllocationCommit) &&
           (info.protect & rex::memory::kMemoryProtectRead);
  }

  static bool IsReadableCommitted(rex::memory::Memory& memory, uint32_t physical) {
    rex::memory::HeapAllocationInfo info{};
    return memory.GetPhysicalHeap()->QueryRegionInfo(physical, &info) && IsReadable(info) &&
           physical + kAtlasBytes <= info.base_address + info.region_size;
  }

  // Candidate pages in [start, end); an atlas may extend to region_end.
  void ScanRegion(rex::memory::Memory& memory, uint32_t start, uint32_t end,
                  uint32_t region_end) {
    for (uint32_t page = start; page < end && page + kAtlasBytes <= region_end;
         page += kGuestPageSize) {
      if (std::any_of(copies_.begin(), copies_.end(),
                      [page](const AtlasCopy& copy) { return copy.physical == page; })) {
        continue;
      }
      const uint8_t* bytes = memory.TranslatePhysical(page);
      if (std::memcmp(bytes + kAtlasSignatureOffset, kAtlasSignature.data(),
                      kAtlasSignature.size()) != 0 ||
          Fnv1a64({bytes, kAtlasBytes}) != kAtlasHash) {
        continue;
      }
      if (original_.empty()) original_.assign(bytes, bytes + kAtlasBytes);
      copies_.push_back({page, original_});
      std::fprintf(stderr, "[mgr-prompts] button atlas found at physical 0x%08X\n", page);
    }
  }

  // Rendered once per device family; re-rendered only when a keybind changes.
  const std::vector<uint8_t>& Patched(PromptFamily family) {
    const bool playstation = family != PromptFamily::kKeyboard;
    ButtonLabels labels =
        playstation ? PlayStationLabels(family == PromptFamily::kPlayStation3)
                    : BuildButtonLabels(
                          [](const char* name) { return rex::cvar::GetFlagByName(name); });
    Rendered& cache = rendered_[size_t(family)];
    if (cache.atlas.empty() || labels != cache.labels) {
      cache.labels = std::move(labels);
      cache.atlas = PatchAtlas(original_, cache.labels, RenderKeyIcon,
                               playstation ? kPlayStationRoundButtons : 0);
    }
    return cache.atlas;
  }

  static void Write(rex::memory::Memory& memory, AtlasCopy& copy,
                    const std::vector<uint8_t>& data, bool patched) {
    std::memcpy(memory.TranslatePhysical(copy.physical), data.data(), kAtlasBytes);
    copy.shown = data;
    // The physical view is not write-watched: notify each watched view so the
    // texture cache reloads the atlas.
    const bool in_e0_view = copy.physical >= kGuestPageSize;
    for (uint32_t view : {kViewA0 | copy.physical, kViewC0 | copy.physical,
                          kViewE0 + copy.physical - kGuestPageSize}) {
      if (view >= kViewE0 && !in_e0_view) continue;
      memory.TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                            view, kAtlasBytes, true, false);
    }
    std::fprintf(stderr, "[mgr-prompts] %s button atlas at 0x%08X\n",
                 patched ? "patched" : "restored", copy.physical);
  }

  std::vector<AtlasCopy> copies_;
  std::vector<uint8_t> original_;
  struct Rendered {
    ButtonLabels labels;
    std::vector<uint8_t> atlas;
  };
  std::array<Rendered, kPromptFamilyCount> rendered_;
  uint32_t frame_ = 0;
  uint32_t scan_cursor_ = 0;
};

AtlasPatcher patcher;  // VdSwap runs only on the guest render thread
}  // namespace

void MgrPromptsOnPresent() {
  if (auto* memory = REX_KERNEL_MEMORY()) patcher.OnPresent(*memory);
}
