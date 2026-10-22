// MGR_TEST_HIDDEN=1 (diagnostic): keep automated runs off the active desktop.
// The window is hidden and an NSProcessInfo activity opts the process out of
// App Nap, which would otherwise throttle a process without visible windows.

#include "mgr_test_script.h"

#include <SDL3/SDL.h>

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// NSActivityUserInitiated | NSActivityLatencyCritical (NSProcessInfo.h).
constexpr uint64_t kNoAppNapActivity = 0x00FFFFFFULL | 0xFF00000000ULL;

void BeginNoAppNapActivity() {
  using GetInstance = id (*)(Class, SEL);
  using BeginActivity = id (*)(id, SEL, uint64_t, CFStringRef);
  Class process_info_class = objc_getClass("NSProcessInfo");
  if (!process_info_class) return;
  id process_info = reinterpret_cast<GetInstance>(objc_msgSend)(
      process_info_class, sel_registerName("processInfo"));
  // The returned activity token is retained for the life of the process.
  id activity = reinterpret_cast<BeginActivity>(objc_msgSend)(
      process_info, sel_registerName("beginActivityWithOptions:reason:"), kNoAppNapActivity,
      CFSTR("MGR automated test run"));
  if (activity) CFRetain(static_cast<CFTypeRef>(activity));
}

}  // namespace

void MgrTestHideWindowIfRequested() {
  const char* hidden = std::getenv("MGR_TEST_HIDDEN");
  if (!hidden || hidden[0] != '1') return;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i) SDL_HideWindow(windows[i]);
  SDL_free(windows);
  BeginNoAppNapActivity();
  std::fprintf(stderr, "[mgr-test] hid %d window(s), App Nap disabled\n", count);
}
