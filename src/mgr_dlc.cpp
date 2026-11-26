#include "mgr_dlc.h"

#include "mgr_game_install.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include <system_error>

using rex::X_RESULT;  // X_ERROR_SUCCESS expands to it unqualified

namespace {
// Renames a package that fails its block hashes, so later launches skip it.
void SetAsideDamaged(const std::filesystem::path& package) {
  const auto aside = mgr_install::DamagedPackagePath(package);
  std::error_code ec;
  std::filesystem::rename(package, aside, ec);
  if (ec) {
    REXLOG_ERROR("DLC: {} is damaged (block hashes fail) and could not be renamed ({}); "
                 "remove it and drop in a fresh copy", package.filename().string(), ec.message());
    return;
  }
  REXLOG_ERROR("DLC: {} is damaged (block hashes fail); moved aside as {}. Get a fresh copy "
               "and drop it in again", package.filename().string(), aside.filename().string());
}
}  // namespace

void MgrInstallPendingDlc(const std::filesystem::path& pending_dir) {
  std::error_code ec;
  if (pending_dir.empty() || !std::filesystem::is_directory(pending_dir, ec)) return;
  auto* content = rex::system::kernel_state()->content_manager();
  for (const auto& item : std::filesystem::directory_iterator(pending_dir, ec)) {
    const auto& package = item.path();
    if (!item.is_regular_file(ec) || !mgr_install::IsPendingPackageFile(package)) continue;
    const rex::X_RESULT result = content->InstallContent(package);
    if (result != X_ERROR_SUCCESS) {
      if (mgr_install::CheckDlcPackage(package).check == mgr_install::DlcCheck::kDamaged) {
        SetAsideDamaged(package);
        continue;
      }
      REXLOG_ERROR("DLC: could not unpack {} ({:#x}); retrying next launch", package.string(),
                   result);
      continue;
    }
    REXLOG_INFO("DLC: unpacked {}", package.filename().string());
    std::filesystem::remove(package, ec);
  }
}
