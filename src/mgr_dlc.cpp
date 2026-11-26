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

void InstallPackage(rex::system::xam::ContentManager& content,
                    const std::filesystem::path& package) {
  const rex::X_RESULT result = content.InstallContent(package);
  if (result != X_ERROR_SUCCESS) {
    if (mgr_install::CheckDlcPackage(package).check == mgr_install::DlcCheck::kDamaged) {
      SetAsideDamaged(package);
      return;
    }
    REXLOG_ERROR("DLC: could not unpack {} ({:#x}); retrying next launch", package.string(),
                 result);
    return;
  }
  REXLOG_INFO("DLC: unpacked {}", package.filename().string());
  std::error_code ec;
  if (!std::filesystem::remove(package, ec) || ec)
    REXLOG_ERROR("DLC: unpacked {} but could not delete it ({}); it will be unpacked again "
                 "next launch", package.string(), ec.message());
}
}  // namespace

void MgrInstallPendingDlc(const std::filesystem::path& pending_dir) {
  std::error_code ec;
  if (pending_dir.empty() || !std::filesystem::is_directory(pending_dir, ec)) return;
  auto* kernel = rex::system::kernel_state();
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content) {
    REXLOG_ERROR("DLC: no content manager; packages stay for the next launch");
    return;
  }
  // increment(ec) instead of a range-for, which throws if the walk fails.
  std::filesystem::directory_iterator item(pending_dir, ec);
  for (const std::filesystem::directory_iterator end; !ec && item != end; item.increment(ec)) {
    std::error_code type_error;
    if (item->is_regular_file(type_error) && mgr_install::IsPendingPackageFile(item->path()))
      InstallPackage(*content, item->path());
  }
  if (ec) REXLOG_ERROR("DLC: could not read {}: {}", pending_dir.string(), ec.message());
}
