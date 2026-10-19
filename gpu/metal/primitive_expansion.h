#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace mgr::metal4 {

inline std::vector<uint32_t> TriangleFanIndices(std::span<const uint32_t> vertices) {
  std::vector<uint32_t> triangles;
  if (vertices.size() < 3) return triangles;
  triangles.reserve((vertices.size() - 2) * 3);
  for (size_t i = 2; i < vertices.size(); ++i) {
    triangles.insert(triangles.end(), {vertices[0], vertices[i - 1], vertices[i]});
  }
  return triangles;
}

}  // namespace mgr::metal4
