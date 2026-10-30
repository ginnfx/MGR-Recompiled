#include "native/gpu/upscale_math.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace mgrn::gpu::upscale;

namespace {

bool Near(f64 a, f64 b, f64 tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

bool NearMatrix(const Matrix &a, const Matrix &b, f64 tolerance = 1e-9) {
  for (u32 i = 0; i < 16; ++i)
    if (!Near(a[i], b[i], tolerance))
      return false;
  return true;
}

// A perspective view-projection: 90 degree FOV, 16:9, reversed depth, looking
// down -z from 'eye_x' along x.
Matrix Camera(f64 eye_x) {
  const f64 f = 1.0, aspect = 16.0 / 9.0, near_z = 0.1, far_z = 1000.0;
  const Matrix projection = {f / aspect, 0, 0, 0,
                             0, f, 0, 0,
                             0, 0, near_z / (far_z - near_z), far_z * near_z / (far_z - near_z),
                             0, 0, -1, 0};
  const Matrix view = {1, 0, 0, -eye_x, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  return Multiply(projection, view);
}

} // namespace

int main() {
  // Multiply and Invert round-trip; a singular matrix is refused.
  const Matrix camera = Camera(0.25);
  Matrix inverse{};
  assert(Invert(camera, inverse));
  assert(NearMatrix(Multiply(camera, inverse), kIdentity));
  assert(NearMatrix(Multiply(kIdentity, camera), camera));
  const Matrix singular = {1, 2, 3, 4, 2, 4, 6, 8, 0, 0, 1, 0, 0, 0, 0, 1};
  assert(!Invert(singular, inverse));

  // An unmoved camera reprojects every point onto itself.
  Matrix reproject{};
  assert(CameraReprojection(camera, camera, 0.5, reproject));
  assert(NearMatrix(reproject, kIdentity));
  assert(Near(MaxProbeMotion(reproject), 0.0));

  // A small step is history; a large one is a cut.
  assert(CameraReprojection(Camera(0.26), camera, 0.5, reproject));
  assert(MaxProbeMotion(reproject) > 0.0);
  assert(!CameraReprojection(Camera(50.0), camera, 0.5, reproject));
  // No history through a singular current camera.
  assert(!CameraReprojection(singular, camera, 0.5, reproject));
  // A probe landing behind the previous camera counts as infinite motion.
  const Matrix flip = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1};
  assert(std::isinf(MaxProbeMotion(flip)));

  // Halton(2) and Halton(3) start 1/2, 1/4, 3/4 and 1/3, 2/3, 1/9.
  assert(Halton(1, 2) == 0.5f && Halton(2, 2) == 0.25f && Halton(3, 2) == 0.75f);
  assert(Near(Halton(1, 3), 1.0 / 3.0, 1e-6) && Near(Halton(2, 3), 2.0 / 3.0, 1e-6));
  assert(Near(Halton(3, 3), 1.0 / 9.0, 1e-6));
  assert(Halton(0, 2) == 0.0f);

  // Phases: 8 per output pixel area, within [8, 32].
  assert(JitterPhases(1.0f) == 8);
  assert(JitterPhases(1.5f) == 18);
  assert(JitterPhases(2.0f) == 32);
  assert(JitterPhases(3.0f) == 32);
  assert(JitterPhases(0.5f) == 8);

  std::cout << "upscale_math: all checks passed\n";
  return 0;
}
