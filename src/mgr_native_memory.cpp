// Recover the high-level CRT memcpy contract rather than reimplementing its
// PowerPC alignment, cache-line prefetch and endian-shuffle state machine.
#include "mgr_native_memory.h"
#include "mgr_guest_ranges.h"
#include <rex/ppc/context.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_822F67E0);
namespace {
bool Overlaps(uint64_t a, uint64_t asize, uint64_t b, uint64_t bsize) {
  return a < b + bsize && b < a + asize;
}
}
bool MgrRunNativeCopy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, count = ctx.r5.u32;
  const uint32_t sp = ctx.r1.u32;
  if (ctx.r5.u64 != count) return false;
  if (!count) return true;
  if (!MgrIsOrdinaryGuestRange(dst, count) || !MgrIsOrdinaryGuestRange(src, count) || sp < 8 ||
      Overlaps(dst, count, src, count) ||
      Overlaps(dst, count, sp - 8, 8) || Overlaps(src, count, sp - 8, 8)) return false;
  // Host byte copy needs no endian conversion; the original's load/store
  // byte swaps cancel. Guest r3 retains the full incoming return-pointer value.
  // Native stores hit the same memory protection / write-watch handlers. Those
  // retry ordinary RAM writes without decoding the faulting instruction.
  std::memcpy(base + dst, base + src, count);
  return true;
}
extern "C" REX_FUNC(sub_822F67E0) {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_NATIVE_MEMORY");
    return !value || value[0] != '0';
  }();
  if (!enabled || !MgrRunNativeCopy(ctx, base)) __imp__sub_822F67E0(ctx, base);
}
