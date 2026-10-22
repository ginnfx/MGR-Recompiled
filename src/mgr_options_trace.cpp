// Diagnostic (MGR_OPTIONS_TRACE=1): calls the retail cOptionMenu makes while it
// exits and animates its cursor, with arguments, return value and caller. Each
// wrapper forwards to the original unchanged. Traced on 2026-10-22: the retail
// exit (B) is 82C40728(group, 0), phase 1 -> 4 (fade) -> 5, with no save call.
#include "generated/default/mgr_pch.h"

#include <cstdio>
#include <cstdlib>

#define MGR_TRACE_FUNC(addr, what)                                                       \
  REX_EXTERN(__imp__sub_##addr);                                                         \
  extern "C" REX_FUNC(sub_##addr) {                                                      \
    static const bool trace = std::getenv("MGR_OPTIONS_TRACE") != nullptr;               \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;                    \
    const uint32_t caller = uint32_t(ctx.lr);                                            \
    __imp__sub_##addr(ctx, base);                                                        \
    if (trace)                                                                           \
      std::fprintf(stderr,                                                               \
                   "[mgr-options-trace] " #addr " %s r3=%08X r4=%08X r5=%08X -> %08X "  \
                   "from %08X\n",                                                        \
                   what, r3, r4, r5, ctx.r3.u32, caller);                                \
  }

MGR_TRACE_FUNC(82C40728, "exit commit (r4=0 on B)")
MGR_TRACE_FUNC(829319B0, "phase-5 call")
MGR_TRACE_FUNC(82936CF0, "playAnimation(ui,node,state)")
