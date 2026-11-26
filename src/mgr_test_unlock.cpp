// Test-only: MGR_TEST_UNLOCK_CHAPTERS=1 reports every chapter as cleared to the
// chapter menu, so scripted runs can open any chapter without finishing the
// story. sub_82C35600(stage, category) reads the cleared-chapter table at
// 0x836D0780 (5 categories per stage); category -1 asks about any category.
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdlib>
#include <string_view>

REX_EXTERN(__imp__sub_82C35600);

extern "C" REX_FUNC(sub_82C35600) {
  static const bool unlock_all = [] {
    const char* value = std::getenv("MGR_TEST_UNLOCK_CHAPTERS");
    return value && std::string_view(value) == "1";
  }();
  __imp__sub_82C35600(ctx, base);
  // Callers test the entry for non-zero; keep a stored non-zero entry as it is.
  if (unlock_all && ctx.r3.u32 == 0) ctx.r3.u64 = 1;
}
