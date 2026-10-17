// Workarounds for SDK runtime bugs hit by MGR, applied by overriding kernel
// imports in the executable (calls from recompiled code bind here first) and
// forwarding to the SDK implementation via dlsym(RTLD_NEXT).

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
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

namespace {

// NtWaitForMultipleObjectsEx busy-spin workaround.
//
// SDK bug (threading_posix.cpp, PosixConditionBase::WaitMultiple): the poll
// sleep is duration_cast<milliseconds>(remaining), which truncates <1 ms to 0,
// and alertable waits always run in 1 ms slices -> every alertable
// multi-object wait spins a core at 100% (seen: 2 MGR threads incl. Wwise IO).
// Re-issue long alertable waits as: alertable zero-timeout poll (delivers APCs
// and user callbacks with the SDK's exact status codes), then a non-alertable
// wait in kWaitWindow slices, where the SDK loop sleeps 1 ms per iteration.
// Opt-in (MGR_WAIT_FIX=1): one user run hung on the title -> gameplay load
// with it enabled (2026-10-17), so it stays off until proven safe.
constexpr int64_t kHundredNsPerMs = 10000;
constexpr int64_t kWaitWindowMs = 16;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kScratchBelowStack = 0x400;  // guest stack scratch for the timeout value

int64_t LoadGuestI64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return int64_t(__builtin_bswap64(value));
}

void StoreGuestI64(uint8_t* base, uint32_t address, int64_t value) {
  uint64_t swapped = __builtin_bswap64(uint64_t(value));
  std::memcpy(base + address, &swapped, sizeof(swapped));
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

extern "C" REX_FUNC(__imp__NtWaitForMultipleObjectsEx) {
  static PPCFunc* const sdk_wait = ResolveSdk("__imp__NtWaitForMultipleObjectsEx");
  static const bool is_enabled = [] {
    const char* value = std::getenv("MGR_WAIT_FIX");
    return value && value[0] == '1';
  }();
  if (!is_enabled) {
    sdk_wait(ctx, base);
    return;
  }
  // r3 count, r4 handles, r5 wait type, r6 wait mode, r7 alertable, r8 timeout*
  const uint32_t alertable = ctx.r7.u32;
  const uint32_t timeout_ptr = ctx.r8.u32;
  int64_t timeout_100ns = timeout_ptr ? LoadGuestI64(base, timeout_ptr) : INT64_MIN;
  const bool infinite = !timeout_ptr;
  // Only long relative or infinite alertable waits spin; forward the rest.
  if (!alertable || (!infinite && (timeout_100ns >= 0 || -timeout_100ns < kWaitWindowMs *
                                                                             kHundredNsPerMs))) {
    sdk_wait(ctx, base);
    return;
  }
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  const int64_t deadline_ms = infinite ? INT64_MAX : NowMs() + (-timeout_100ns / kHundredNsPerMs);
  const uint32_t scratch = (ctx.r1.u32 - kScratchBelowStack) & ~7u;
  while (true) {
    // 1) Alertable poll: APC / user callback delivery or already-signaled.
    ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
    ctx.r7.u64 = 1;
    StoreGuestI64(base, scratch, 0);
    ctx.r8.u64 = scratch;
    sdk_wait(ctx, base);
    if (ctx.r3.u32 != kStatusTimeout) return;
    // 2) Non-alertable window (no spin inside the SDK loop).
    int64_t remaining_ms = deadline_ms == INT64_MAX ? kWaitWindowMs : deadline_ms - NowMs();
    if (remaining_ms <= 0) {
      ctx.r3.u64 = kStatusTimeout;
      return;
    }
    ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
    ctx.r7.u64 = 0;
    StoreGuestI64(base, scratch, -std::min(remaining_ms, kWaitWindowMs) * kHundredNsPerMs);
    ctx.r8.u64 = scratch;
    sdk_wait(ctx, base);
    if (ctx.r3.u32 != kStatusTimeout) return;
  }
}

extern "C" REX_FUNC(__imp__RtlRaiseException) {
  static PPCFunc* const sdk_raise_exception = ResolveSdk("__imp__RtlRaiseException");
  const uint32_t exception_record = ctx.r3.u32;
  if (exception_record && LoadGuestU32(base, exception_record) == kSetThreadNameExceptionCode) {
    return;
  }
  sdk_raise_exception(ctx, base);
}
