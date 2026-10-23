// Native renderer: write tracking for guest memory the renderer keeps host
// copies of (physical vertex/index buffers, texture mirrors).
//
// Built on the runtime's physical memory access callbacks: Arm write-protects
// the pages of a range, the first guest write to a protected page lands in the
// invalidation callback, which stamps the page with a new write generation and
// lets the write through. A copy taken right after Arm is stale once any of its
// pages carries a generation newer than the stamp Arm returned.
#pragma once

#include <rex/types.h>

namespace mgrn::gpu::write_watch {

using Stamp = u32;
// Returned by Arm for memory outside the physical heaps, which the runtime
// cannot watch; such copies need another staleness check.
inline constexpr Stamp kUnwatched = 0;

// Watches [va, va + size) and returns the stamp to keep with a copy of it.
// Read the guest bytes only after this returns.
Stamp Arm(u32 va, u32 size);

// True when the guest wrote into [va, va + size) after Arm returned 'stamp'.
bool WrittenSince(u32 va, u32 size, Stamp stamp);

}  // namespace mgrn::gpu::write_watch
