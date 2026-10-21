#pragma once
#include <rex/ppc/func.h>

enum class MgrNativeMathOp { PackHalf, UnpackHalf, TransformNormal, Transpose };
// Returns false for unsupported guest layouts; caller must run the original.
bool MgrRunNativeMath(MgrNativeMathOp op, PPCContext& ctx, uint8_t* base);
void MgrDispatchNativeMath(MgrNativeMathOp op, PPCContext& ctx, uint8_t* base,
                          PPCFunc* original);

void MgrNativeMathOnPresent();
