// Scaled resolve memory addressing (step 6 reference): for every scale and
// block size, host texels of a scaled texture map one-to-one onto the bytes of
// their guest groups' scaled regions, and each host texel lands in the guest
// group that contains its center.
#include <rex/graphics/util/scaled_texture_address.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using rex::graphics::GetScaledGroupBlocksLog2;
using rex::graphics::GetScaledTexelAddressing;
using rex::graphics::ScaleRatio;

namespace {

void CheckBijection(ScaleRatio scale, uint32_t bytes_per_block_log2) {
  const auto group_log2 = GetScaledGroupBlocksLog2(bytes_per_block_log2);
  const uint32_t group_w = 1u << group_log2.x, group_h = 1u << group_log2.y;
  // Guest group bytes are 128 or 64; the scaled region is exactly x area.
  const uint32_t group_bytes = (group_w * group_h) << bytes_per_block_log2;
  assert(group_bytes == 128 || group_bytes == 64);
  const uint64_t region_bytes = rex::graphics::ScaleArea(scale, scale, group_bytes);
  constexpr uint32_t kGroupsX = 7, kGroupsY = 5;  // odd counts on purpose
  const uint32_t host_w = scale.ScaleExact(kGroupsX * group_w);
  const uint32_t host_h = scale.ScaleExact(kGroupsY * group_h);
  std::vector<uint8_t> hits(size_t(kGroupsX) * kGroupsY * region_bytes, 0);
  for (uint32_t y = 0; y < host_h; ++y) {
    for (uint32_t x = 0; x < host_w; ++x) {
      const uint32_t position[2] = {x, y};
      const auto a = GetScaledTexelAddressing(position, scale, scale, bytes_per_block_log2);
      assert(a.guest_group_origin[0] % group_w == 0 && a.guest_group_origin[1] % group_h == 0);
      const uint32_t gx = a.guest_group_origin[0] / group_w, gy = a.guest_group_origin[1] / group_h;
      assert(gx < kGroupsX && gy < kGroupsY);
      assert(a.host_byte_offset_in_guest_group % (1u << bytes_per_block_log2) == 0);
      assert(a.host_byte_offset_in_guest_group < region_bytes);
      if (!scale.IsInteger()) {
        // The guest texel containing the host texel center lies in this group.
        const uint32_t guest_x = scale.UnscaleFloor(2 * x + 1) / 2;
        const uint32_t guest_y = scale.UnscaleFloor(2 * y + 1) / 2;
        assert(guest_x / group_w == gx && guest_y / group_h == gy);
      }
      const size_t base = (size_t(gy) * kGroupsX + gx) * region_bytes;
      for (uint32_t b = 0; b < (1u << bytes_per_block_log2); ++b)
        assert(hits[base + a.host_byte_offset_in_guest_group + b]++ == 0);
    }
  }
  for (uint8_t h : hits) assert(h == 1);  // no gaps: every region byte written once
}

// Shaders load and store whole host-group rows (and 16-byte pieces of them)
// from one computed address: within a row, texel x is at row start + x * bpb.
void CheckHostGroupRowsContiguous(ScaleRatio scale, uint32_t bytes_per_block_log2) {
  const auto h = rex::graphics::GetScaledHostGroupBlocksLog2(scale, scale, bytes_per_block_log2);
  const uint32_t width = 1u << h.x;
  for (uint32_t y = 0; y < 48; ++y) {
    for (uint32_t x0 = 0; x0 < 96; x0 += width) {
      const uint32_t p0[2] = {x0, y};
      const auto a0 = GetScaledTexelAddressing(p0, scale, scale, bytes_per_block_log2);
      // Row starts are 8-byte aligned (16-byte when the row is 16 bytes or more).
      const uint32_t row_bytes = width << bytes_per_block_log2;
      assert(a0.host_byte_offset_in_guest_group % (row_bytes >= 16 ? 16 : row_bytes) == 0);
      for (uint32_t i = 1; i < width; ++i) {
        const uint32_t p[2] = {x0 + i, y};
        const auto a = GetScaledTexelAddressing(p, scale, scale, bytes_per_block_log2);
        assert(a.guest_group_origin[0] == a0.guest_group_origin[0]);
        assert(a.host_byte_offset_in_guest_group ==
               a0.host_byte_offset_in_guest_group + (i << bytes_per_block_log2));
      }
    }
  }
}

}  // namespace

int main() {
  const ScaleRatio scales[] = {ScaleRatio::Integer(1), ScaleRatio::Integer(2),
                               ScaleRatio::Integer(3), ScaleRatio{3, 2}};
  for (const ScaleRatio scale : scales)
    for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) CheckBijection(scale, bpb_log2);

  // Integer scale 1 is the identity on guest group bytes.
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    const uint32_t p[2] = {5, 3};
    const auto a = GetScaledTexelAddressing(p, ScaleRatio::Integer(1), ScaleRatio::Integer(1),
                                            bpb_log2);
    const auto g = GetScaledGroupBlocksLog2(bpb_log2);
    const uint32_t local_x = p[0] & ((1u << g.x) - 1), local_y = p[1] & ((1u << g.y) - 1);
    assert(a.host_byte_offset_in_guest_group ==
           (((local_y << g.x) + local_x) << bpb_log2));
  }
  // 3/2 host groups: half the guest group per axis, 3x3 per guest group.
  const uint32_t expected[5][2] = {{8, 4}, {8, 2}, {8, 1}, {2, 1}, {1, 1}};
  const ScaleRatio half3{3, 2};
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    const auto h = rex::graphics::GetScaledHostGroupBlocksLog2(half3, half3, bpb_log2);
    assert((1u << h.x) == expected[bpb_log2][0] && (1u << h.y) == expected[bpb_log2][1]);
    CheckHostGroupRowsContiguous(half3, bpb_log2);
    CheckHostGroupRowsContiguous(ScaleRatio::Integer(2), bpb_log2);
  }
  // Integer scales keep the upstream layout: host group = guest group.
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    const auto g = GetScaledGroupBlocksLog2(bpb_log2);
    const auto h = rex::graphics::GetScaledHostGroupBlocksLog2(ScaleRatio::Integer(3),
                                                               ScaleRatio::Integer(3), bpb_log2);
    assert(g.x == h.x && g.y == h.y);
  }
  std::cout << "scaled texture address: one-to-one group regions at 1x/2x/3x/3:2, "
               "center-in-group and contiguous host-group rows at 3:2\n";
}
