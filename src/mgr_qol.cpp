// Quality-of-life options for 4B4E080A / 0.0.0.4, after the PC Fix and
// SilentPatchMGR feature sets. Strong symbols override the generated weak
// entry points; __imp__sub_* is the original implementation.
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstring>

REX_EXTERN(__imp__sub_82BC9920);
REX_EXTERN(__imp__sub_82852138);

REXCVAR_DEFINE_BOOL(mgr_skip_intro_logos, false, "MGR/Game",
                    "Skip the publisher and middleware logos at boot (as PC Fix and "
                    "SilentPatchMGR do). Applies to remaining logos; loading still completes first");
REXCVAR_DEFINE_BOOL(mgr_film_grain, true, "MGR/Graphics",
                    "Film grain (noise) in the final post-process filter. Off as in the PC "
                    "Fix 'disable noise filter'; also saves a little GPU time");

namespace {
// Logo sequence object (384 bytes, built by sub_82BC9040): word +380 is the
// "sequence finished" flag the boot loop sub_82BCCC70 polls in its last phase.
constexpr uint32_t kLogoFinishedOffset = 380;
// Final gather filter shader objects (loaded by sub_82BE3270 from
// FilterShaderGather.* and FilterShaderGatherNoise.*). sub_82BEA210 itself
// substitutes Gather for GatherNoise in some scenes with the same arguments.
constexpr uint32_t kGatherShader = 0x8394BAEC;
constexpr uint32_t kGatherNoiseShader = 0x8394BB80;

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
}  // namespace

// Logo sequence update, called once per boot-loop frame with r3 = the object.
extern "C" REX_FUNC(sub_82BC9920) {
  const uint32_t sequence = ctx.r3.u32;
  __imp__sub_82BC9920(ctx, base);
  if (sequence && REXCVAR_GET(mgr_skip_intro_logos)) {
    StoreU32(base, sequence + kLogoFinishedOffset, 1);
  }
}

// Draws a full-screen filter pass with r3 = shader object.
extern "C" REX_FUNC(sub_82852138) {
  if (ctx.r3.u32 == kGatherNoiseShader && !REXCVAR_GET(mgr_film_grain)) {
    ctx.r3.u64 = kGatherShader;
  }
  __imp__sub_82852138(ctx, base);
}
