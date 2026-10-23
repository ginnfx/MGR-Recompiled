/**
 * @file    core/shutdown.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR: the renderer only asks for shutdown on a fatal device
 * error. MGR's app owns the normal quit sequence, so a request logs and
 * terminates instead of running re:Blue's staged teardown.
 */
#include "core/shutdown.h"

#include <atomic>
#include <cstdlib>

#include "core/logging.h"

namespace mgrn {

namespace {
std::atomic<bool> g_shutting_down{false};
} // namespace

void SetShutdownDispatcher(std::function<bool(std::function<void()>)>) {}
void SetShutdownUIPump(std::function<void()>) {}

void RequestShutdown(ShutdownReason reason, int exit_code) {
  if (g_shutting_down.exchange(true))
    return;
  MGRN_CRITICAL("Native renderer requested shutdown (reason {}, exit code {})",
                static_cast<int>(reason), exit_code);
  std::_Exit(exit_code);
}

bool IsShuttingDown() { return g_shutting_down.load(std::memory_order_acquire); }

void QuiesceForExit() {}

} // namespace mgrn
