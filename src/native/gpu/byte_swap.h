/**
 * @file    gpu/byte_swap.h
 * @brief   Per-element byte swap for guest big-endian buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace mgrn::gpu {

// Byte-swaps 'bytes' worth of elements from src (guest big-endian) into dst
// (host little-endian). element_size == 2 swaps per u16 (index buffers), and
// any other value swaps per u32 (vertex buffers, dwords).
inline void ByteSwapElements(void *dst, const void *src, u32 bytes,
                             u32 element_size) {
  if (element_size == 2) {
    rex::memory::copy_and_swap(static_cast<u16 *>(dst),
                               static_cast<const u16 *>(src), bytes / 2);
  } else {
    rex::memory::copy_and_swap(static_cast<u32 *>(dst),
                               static_cast<const u32 *>(src), bytes / 4);
  }
}

// Xenos indexed draws use only the low 24 bits of a 32-bit index. Vulkan and
// Plume use the full 32-bit value, and Plume's strip restart marker is fixed
// at UINT32_MAX. Normalize both properties while making an INDEX32 host copy.
// 0xFFFFFF cannot name a vertex in MGR's 16-bit draw-count range, so it is
// reserved for Xenos primitive restart and maps safely to the host marker.
inline void ByteSwapIndexElements(void *dst, const void *src, u32 bytes,
                                  u32 element_size) {
  if (element_size != 4) {
    ByteSwapElements(dst, src, bytes, element_size);
    return;
  }
  const auto *in = static_cast<const u32 *>(src);
  auto *out = static_cast<u32 *>(dst);
  for (u32 i = 0; i < bytes / sizeof(u32); ++i) {
    const u32 index = rex::byte_swap(in[i]) & 0x00FFFFFFu;
    out[i] = index == 0x00FFFFFFu ? 0xFFFFFFFFu : index;
  }
}

} // namespace mgrn::gpu
