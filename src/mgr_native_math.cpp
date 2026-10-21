// Native reconstruction of the XDK memory-oriented math API in 4B4E080A v0.0.0.4.
// The ABI boundary carries guest addresses; algorithms use host values and never
// stage a value through the emulated vector bank or the guest stack.
#include "mgr_native_math.h"
#include "mgr_guest_ranges.h"
#include <rex/ppc/context.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REX_EXTERN(__imp__sub_826CBFB8);
REX_EXTERN(__imp__sub_826CC010);
REX_EXTERN(__imp__sub_826CC160);

namespace {
std::atomic<uint64_t> live_checks[2]{};
template<class T> T Load(const uint8_t* base, uint32_t address) {
  T word;
  std::memcpy(&word, base + address, sizeof word);
  return std::byteswap(word);
}
template<class T> void Store(uint8_t* base, uint32_t address, T word) {
  word = std::byteswap(word);
  std::memcpy(base + address, &word, sizeof word);
}
bool Intersects(uint32_t a, uint64_t size, uint64_t b, uint64_t end) {
  return uint64_t(a) < end && b < uint64_t(a) + size;
}
bool SafeRange(uint32_t a, uint64_t size) { return MgrIsOrdinaryGuestRange(a, size); }
bool SafeArray(const PPCContext& ctx, unsigned out_stride, unsigned in_stride) {
  const uint64_t count = ctx.r5.u32;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, sp = ctx.r1.u32;
  // Preserve pathological wrapping and stack-alias behavior through the oracle.
  return (sp & 15) == 0 && sp >= 16 &&
      SafeRange(dst, count * out_stride) && SafeRange(src, count * in_stride) &&
      !Intersects(dst, count * out_stride, sp - 16, sp) &&
      !Intersects(src, count * in_stride, sp - 16, sp);
}
// Matches the GENERATED vpkd3d128 implementation exactly. Its cutoff is 65504,
// its mantissa truncates, and its small values use gradual underflow. Replacing
// this with _Float16 or the GPU's extended-range helper changes behavior.
uint16_t PackHalf(uint32_t bits) {
  const uint32_t sign = (bits >> 16) & 0x8000;
  const uint32_t magnitude = bits & 0x7fffffff;
  if (magnitude > 0x477fe000) return uint16_t(sign | 0x7fff);
  const unsigned exponent = magnitude >> 23;
  const unsigned mantissa = (magnitude >> 13) & 0x3ff;
  if (exponent > 112) return uint16_t(sign | ((exponent - 112) << 10) | mantissa);
  const unsigned shift = 113 - exponent;
  return uint16_t(sign | (shift > 31 ? 0 : ((0x400 + mantissa) >> shift)));
}
uint32_t UnpackHalf(uint16_t bits) {
  const uint32_t sign = uint32_t(bits & 0x8000) << 16;
  const uint32_t exponent = bits & 0x7c00;
  // vupkd3d flushes exponent-zero values; exponent 31 is a finite extended
  // value, including encodings that IEEE half would call infinity or NaN.
  return exponent ? sign | ((exponent + 0x1c000) << 13) |
                              (uint32_t(bits & 0x3ff) << 13) : sign;
}
using Matrix = std::array<std::array<uint32_t, 4>, 4>;
Matrix LoadMatrix(const uint8_t* base, uint32_t src) {
  Matrix m;
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c) m[r][c] = Load<uint32_t>(base, src + 16*r + 4*c);
  return m;
}
}

bool MgrRunNativeMath(MgrNativeMathOp op, PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (op == MgrNativeMathOp::PackHalf || op == MgrNativeMathOp::UnpackHalf) {
    const bool pack = op == MgrNativeMathOp::PackHalf;
    const uint32_t count = ctx.r5.u32;
    if (!count) return true;  // No memory access or FP-mode change on the empty path.
    if (!SafeArray(ctx, pack ? 2 : 4, pack ? 4 : 2)) return false;
    // Forward order is deliberate: overlapping arrays have the same semantics
    // as the original, including expansion over not-yet-read source elements.
    if (pack) {
      for (uint32_t i = 0; i < count; ++i)
        Store<uint16_t>(base, dst + 2*i, PackHalf(Load<uint32_t>(base, src + 4*i)));
      ctx.fpscr.enableFlushMode();
    } else {
      for (uint32_t i = 0; i < count; ++i)
        Store<uint32_t>(base, dst + 4*i, UnpackHalf(Load<uint16_t>(base, src + 2*i)));
      ctx.fpscr.disableFlushMode();
    }
    return true;
  }
  if (op == MgrNativeMathOp::Transpose) {
    const uint32_t sp = ctx.r1.u32;
    if ((dst & 3) || sp < 64 || !SafeRange(src, 64) || !SafeRange(dst, 64) ||
        Intersects(src, 64, sp - 64, sp) || Intersects(dst, 64, sp - 64, sp)) return false;
    const auto m = LoadMatrix(base, src); // Snapshot before writing: supports in-place/partial alias.
    for (unsigned r = 0; r < 4; ++r)
      for (unsigned c = 0; c < 4; ++c)
        Store<uint32_t>(base, dst + 16*r + 4*c, m[c][r]);
    return true;
  }
  const uint32_t matrix = ctx.r5.u32;
  if ((dst & 3) || !SafeRange(src, 12) || !SafeRange(matrix, 48) || !SafeRange(dst, 12))
    return false;
  // D3DXVec3TransformNormal: row-vector times the upper three matrix rows.
  // This order and separate rounding reproduce the generated vmul/vmadd path.
  ctx.fpscr.enableFlushMode();
  const float x = std::bit_cast<float>(Load<uint32_t>(base, src));
  const float y = std::bit_cast<float>(Load<uint32_t>(base, src + 4));
  const float z = std::bit_cast<float>(Load<uint32_t>(base, src + 8));
#if defined(__aarch64__)
  // Native four-lane row algebra retains ARM SIMD's NaN propagation as well as
  // the multiply/add rounding. No VMX register-bank loads or byte permutations.
  auto row = [&](unsigned r) {
    return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + matrix + 16*r)));
  };
  const float32x4_t result = vaddq_f32(vmulq_n_f32(row(0), x),
      vaddq_f32(vmulq_n_f32(row(1), y), vmulq_n_f32(row(2), z)));
  uint32_t words[4];
  vst1q_u32(words, vreinterpretq_u32_f32(result));
  for (unsigned c = 0; c < 3; ++c) Store<uint32_t>(base, dst + 4*c, words[c]);
