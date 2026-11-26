/**
 * @file    gpu/pipeline/msaa_twin.h
 * @brief   Alpha-test state of a scene pipeline at another sample count.
 *
 * Video::SetAlphaTestMode (draw_bindings.cpp) turns a clip-based alpha test
 * into alpha-to-coverage when the draw is multisampled, unblended and
 * transparency AA is on. A pipeline recorded at one sample must therefore
 * change both the A2C flag and the spec constant bit to match the pipeline
 * the draw will actually request after an MSAA change.
 */
#pragma once

#include <rex/types.h>

#include "gpu/shaders/shader_constants.h"

namespace mgrn::gpu {

struct AlphaTestVariant {
  u32 specConstants;
  bool alphaToCoverage;
};

inline AlphaTestVariant AlphaTestVariantFor(u32 specConstants, bool alphaBlend,
                                            bool transparencyAA, bool multisampled) {
  constexpr u32 kAlphaBits = kSpecConstantAlphaTest | kSpecConstantAlphaToCoverage;
  const bool alphaTested = (specConstants & kAlphaBits) != 0;
  const bool coverage = alphaTested && transparencyAA && !alphaBlend && multisampled;
  u32 out = specConstants & ~kAlphaBits;
  if (alphaTested)
    out |= coverage ? kSpecConstantAlphaToCoverage : kSpecConstantAlphaTest;
  return {out, coverage};
}

} // namespace mgrn::gpu
