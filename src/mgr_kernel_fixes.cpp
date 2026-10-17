// Workarounds for SDK runtime bugs hit by MGR, applied by overriding kernel
// imports in the executable (calls from recompiled code bind here first) and
// forwarding to the SDK implementation via dlsym(RTLD_NEXT).

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// MSVC "set thread name" exception (THREADNAME_INFO). The SDK handles it in
// HandleSetThreadName -> PosixThread::set_name, which races when one guest
// thread names another (std::string name_ written from two threads ->
// "pointer being freed was not allocated", seen 2026-10-17 at boot).
// Thread names are cosmetic, so drop the exception instead.
constexpr uint32_t kSetThreadNameExceptionCode = 0x406D1388;

PPCFunc* ResolveSdk(const char* name) {
  auto* fn = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, name));
  if (!fn) {
    std::fprintf(stderr, "[mgr-kernel] FATAL: SDK %s not found: %s\n", name, dlerror());
    std::abort();
  }
  return fn;
}

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

extern "C" REX_FUNC(__imp__RtlRaiseException) {
  static PPCFunc* const sdk_raise_exception = ResolveSdk("__imp__RtlRaiseException");
  const uint32_t exception_record = ctx.r3.u32;
  if (exception_record && LoadGuestU32(base, exception_record) == kSetThreadNameExceptionCode) {
    return;
  }
  sdk_raise_exception(ctx, base);
}