#else
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
  std::array<float, 3> result;
  for (unsigned c = 0; c < 3; ++c) {
    const float a = std::bit_cast<float>(Load<uint32_t>(base, matrix + 4*c));
    const float b = std::bit_cast<float>(Load<uint32_t>(base, matrix + 16 + 4*c));
    const float d = std::bit_cast<float>(Load<uint32_t>(base, matrix + 32 + 4*c));
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(d)) return false;
    result[c] = x*a + (y*b + z*d);
    // Scalar and SIMD NaN selection differ on ARM; keep exceptional arithmetic
    // on the original path, including finite inputs that overflow.
    if (!std::isfinite(result[c])) return false;
  }
  for (unsigned c = 0; c < 3; ++c)
    Store<uint32_t>(base, dst + 4*c, std::bit_cast<uint32_t>(result[c]));
#endif
  return true;
}

void MgrDispatchNativeMath(MgrNativeMathOp op, PPCContext& ctx, uint8_t* base,
                          PPCFunc* original) {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_NATIVE_MATH");
    return !value || value[0] != '0';
  }();
  static const bool verify = [] {
    const char* value = std::getenv("MGR_NATIVE_MATH_VERIFY");
    return value && value[0] == '1';
  }();
  if (!enabled) { original(ctx, base); return; }
  if (verify && (op == MgrNativeMathOp::TransformNormal || op == MgrNativeMathOp::Transpose)) {
    // Original executes once; capture expected output, restore it, then run native.
    // Inputs may alias output, so restore the entire output before the native run.
    const uint32_t dst = ctx.r3.u32;
    const size_t bytes = op == MgrNativeMathOp::Transpose ? 64 : 12;
    if ((dst & 3) || !SafeRange(dst, bytes)) { original(ctx, base); return; }
    std::array<uint8_t, 64> before, expected;
    std::memcpy(before.data(), base + dst, bytes);
    PPCContext oracle = ctx;
    original(oracle, base);
    std::memcpy(expected.data(), base + dst, bytes);
    std::memcpy(base + dst, before.data(), bytes);
    if (!MgrRunNativeMath(op, ctx, base)) { original(ctx, base); return; }
    if (std::memcmp(expected.data(), base + dst, bytes) != 0) {
      std::fprintf(stderr, "[mgr-native] math mismatch op=%u dst=%08x\n", unsigned(op), dst);
      std::abort();
    }
    live_checks[op == MgrNativeMathOp::Transpose ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!MgrRunNativeMath(op, ctx, base)) original(ctx, base);
}
extern "C" REX_FUNC(sub_826CBFB8) {
  MgrDispatchNativeMath(MgrNativeMathOp::PackHalf, ctx, base, __imp__sub_826CBFB8);
}
extern "C" REX_FUNC(sub_826CC010) {
  MgrDispatchNativeMath(MgrNativeMathOp::UnpackHalf, ctx, base, __imp__sub_826CC010);
}
extern "C" REX_FUNC(sub_826CC160) {
  MgrDispatchNativeMath(MgrNativeMathOp::Transpose, ctx, base, __imp__sub_826CC160);
}

void MgrNativeMathOnPresent() {
  static unsigned frames = 0;
  const auto normal = live_checks[0].load(std::memory_order_relaxed);
  const auto transpose = live_checks[1].load(std::memory_order_relaxed);
  if (++frames % 120 == 0 && (normal || transpose))
    std::fprintf(stderr, "[mgr-native-math] live output comparisons: transform=%llu transpose=%llu\n",
        static_cast<unsigned long long>(normal), static_cast<unsigned long long>(transpose));
}
