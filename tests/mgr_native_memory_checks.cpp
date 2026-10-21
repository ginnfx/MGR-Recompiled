#include "mgr_native_memory.h"
#include <rex/ppc/context.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
REX_EXTERN(__imp__sub_822F67E0);
REX_EXTERN(sub_822F67E0);
namespace {
constexpr size_t kSize = 65536, kSp = 60000;
struct alignas(32) Memory { std::array<uint8_t,kSize> bytes; };
void Check(const Memory& initial, uint32_t dst, uint32_t src, uint32_t count,
           bool expect_native) {
  Memory a=initial, b=initial;
  PPCContext ca{}, cb{};
  ca.fpscr.InitHost(); ca.r1.u64=kSp; ca.lr=0x12345678;
  ca.r3.u64=0x123400000000ull | dst; ca.r4.u64=src; ca.r5.u64=count;
  ca.r14.u64=0xaabbccdd12345678; ca.r31.u64=0x1122334455667788;
  cb=ca;
  __imp__sub_822F67E0(ca,a.bytes.data());
  const bool accepted=MgrRunNativeCopy(cb,b.bytes.data());
  if (accepted != expect_native) { std::fprintf(stderr,"unexpected acceptance\n"); std::abort(); }
  if (!accepted) {
    if (b.bytes != initial.bytes) std::abort(); // Fallback must have no memory side effects.
    __imp__sub_822F67E0(cb,b.bytes.data());
  }
  // The original saves its return pointer in the ABI scratch slot.
  std::memcpy(b.bytes.data()+kSp-8,a.bytes.data()+kSp-8,8);
  if (a.bytes != b.bytes || ca.r3.u64 != cb.r3.u64 || ca.r1.u64 != cb.r1.u64 ||
      ca.lr != cb.lr || ca.r14.u64 != cb.r14.u64 || ca.r31.u64 != cb.r31.u64 ||
      ca.fpscr.csr != cb.fpscr.csr) {
    std::fprintf(stderr,"FAIL copy dst=%x src=%x n=%u\n",dst,src,count); std::abort();
  }
}
}
int main(int argc,char**) {
  std::mt19937 random(0x822f67e0);
  Memory memory;
  for (auto& b : memory.bytes) b=uint8_t(random());
  constexpr uint32_t lengths[]={0,1,2,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,
    127,128,129,255,256,257,511,512,513,1023,1024,1025,4095,4096,4097,8191,8192,8193};
  unsigned checked=0;
  for (uint32_t dst_align=0;dst_align<32;++dst_align)
    for (uint32_t src_align=0;src_align<32;++src_align)
      for (uint32_t n:lengths) {
        Check(memory,32768+dst_align,1024+src_align,n,true); ++checked;
      }
  for (uint32_t n:{1u,3u,8u,31u,128u,1024u}) {
    Check(memory,4096,4096,n,false);
    Check(memory,4097,4096,n,n==1);
    Check(memory,4096,4097,n,n==1);
    Check(memory,kSp-4,1024,n,false);
    Check(memory,1024,kSp-4,n,false);
  }
  for (uint32_t address:{0xa0000000u,0xe0000000u,0x7f000000u,0xfffffff0u,0x9fffffffu}) {
    PPCContext ctx{}; ctx.r1.u32=kSp; ctx.r3.u32=32768; ctx.r4.u32=address; ctx.r5.u32=128;
    if (MgrRunNativeCopy(ctx,memory.bytes.data())) std::abort();
    ctx.r3.u32=address; ctx.r4.u32=1024;
    if (MgrRunNativeCopy(ctx,memory.bytes.data())) std::abort();
  }
  // The strong executable hook itself must return the full guest return pointer.
  PPCContext hook{}; hook.r1.u32=kSp; hook.r3.u64=0x123400008000ull;
  hook.r4.u32=1024; hook.r5.u32=4096;
  sub_822F67E0(hook,memory.bytes.data());
  if (hook.r3.u64 != 0x123400008000ull || std::memcmp(memory.bytes.data()+32768,memory.bytes.data()+1024,4096))
    std::abort();
  std::printf("PASS: %u disjoint copy cases; overlap, stack, MMIO, physical/wrap fallbacks; strong hook and ABI\n",checked);
  if (argc<2) return 0;
  for (unsigned alignment:{0u,3u}) for (uint32_t n:{16u,64u,256u,4096u,16384u}) {
    for (bool native:{false,true}) {
      PPCContext ctx{}; ctx.r1.u32=kSp;
      constexpr unsigned iterations=10000;
      const auto start=std::chrono::steady_clock::now();
      for (unsigned i=0;i<iterations;++i) {
        ctx.r3.u64=32768+alignment; ctx.r4.u64=1024+alignment; ctx.r5.u64=n;
        if (native) sub_822F67E0(ctx,memory.bytes.data());
        else __imp__sub_822F67E0(ctx,memory.bytes.data());
        // Make every output observable to the optimizer in both paths.
        asm volatile("" : : "r"(memory.bytes.data()) : "memory");
      }
      const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
      std::printf("BENCH copy n=%u alignment=%u %s %.2f ns/call\n",n,alignment,native?"native":"original",ns);
    }
  }
}
