#include "mgr_native_math.h"
#include <rex/ppc/context.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
REX_EXTERN(__imp__sub_826CBFB8);
REX_EXTERN(__imp__sub_826CC010);
REX_EXTERN(__imp__sub_826CC0E0);
REX_EXTERN(__imp__sub_826CC160);

namespace {
constexpr size_t kSize = 65536, kSp = 60000;
struct alignas(32) Memory { std::array<uint8_t, kSize> bytes; };
struct Case { MgrNativeMathOp op; const char* name; PPCFunc* original; };
unsigned fallbacks[4]{};
constexpr Case cases[] = {
  {MgrNativeMathOp::PackHalf, "pack", __imp__sub_826CBFB8},
  {MgrNativeMathOp::UnpackHalf, "unpack", __imp__sub_826CC010},
  {MgrNativeMathOp::TransformNormal, "transform-normal", __imp__sub_826CC0E0},
  {MgrNativeMathOp::Transpose, "transpose", __imp__sub_826CC160},
};
void Check(const Case& test, Memory& initial, uint32_t dst, uint32_t src,
           uint32_t arg, unsigned round = 0) {
  Memory a = initial, b = initial;
  PPCContext ca{}, cb{};
  ca.fpscr.InitHost(); ca.fpscr.storeFromGuest(round);
  ca.r1.u64 = kSp; ca.r3.u64 = dst; ca.r4.u64 = src; ca.r5.u64 = arg;
  // Seed nonvolatile registers so preservation is checked independently of output.
  ca.r14.u64 = 0x123456789abcdef0; ca.r31.u64 = 0x9988776655443322;
  cb = ca;
  test.original(ca, a.bytes.data());
  const uint32_t oracle_fp = ca.fpscr.getcsr() & rex::ppc::FPSCRRegister::GuestMask;
  cb.fpscr.setcsr(cb.fpscr.csr);
  if (!MgrRunNativeMath(test.op, cb, b.bytes.data())) {
    ++fallbacks[unsigned(test.op)];
    test.original(cb, b.bytes.data());
  }
  const uint32_t native_fp = cb.fpscr.getcsr() & rex::ppc::FPSCRRegister::GuestMask;
  // Red-zone scratch is not a return value. All other memory, including guards,
  // inputs and overlapping output, must match byte for byte.
  std::copy_n(a.bytes.data() + kSp - 256, 256, b.bytes.data() + kSp - 256);
  if (a.bytes != b.bytes || native_fp != oracle_fp ||
      ca.r3.u64 != cb.r3.u64 || ca.r1.u64 != cb.r1.u64 ||
      ca.r14.u64 != cb.r14.u64 || ca.r31.u64 != cb.r31.u64 || ca.lr != cb.lr) {
    size_t at = 0;
    while (at < kSize && a.bytes[at] == b.bytes[at]) ++at;
    std::fprintf(stderr, "FAIL %s dst=%x src=%x arg=%x round=%u byte=%zx expected=%02x actual=%02x fp=%x/%x\n",
        test.name, dst, src, arg, round, at, at < kSize ? a.bytes[at] : 0,
        at < kSize ? b.bytes[at] : 0, oracle_fp, native_fp);
    std::abort();
  }
}
void Store32(Memory& m, uint32_t addr, uint32_t value) {
  value = std::byteswap(value); std::memcpy(m.bytes.data() + addr, &value, 4);
}
}
int main(int argc, char**) {
  std::mt19937 rng(0x4b4e080a);
  Memory memory;
  for (auto& b : memory.bytes) b = uint8_t(rng());
  // Exhaust every half encoding, including zeros, subnormals and exponent 31.
  for (unsigned start = 0; start < 65536; start += 4096) {
    for (unsigned i = 0; i < 4096; ++i) {
      uint16_t h = std::byteswap(uint16_t(start+i));
      std::memcpy(memory.bytes.data() + 1024 + 2*i, &h, 2);
    }
    Check(cases[1], memory, 16384, 1024, 4096);
  }
  constexpr uint32_t edges[] = {0,0x80000000,1,0x007fffff,0x00800000,0x33000000,
    0x337fffff,0x33800000,0x387fffff,0x38800000,0x477fdfff,0x477fe000,
    0x477fe001,0x47800000,0x7f800000,0xff800000,0x7f800001,0x7fc12345,0xffc12345};
  for (auto bits : edges) for (unsigned round = 0; round < 4; ++round) {
    Store32(memory, 1024, bits); Check(cases[0], memory, 16384, 1024, 1, round);
  }
  for (unsigned trial = 0; trial < 4000; ++trial) {
    for (unsigned i = 0; i < 1024; i += 4) Store32(memory, 1024+i, rng());
    const unsigned alignment = trial % 16;
    // Random packing and overlaps in both directions; empty counts and odd byte addresses.
    for (unsigned op = 0; op < 2; ++op) {
      const uint32_t src = 1024+alignment;
      const uint32_t dst = trial % 3 == 0 ? src : trial % 3 == 1 ? src+4 : 4096+alignment;
      Check(cases[op], memory, dst, src, trial % 129, trial % 4);
    }
    // All input byte alignments; matrix/vector/output aliases; finite and NaN bit patterns.
    const uint32_t src = 1024+alignment, matrix = 1152+(trial/16)%16;
    const uint32_t dst = trial%3 == 0 ? 4096 : trial%3 == 1 ? src & ~3u : matrix & ~3u;
    Check(cases[2], memory, dst, src, matrix, trial%4);
    Check(cases[3], memory, dst, src, 0);
  }
  PPCContext unsupported{}; unsupported.r1.u32 = kSp;
  unsupported.r3.u32 = 0xfffffff0; unsupported.r4.u32 = 1024; unsupported.r5.u32 = 100;
  if (MgrRunNativeMath(MgrNativeMathOp::PackHalf, unsupported, memory.bytes.data())) std::abort();
  for (unsigned i=0; i<4; ++i) std::printf("fallback op=%u count=%u\n", i, fallbacks[i]);
  for (uint32_t address : {0xa0000000u, 0xe0000000u, 0x7f000010u, 0xfffffff0u}) {
    unsupported.r3.u32 = 16384; unsupported.r4.u32 = address; unsupported.r5.u32 = 100;
    if (MgrRunNativeMath(MgrNativeMathOp::PackHalf, unsupported, memory.bytes.data())) std::abort();
  }
  std::puts("PASS: exhaustive half decode, boundary packing, 16000 randomized/alias/alignment cases, ABI and FP modes");
  if (argc < 2) return 0;
  // Compare the exact old and new guest ABI paths; no isolated arithmetic-only timing.
  for (const auto& test : cases) {
    for (unsigned i = 0; i < 8192; i += 4)
      Store32(memory, 1024+i, std::bit_cast<uint32_t>(float(i+1)*0.01f));
    for (bool native : {false, true}) {
      constexpr unsigned iterations = 20000;
      PPCContext ctx{}; ctx.fpscr.InitHost(); ctx.r1.u64 = kSp;
      const bool array = test.op == MgrNativeMathOp::PackHalf || test.op == MgrNativeMathOp::UnpackHalf;
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < iterations; ++i) {
        ctx.r3.u64 = 16384; ctx.r4.u64 = 1024; ctx.r5.u64 = array ? 1024 : 1152;
        if (native) MgrDispatchNativeMath(test.op, ctx, memory.bytes.data(), test.original);
        else test.original(ctx, memory.bytes.data());
        // Make every output observable to the optimizer in both paths.
        asm volatile("" : : "r"(memory.bytes.data()) : "memory");
      }
      const auto ns = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
      std::printf("BENCH %s %s %.2f ns/call (%u elements)\n", test.name,
                  native ? "native" : "original", ns/iterations, array ? 1024 : 1);
    }
  }
}
