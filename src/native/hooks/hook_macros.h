// Helpers for the native renderer's guest hooks.
//
// The D3D entry points are aliases (src/native/d3d_functions.h), so the
// original body's symbol has to be formed after the alias expands:
// MGRN_IMP(D3DDevice_SetTexture) is __imp__sub_826AEFD0.
#pragma once

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "d3d_functions.h"
#include "mgr_native.h"

#define MGRN_IMP_(name) __imp__##name
#define MGRN_IMP(name) MGRN_IMP_(name)
#define MGRN_EXTERN_(name) REX_EXTERN(__imp__##name)
#define MGRN_EXTERN(name) MGRN_EXTERN_(name)
#define MGRN_FUNC_(name) extern "C" REX_FUNC(name)
// A raw hook on a D3D alias; the body sees ctx/base and decides whether to
// call MGRN_IMP(name)(ctx, base).
#define MGRN_HOOK_RAW(name) MGRN_FUNC_(name)
