// MGR_OPTIONS_DUMP=<file> (diagnostic): layout of the retail Options UI nodes.
#include "mgr_options_dump.h"

#include "generated/default/mgr_pch.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
// Constants of the cOptionMenu layout (see mgr_native_options.cpp).
constexpr uint32_t kGroupOffsets[] = {28, 296, 564};
constexpr uint32_t kTextVtable = 0x821CBF50;
constexpr uint32_t kCursorNode = 860, kGroupContainers = 848;

uint32_t U32(const uint8_t* base, uint32_t a) {
  uint32_t v; std::memcpy(&v, REX_RAW_ADDR(a), 4); return __builtin_bswap32(v);
}
uint32_t Node(const uint8_t* base, uint32_t owner, uint32_t id) {
  const uint32_t ui = U32(base, owner + 24);
  if (!ui || id >= U32(base, ui + 124)) return 0;
  return U32(base, ui + 120) + id * 1024;
}
}  // namespace

// Writes every UI node's non-zero words and
// the group's slot id arrays, to measure the retail layout.
void MgrDumpOptionsLayout(uint8_t* base, uint32_t object) {
  const char* path = std::getenv("MGR_OPTIONS_DUMP");
  if (!path || !*path) return;
  std::FILE* out = std::fopen(path, "w");
  if (!out) return;
  const uint32_t ui = U32(base, object + 24), count = U32(base, ui + 124);
  std::fprintf(out, "object=%08X ui=%08X nodes=%u cursor id=%u\n", object, ui, count, U32(base, object + kCursorNode));
  const uint32_t group = object + 28;
  for (uint32_t offset = 40; offset < 136; offset += 4) {
    std::fprintf(out, "group+%u id=%u\n", offset, U32(base, group + offset));
  }
  for (uint32_t k = 0; k < 3; ++k) {
    std::fprintf(out, "container[%u] id=%u\n", k, U32(base, object + kGroupContainers + k * 4));
  }
  for (uint32_t offset : kGroupOffsets) {
    const uint32_t owner = object + offset, group_ui = U32(base, owner + 24);
    std::fprintf(out, "group@+%u ui=%08X nodes=%u text ids:", offset, group_ui, group_ui ? U32(base, group_ui + 124) : 0);
    for (uint32_t id = 0; group_ui && id < U32(base, group_ui + 124); ++id) {
      const uint32_t node = Node(base, owner, id), widget = U32(base, node + 1008);
      if (widget && U32(base, widget) == kTextVtable)
        std::fprintf(out, " %u(x=%.0f,y=%.0f)", id, std::bit_cast<float>(U32(base, node)), std::bit_cast<float>(U32(base, node + 4)));
    }
    std::fprintf(out, "\n");
    for (uint32_t array : {40u, 64u, 88u, 112u}) {
      std::fprintf(out, "  group@+%u array+%u:", offset, array);
      for (uint32_t i = 0; i < 6; ++i) {
        const uint32_t id = U32(base, owner + array + i * 4), node = Node(base, owner, id);
        std::fprintf(out, " %u(y=%.1f vis=%u)", id, node ? std::bit_cast<float>(U32(base, node + 4)) : -1.f, node ? U32(base, node + 944) : 0);
      }
      std::fprintf(out, "\n");
    }
  }
  for (uint32_t id = 0; id < count; ++id) {
    const uint32_t node = Node(base, object, id);
    std::fprintf(out, "node %u @%08X:", id, node);
    for (uint32_t off = 0; off < 1024; off += 4) {
      const uint32_t v = U32(base, node + off);
      if (v) std::fprintf(out, " %u=%08X", off, v);
    }
    std::fprintf(out, "\n");
  }
  // Widget vtable per node of the outer UI and each group UI (sprite vs text).
  const uint32_t owners[] = {object, object + kGroupOffsets[0], object + kGroupOffsets[1], object + kGroupOffsets[2]};
  for (uint32_t owner : owners) {
    const uint32_t owner_ui = U32(base, owner + 24);
    std::fprintf(out, "widgets owner=%08X:", owner);
    for (uint32_t id = 0; owner_ui && id < U32(base, owner_ui + 124); ++id) {
      const uint32_t node = Node(base, owner, id), widget = U32(base, node + 1008);
      if (widget) std::fprintf(out, " %u:%08X/vt%08X", id, widget, U32(base, widget));
    }
    std::fprintf(out, "\n");
  }
  // Sprite widgets (vtable 821CBF14): raw words and one pointer level down.
  for (uint32_t id : {2u, 3u, 9u, 33u, 34u}) {
    const uint32_t widget = U32(base, Node(base, object, id) + 1008);
    std::fprintf(out, "sprite node %u widget %08X:", id, widget);
    for (uint32_t off = 0; off < 320; off += 4) std::fprintf(out, " %u=%08X", off, U32(base, widget + off));
    std::fprintf(out, "\n");
    for (uint32_t off = 4; off < 320; off += 4) {
      const uint32_t ptr = U32(base, widget + off);
      if (ptr < 0x40000000 || (ptr & 3)) continue;
      std::fprintf(out, "  ptr@%u=%08X:", off, ptr);
      for (uint32_t k = 0; k < 96; k += 4) std::fprintf(out, " %08X", U32(base, ptr + k));
      std::fprintf(out, "\n");
    }
  }
  // The controller diagram's texture buffer (guest F1D5F000 in the 2026-11-26 trace):
  // header words and the first texture bytes, to compare with core.wtb.
  {
    const uint32_t widget = U32(base, Node(base, object, 9) + 1008);
    const uint32_t holder = U32(base, widget + 44), buffer = U32(base, holder + 4);
    std::fprintf(out, "texture buffer %08X header:", buffer);
    for (uint32_t off = 0; off < 0xE0; off += 4) std::fprintf(out, " %08X", U32(base, buffer + off));
    std::fprintf(out, "\ntexture data @+0x1000:");
    for (uint32_t off = 0; off < 64; off += 4) std::fprintf(out, " %08X", U32(base, buffer + 0x1000 + off));
    std::fprintf(out, "\n");
  }
  std::fclose(out);
  std::fprintf(stderr, "[mgr-native-options] layout dumped to %s (%u nodes)\n", path, count);
}
