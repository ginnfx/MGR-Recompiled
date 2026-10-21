#pragma once
#include <rex/ppc/func.h>
// Native copy is restricted to ordinary, disjoint RAM. Unsupported guest
// layouts retain the original's exact access ordering and address translation.
bool MgrRunNativeCopy(PPCContext& ctx, uint8_t* base);
