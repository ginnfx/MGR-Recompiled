#include "mgr_render_writes.h"
#include "mgr_resolve_coherence.h"
#include <cassert>
#include <iostream>
#include <array>
#include <random>

int main() {
  unsigned cases = 0;
  for (unsigned mask = 0; mask < 65536; ++mask)
    for (unsigned controls = 0; controls < 8; ++controls) {
      const bool raster = controls & 1, depth = controls & 2, stencil = controls & 4;
      const unsigned writes = mgr::RenderTargetWriteMask(raster, depth, stencil, mask);
      assert(bool(writes & 1) == (raster && (depth || stencil)));
      for (unsigned rt = 0; rt < 4; ++rt)
        assert(bool(writes & (2u << rt)) == (raster && (mask & (15u << (4*rt)))));
      ++cases;
    }
  // A read-only attachment survives drawing to another target; subsequent
  // writes, transfers and clears must still invalidate its snapshot.
  mgr::ResolveCoherence<32> cache;
  cache.MarkCurrent(1, 0, 4, 1, 8);  // depth
  cache.MarkCurrent(2, 8, 4, 1, 8);  // color
  auto draw = [&](unsigned writes) {
    if (writes & 1) cache.InvalidateOwner(1);
    if (writes & 2) cache.InvalidateOwner(2);
  };
  draw(mgr::RenderTargetWriteMask(true, false, false, 15));
  assert(cache.IsCurrent(1, 0, 4, 1, 8));
  assert(!cache.IsCurrent(2, 8, 4, 1, 8));
  draw(mgr::RenderTargetWriteMask(true, true, false, 0));
  assert(!cache.IsCurrent(1, 0, 4, 1, 8));
  cache.MarkCurrent(1, 0, 4, 1, 8);
  draw(mgr::RenderTargetWriteMask(true, false, true, 0));
  assert(!cache.IsCurrent(1, 0, 4, 1, 8));
  cache.MarkCurrent(1, 0, 4, 1, 8);
  cache.InvalidateOwner(1);  // transfer/clear ignores draw write mask
  assert(!cache.IsCurrent(1, 0, 4, 1, 8));
  // Compare actual contents with unconditional copying through mixed writes,
  // read-only bindings, owner aliases, clears and repeated resolves.
  mgr::ResolveCoherence<32> conservative, selective;
  std::array<std::array<uint64_t, 32>, 5> images{};
  std::array<uint64_t, 32> reference{}, old_edram{}, new_edram{};
  std::mt19937 rng(0xD3D);
  uint64_t version = 1, old_skips = 0, new_skips = 0;
  for (auto& image : images) for (auto& tile : image) tile = version++;
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    const unsigned controls = rng()%8, colors = rng()%65536;
    const unsigned writes = mgr::RenderTargetWriteMask(controls&1, controls&2, controls&4, colors);
    for (unsigned owner = 0; owner < 5; ++owner) {
      conservative.InvalidateOwner(owner+1);  // accumulated binding policy
      if (writes & (1u << owner)) {
        selective.InvalidateOwner(owner+1);
        for (auto& tile : images[owner]) tile = version++;
      }
    }
    if (rng()%17 == 0) {  // clear/transfer is independent of draw controls
      const unsigned owner = rng()%5;
      conservative.InvalidateOwner(owner+1);
      selective.InvalidateOwner(owner+1);
      for (auto& tile : images[owner]) tile = version++;
    }
    const unsigned owner = rng()%5, base = rng()%28;
    auto resolve = [&](auto& metadata, auto& memory, uint64_t& skips) {
      if (metadata.IsCurrent(owner+1, base, 4, 1, 8)) { ++skips; return; }
      for (unsigned i = base; i < base+4; ++i) memory[i] = images[owner][i];
      metadata.MarkCurrent(owner+1, base, 4, 1, 8);
    };
    for (unsigned i = base; i < base+4; ++i) reference[i] = images[owner][i];
    resolve(conservative, old_edram, old_skips);
    resolve(selective, new_edram, new_skips);
    assert(reference == old_edram && reference == new_edram);
  }
  assert(new_skips > old_skips);
  std::cout << "render writes: " << cases << " exhaustive state cases and read-only, depth, stencil, transfer invalidation checks passed\n";
  std::cout << "100000 mixed draw/resolve content comparisons passed; conservative skips="
            << old_skips << ", selective skips=" << new_skips << " (model only)\n";
}
