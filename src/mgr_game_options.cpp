// MGR 4B4E080A / 0.0.0.4 original OPTIONS integration.
// cOptionMenu vtable 0x8218C018: init +8, update +16. The title and pause
// paths both create this object through 0x82C48890.
#include "mgr_game_options.h"
#include "mgr_native_options.h"

#include "generated/default/mgr_pch.h"
#include <rex/input/mnk/mnk_input_driver.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_82C5CC00);
REX_EXTERN(__imp__sub_82C71448);
REX_EXTERN(__imp__sub_82C40A88);

namespace {
std::atomic<bool> installed{false};
std::atomic<bool> page_owns_input{false};
std::atomic<uint32_t> current_object{0};
std::atomic<uint32_t> session{0};
std::atomic<uint64_t> published{0};
std::atomic<bool> saved_mouse_look{true};

void Publish(uint32_t object, uint8_t phase) {
  if (installed.load(std::memory_order_acquire) &&
      current_object.load(std::memory_order_acquire) == object) {
    published.store((uint64_t(session.load()) << 1) | (phase == 1),
                    std::memory_order_release);
  }
}
}  // namespace

void MgrGameOptionsInstall() {
  current_object.store(0);
  published.store(0);
  page_owns_input.store(false);
  installed.store(true, std::memory_order_release);
}

void MgrGameOptionsUninstall() {
  installed.store(false, std::memory_order_release);
  published.store(0, std::memory_order_release);
  page_owns_input.store(false, std::memory_order_release);
  if (current_object.exchange(0)) {
    rex::input::mnk::SetMouseLookActive(saved_mouse_look.load());
    rex::cvar::SetFlagByName("mnk_menu_navigation", "false");
  }
}

MgrGameOptionsState MgrGameOptionsGetState() {
  const uint64_t state = published.load(std::memory_order_acquire);
  return {uint32_t(state >> 1), bool(state & 1)};
}

void MgrGameOptionsSetPageInput(bool owned) {
  page_owns_input.store(owned, std::memory_order_release);
}

// Resource initialization, before the original fade-in and interactive phase.
extern "C" REX_FUNC(sub_82C5CC00) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82C5CC00(ctx, base);
  if (!installed.load(std::memory_order_acquire)) return;
  if (!current_object.exchange(object, std::memory_order_acq_rel))
    saved_mouse_look.store(rex::input::mnk::IsMouseLookActive());
  session.fetch_add(1);
  page_owns_input.store(false, std::memory_order_release);
  // Fade-in still polls the input driver, releasing relative mouse capture
  // before a graphics/keyboard page blocks guest input.
  rex::input::mnk::SetMouseLookActive(false);
  rex::cvar::SetFlagByName("mnk_menu_navigation", "true");
  Publish(object, 0);
  MgrNativeOptionsBegin(ctx, base, object);
  if (std::getenv("MGR_OPTIONS_TRACE")) {
    for (uint32_t group : {0u, 28u, 296u, 564u}) {
      const uint32_t ui = REX_LOAD_U32(object + group + 24);
      if (!ui) continue;
      const uint32_t nodes = REX_LOAD_U32(ui + 120);
      const uint32_t count = REX_LOAD_U32(ui + 124);
      std::fprintf(stderr, "[mgr-guest-ui] group=%u ui=%08X nodes=%08X count=%u\n", group, ui, nodes, count);
      for (uint32_t i = 0; i < count && i < 512; ++i) {
        const uint32_t node = nodes + i * 1024;
        const uint32_t widget = REX_LOAD_U32(node + 1008);
        if (!widget) continue;
        std::fprintf(stderr, "[mgr-guest-ui] node=%u widget=%08X vtable=%08X context=%08X message=%u\n",
                     i, widget, REX_LOAD_U32(widget), REX_LOAD_U32(widget + 20), REX_LOAD_U32(widget + 168));
      }
    }
  }
  std::fprintf(stderr, "[mgr-options] original OPTIONS session %u\n", session.load());
}

extern "C" REX_FUNC(sub_82C71448) {
  const uint32_t object = ctx.r3.u32;
  const bool ours = installed.load(std::memory_order_acquire) &&
                    current_object.load(std::memory_order_acquire) == object;
  if (ours) {
    const uint8_t phase = REX_LOAD_U8(object + 920);
    Publish(object, phase);
    if (MgrNativeOptionsUpdate(ctx, base, object)) return;
    // Keep the original options state parked while a child page has focus.
    // Parent title/pause state and all frame/presentation clocks still run.
    // This also prevents an already-sampled confirm press changing a guest row.
    if (phase == 1 && page_owns_input.load(std::memory_order_acquire)) return;
  }
  static const bool trace = std::getenv("MGR_OPTIONS_TRACE") != nullptr;
  const uint8_t phase_before = REX_LOAD_U8(object + 920);
  __imp__sub_82C71448(ctx, base);
  if (trace && REX_LOAD_U8(object + 920) != phase_before)
    std::fprintf(stderr,
                 "[mgr-options-trace] phase %u -> %u row=%u flag922=%u flag923=%u b931=%u b945=%u\n",
                 phase_before, REX_LOAD_U8(object + 920), REX_LOAD_U8(object + 921),
                 REX_LOAD_U8(object + 922), REX_LOAD_U8(object + 923), REX_LOAD_U8(object + 931),
                 REX_LOAD_U8(object + 945));
  if (trace) {
    static uint32_t last_cursor = 0;
    const uint32_t cursor = REX_LOAD_U32(object + 956);
    if (cursor != last_cursor) {
      last_cursor = cursor;
      float y;
      std::memcpy(&y, &cursor, sizeof(y));
      std::fprintf(stderr, "[mgr-options-trace] cursor y=%.3f row=%u moving=%u\n", y,
                   REX_LOAD_U8(object + 921), REX_LOAD_U32(object + 964));
    }
  }
  if (ours) MgrNativeOptionsAfterUpdate(ctx, base, object);
  if (ours) Publish(object, REX_LOAD_U8(object + 920));
}

extern "C" REX_FUNC(sub_82C40A88) {
  const uint32_t object = ctx.r3.u32;
  uint32_t expected = object;
  if (current_object.compare_exchange_strong(expected, 0)) {
    MgrNativeOptionsEnd(base, object);
    rex::cvar::SetFlagByName("mnk_menu_navigation", "false");
    published.store(0, std::memory_order_release);
    page_owns_input.store(false, std::memory_order_release);
    rex::input::mnk::SetMouseLookActive(saved_mouse_look.load());
  }
  __imp__sub_82C40A88(ctx, base);
}
