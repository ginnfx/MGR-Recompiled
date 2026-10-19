#pragma once

#include <cstdint>

// Original cOptionMenu lifecycle. Guest thread publishes a snapshot; the UI
// never reads guest objects or invokes guest code from the UI thread.
struct MgrGameOptionsState {
  uint32_t session;
  bool interactive;
};
void MgrGameOptionsInstall();
void MgrGameOptionsUninstall();
MgrGameOptionsState MgrGameOptionsGetState();
void MgrGameOptionsSetPageInput(bool owned);
