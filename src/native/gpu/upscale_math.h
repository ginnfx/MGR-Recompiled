/**
 * @file    gpu/upscale_math.h
 * @brief   The upscaler's camera and jitter math: 4x4 matrices, the camera
 *          reprojection and cut test, and the Halton jitter sequence.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <rex/types.h>

namespace mgrn::gpu::upscale {

using Matrix = std::array<f64, 16>; // row-major: element (row, col) at row * 4 + col

inline constexpr Matrix kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

inline Matrix Multiply(const Matrix &a, const Matrix &b) {
  Matrix r{};
  for (u32 row = 0; row < 4; ++row)
    for (u32 col = 0; col < 4; ++col)
      for (u32 k = 0; k < 4; ++k)
        r[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
  return r;
}

// Gauss-Jordan with partial pivoting; false when 'm' is singular.
inline bool Invert(const Matrix &m, Matrix &inverse) {
  constexpr f64 kSingularPivot = 1e-12;
  std::array<f64, 32> a{}; // [m | I], 8 columns per row
  for (u32 row = 0; row < 4; ++row) {
    for (u32 col = 0; col < 4; ++col)
      a[row * 8 + col] = m[row * 4 + col];
    a[row * 8 + 4 + row] = 1.0;
  }
  for (u32 col = 0; col < 4; ++col) {
    u32 pivot = col;
    for (u32 row = col + 1; row < 4; ++row)
      if (std::fabs(a[row * 8 + col]) > std::fabs(a[pivot * 8 + col]))
        pivot = row;
    if (std::fabs(a[pivot * 8 + col]) < kSingularPivot)
      return false;
    for (u32 k = 0; k < 8; ++k)
      std::swap(a[col * 8 + k], a[pivot * 8 + k]);
    const f64 scale = 1.0 / a[col * 8 + col];
    for (u32 k = 0; k < 8; ++k)
      a[col * 8 + k] *= scale;
    for (u32 row = 0; row < 4; ++row) {
      if (row == col)
        continue;
      const f64 factor = a[row * 8 + col];
      for (u32 k = 0; k < 8; ++k)
        a[row * 8 + k] -= factor * a[col * 8 + k];
    }
  }
  for (u32 row = 0; row < 4; ++row)
    for (u32 col = 0; col < 4; ++col)
      inverse[row * 4 + col] = a[row * 8 + 4 + col];
  return true;
}

// Largest NDC distance a probe point at mid depth moves under 'reproject', or
// infinity when one lands behind the previous camera.
inline f64 MaxProbeMotion(const Matrix &reproject) {
  constexpr f64 kProbes[][2] = {{0, 0}, {-0.8, -0.8}, {0.8, -0.8}, {-0.8, 0.8}, {0.8, 0.8}};
  constexpr f64 kProbeDepth = 0.5;
  f64 largest = 0.0;
  for (const auto &p : kProbes) {
    const f64 in[4] = {p[0], p[1], kProbeDepth, 1.0};
    f64 out[4] = {};
    for (u32 row = 0; row < 4; ++row)
      for (u32 k = 0; k < 4; ++k)
        out[row] += reproject[row * 4 + k] * in[k];
    if (out[3] <= 0.0)
      return INFINITY;
    largest = std::max(largest, std::hypot(out[0] / out[3] - p[0], out[1] / out[3] - p[1]));
  }
  return largest;
}

// Takes this frame's NDC position to the previous frame's clip position, or
// false when there is no usable history: no previous camera, a singular
// projection, or a cut (a probe moving more than 'cut_ndc').
inline bool CameraReprojection(const Matrix &view_proj, const Matrix &previous_view_proj,
                               f64 cut_ndc, Matrix &reproject) {
  Matrix inverse{};
  if (!Invert(view_proj, inverse))
    return false;
  reproject = Multiply(previous_view_proj, inverse);
  return MaxProbeMotion(reproject) <= cut_ndc;
}

// Element 'index' (from 1) of the Halton sequence in 'base', in [0, 1).
inline f32 Halton(u32 index, u32 base) {
  f32 result = 0.0f, fraction = 1.0f;
  for (u32 i = index; i > 0; i /= base) {
    fraction /= f32(base);
    result += fraction * f32(i % base);
  }
  return result;
}

// Jitter phases for an output/input 'ratio' per axis: 8 per output pixel area
// (the FSR and MetalFX guidance), within [8, 32].
inline u32 JitterPhases(f32 ratio) {
  constexpr f32 kPhasesPerArea = 8.0f;
  constexpr u32 kMinPhases = 8, kMaxPhases = 32;
  const u32 phases = u32(std::lround(kPhasesPerArea * ratio * ratio));
  return std::clamp(phases, kMinPhases, kMaxPhases);
}

} // namespace mgrn::gpu::upscale
