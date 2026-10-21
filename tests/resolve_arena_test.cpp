#include <rex/graphics/vulkan/resolve_arena.h>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using rex::graphics::vulkan::ResolveArena;
int main() {
  ResolveArena arena;
  constexpr uint64_t page = 4096;
  arena.Reset(16 * page, 4, 256);
  std::vector<ResolveArena::Copy> copies;
  // A high guest address consumes only its resident range, not its prefix.
  constexpr uint64_t guest = 0x1f000000;
  assert(arena.Commit(guest, page, 1, 0, copies));
  uint64_t first;
  assert(arena.Lookup(guest + 20, 4, first) && first == 80);
  assert(copies.empty() && arena.live_bytes() == 4 * page);
  // Growing a range copies all old bytes and keeps old descriptors valid.
  assert(arena.Commit(guest + page - 4, 8, 2, 0, copies));
  assert(copies.size() == 1 && copies[0].source == 0 && copies[0].length == 4 * page);
  uint64_t moved;
  assert(arena.Lookup(guest, 2 * page, moved) && moved == 4 * page);
  // No space for a second two-page range until the copy's fence completes.
  assert(!arena.Commit(0x100000, 2 * page, 3, 1, copies));
  assert(arena.Lookup(guest, 2 * page, first) && first == moved);
  assert(arena.Commit(0x100000, page, 3, 2, copies));
  assert(arena.Lookup(0x100000, page, first) && first == 0);
  assert(!arena.Commit(ResolveArena::kGuestSize - 1, 2, 4, 3, copies));
  assert(!arena.Commit(UINT64_MAX, 2, 4, 3, copies));
  // Exercise merging two disjoint allocations against a byte-content model.
  arena.Reset(64 * page, 9, 256);
  std::vector<uint8_t> memory(64 * page, 0);
  assert(arena.Commit(page, page, 1, 0, copies));
  uint64_t a, b;
  assert(arena.Lookup(page, page, a));
  for (uint64_t i = 0; i < 9 * page; ++i) memory[a+i] = uint8_t(i * 7);
  assert(arena.Commit(3 * page, page, 1, 0, copies));
  assert(arena.Lookup(3 * page, page, b));
  for (uint64_t i = 0; i < 9 * page; ++i) memory[b+i] = uint8_t(i * 11);
  assert(arena.Commit(2 * page - 8, page + 16, 2, 0, copies));
  assert(copies.size() == 2);
  for (auto c : copies)
    for (uint64_t i = 0; i < c.length; ++i) memory[c.destination+i] = memory[c.source+i];
  assert(arena.Lookup(page, 3 * page, a));
  for (uint64_t i = 0; i < 9 * page; ++i) {
    assert(memory[a+i] == uint8_t(i * 7));
    assert(memory[a+18*page+i] == uint8_t(i * 11));
  }
  // Out of space: only an idle, GPU-completed block is evicted, oldest first.
  constexpr uint64_t idle = ResolveArena::kEvictIdleSubmissions;
  arena.Reset(4 * page, 1, 256);
  assert(arena.Commit(0, 2 * page, 1, 0, copies));          // A, last used at 1
  assert(arena.Commit(8 * page, 2 * page, 2, 0, copies));   // B, last used at 2
  assert(arena.Lookup(0, page, a, 3));                       // A bound again at 3
  // B is the least recently used but the GPU has not finished submission 2.
  assert(!arena.Commit(16 * page, 2 * page, 4 + idle, 1, copies));
  assert(arena.evictions() == 0);
  // Both are complete; B (last use 2) is older than A (last use 3).
  assert(arena.Commit(16 * page, 2 * page, 4 + idle, 3 + idle, copies));
  assert(arena.evictions() == 1 && copies.empty());
  assert(arena.Lookup(0, page, a) && !arena.Lookup(8 * page, page, b));
  // Recently used blocks are never evicted even when complete.
  assert(arena.Lookup(0, page, a, 4 + idle));
  assert(!arena.Commit(24 * page, 2 * page, 5 + idle, 4 + idle, copies));
  assert(arena.evictions() == 1);
  std::cout << "resolve arena: preservation, overlap, bounds, fence and eviction tests passed\n";
}
