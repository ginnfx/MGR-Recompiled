#include "mgr_dlc.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include <system_error>

using rex::X_RESULT;  // X_ERROR_SUCCESS expands to it unqualified

void MgrInstallPendingDlc(const std::filesystem::path& pending_dir) {
  std::error_code ec;
  if (pending_dir.empty() || !std::filesystem::is_directory(pending_dir, ec)) return;
  auto* content = rex::system::kernel_state()->content_manager();
  for (const auto& item : std::filesystem::directory_iterator(pending_dir, ec)) {
    const auto& package = item.path();
    if (!item.is_regular_file(ec) || package.extension() == ".partial") continue;
    const rex::X_RESULT result = content->InstallContent(package);
    if (result != X_ERROR_SUCCESS) {
      REXLOG_ERROR("DLC: could not unpack {} ({:#x}); retrying next launch", package.string(),
                   result);
      continue;
    }
    REXLOG_INFO("DLC: unpacked {}", package.filename().string());
    std::filesystem::remove(package, ec);
  }
}
