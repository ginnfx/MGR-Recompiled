// AlphaTestVariantFor mirrors Video::SetAlphaTestMode's coverage rule.
#include "gpu/pipeline/msaa_twin.h"

#include <cassert>

using namespace mgrn::gpu;

int main() {
  const u32 other = 0x4;  // unrelated spec bit must survive
  auto r = AlphaTestVariantFor(kSpecConstantAlphaTest | other, false, true, true);
  assert(r.alphaToCoverage && (r.specConstants & kSpecConstantAlphaToCoverage) &&
         !(r.specConstants & kSpecConstantAlphaTest) && (r.specConstants & other));
  r = AlphaTestVariantFor(kSpecConstantAlphaToCoverage, false, true, false);
  assert(!r.alphaToCoverage && r.specConstants == kSpecConstantAlphaTest);
  r = AlphaTestVariantFor(kSpecConstantAlphaTest, true, true, true);  // blended keeps clip
  assert(!r.alphaToCoverage && r.specConstants == kSpecConstantAlphaTest);
  r = AlphaTestVariantFor(kSpecConstantAlphaTest, false, false, true);  // TAA off
  assert(!r.alphaToCoverage && r.specConstants == kSpecConstantAlphaTest);
  r = AlphaTestVariantFor(other, false, true, true);  // not alpha tested
  assert(!r.alphaToCoverage && r.specConstants == other);
  // One-sample normalisation used as the remembered-pipeline key: the MSAA 4
  // form (coverage bit) and the MSAA 1 form (clip bit) collapse to one value.
  const auto at4 = AlphaTestVariantFor(kSpecConstantAlphaTest | other, false, true, true);
  const auto norm4 = AlphaTestVariantFor(at4.specConstants, false, false, false);
  const auto norm1 = AlphaTestVariantFor(kSpecConstantAlphaTest | other, false, false, false);
  assert(norm4.specConstants == norm1.specConstants && !norm4.alphaToCoverage &&
         !norm1.alphaToCoverage);
  // Round trip: normalising then re-applying MSAA gives the MSAA form again.
  const auto back = AlphaTestVariantFor(norm4.specConstants, false, true, true);
  assert(back.specConstants == at4.specConstants && back.alphaToCoverage);
  return 0;
}
