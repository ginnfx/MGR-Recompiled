// Auto-updates for the packaged app, through Sparkle. Compiled in only when
// CMake gets MGR_SPARKLE_DIR (scripts/package-macos.sh); otherwise a no-op.
#pragma once

#if MGR_UPDATER
// Call on the main thread once a window is open; later calls do nothing. Does
// nothing unless Info.plist names a feed (SUFeedURL) and a key (SUPublicEDKey).
void MgrStartUpdater();
#else
inline void MgrStartUpdater() {}
#endif
