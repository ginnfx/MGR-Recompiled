#include "mgr_setup_wizard.h"

#include <SDL3/SDL.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mgr_disc_source.h"
#include "mgr_game_install.h"
#include "mgr_setup_ui.h"

namespace {

namespace fs = std::filesystem;
namespace ui = mgr_setup_ui;
using mgr_install::DlcCheck;
using mgr_install::XexCheck;

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr uint64_t kSpaceMargin = 256ull << 20;  // logs, cache and the install marker

std::string GiB(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f GiB", double(bytes) / kGiB);
  return text;
}

// Paths under the home folder read as ~/..., which is shorter and doesn't
// put the account name on screen.
std::string Tilde(const fs::path& path) {
  const char* home = std::getenv("HOME");
  const std::string text = path.string();
  if (home && *home && text.rfind(home, 0) == 0) return "~" + text.substr(std::strlen(home));
  return text;
}

std::string Shorten(const std::string& text, size_t max) {
  return text.size() <= max ? text : text.substr(0, max - 1) + "…";
}

// Controller buttons, latched by an SDL event watch and consumed per frame.
enum PadBits : uint32_t { kPadUp = 1, kPadDown = 2, kPadA = 4, kPadB = 8, kPadAny = 16 };

class SetupWizard final : public rex::ui::ImGuiDialog {
 public:
  SetupWizard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, fs::path data_root,
              MgrSetupReason reason, std::function<void(bool)> done)
      : ImGuiDialog(drawer), immediate_(immediate), data_root_(std::move(data_root)),
        reason_(reason), done_(std::move(done)),
        screen_(Screen::kIntro) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_AddEventWatch(WatchPad, this);
  }

  ~SetupWizard() override {
    SDL_RemoveEventWatch(WatchPad, this);
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    for (auto* pad : pads_) SDL_CloseGamepad(pad);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Same pages as Unleashed Recompiled's installer.
  enum class Screen { kIntro, kGame, kDlc, kSpace, kInstall, kFailed, kDone };

  struct Dlc {
    fs::path path;
    mgr_install::DlcPackage info;
  };

  static bool SDLCALL WatchPad(void* self, SDL_Event* event);
  void Go(Screen screen);
  void Finish(bool installed);
  void Pick(bool folder);
  void PickDlc();
  void StartInstall();
  void Content(ui::Panel& panel);
  uint64_t RequiredBytes();
  std::string DrawScreen(ImDrawList* dl, ImVec2 size, uint32_t pad, bool confirm);
  void Act(const std::string& id);
  void Back();

  rex::ui::ImmediateDrawer* immediate_;
  fs::path data_root_;
  MgrSetupReason reason_;
  std::function<void(bool)> done_;
  Screen screen_;
  int focus_ = 0;
  bool pad_mode_ = false;
  bool finished_ = false;
  ui::Backdrop backdrop_;
  ui::Panel panel_;

  std::atomic<uint32_t> pad_edges_{0};
  std::vector<SDL_Gamepad*> pads_;

  // The chosen copy and its check.
  std::unique_ptr<mgr_install::GameSource> source_;
  std::string copy_name_;
  XexCheck xex_ = XexCheck::kNotXex;
  bool picked_ = false;

  std::mutex dlc_mutex_;
  std::vector<Dlc> dlc_;
  std::atomic<int> dlc_checking_{0};
  std::string dlc_notice_;

  // Install worker.
  std::thread worker_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> confirm_cancel_{false};
  std::atomic<uint64_t> done_bytes_{0};
  std::atomic<bool> worker_done_{false};
  mgr_install::InstallResult result_;
  uint64_t total_bytes_ = 0;
};

bool SDLCALL SetupWizard::WatchPad(void* self, SDL_Event* event) {
  auto* wizard = static_cast<SetupWizard*>(self);
  if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
    if (SDL_Gamepad* pad = SDL_OpenGamepad(event->gdevice.which)) wizard->pads_.push_back(pad);
  } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    uint32_t bit = kPadAny;
    switch (event->gbutton.button) {
      case SDL_GAMEPAD_BUTTON_DPAD_UP: bit |= kPadUp; break;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: bit |= kPadDown; break;
      case SDL_GAMEPAD_BUTTON_SOUTH: bit |= kPadA; break;
      case SDL_GAMEPAD_BUTTON_EAST: bit |= kPadB; break;
      default: break;
    }
    wizard->pad_edges_.fetch_or(bit);
  }
  return true;
}

void SetupWizard::Go(Screen screen) {
  screen_ = screen;
  focus_ = 0;
}

void SetupWizard::Finish(bool installed) {
  if (finished_) return;
  finished_ = true;
  REXLOG_INFO("Setup: {}", installed ? "game files ready" : "closed without installing");
  auto done = std::move(done_);
  Close();
  done(installed);
}

void SetupWizard::Pick(bool folder) {
  const auto chosen = MgrPickGameCopy(folder);
  if (!chosen) return;
  picked_ = true;
  copy_name_ = chosen->filename().string();
  if (folder) {
    std::error_code ec;
    source_ = fs::is_regular_file(*chosen / "default.xex", ec)
                  ? std::make_unique<mgr_install::FolderSource>(*chosen)
                  : nullptr;
  } else {
    source_ = mgr_install::OpenDiscImage(*chosen);
  }
  xex_ = source_ ? mgr_install::CheckSourceXex(*source_) : XexCheck::kNotXex;
  REXLOG_INFO("Setup: {} -> {}", chosen->string(), mgr_install::Describe(xex_));
  Go(Screen::kGame);  // focus lands on Next when the copy is good
}

void SetupWizard::PickDlc() {
  auto picked = MgrPickDlcPackages();
  if (picked.empty()) return;
  dlc_notice_.clear();
  if (worker_.joinable()) worker_.join();
  dlc_checking_ = int(picked.size());
  worker_ = std::thread([this, picked = std::move(picked)] {
    for (const auto& path : picked) {
      auto info = mgr_install::CheckDlcPackage(path);
      REXLOG_INFO("Setup: DLC {} ({}) -> {}", path.filename().string(), info.name,
                  mgr_install::Describe(info.check));
      std::lock_guard lock(dlc_mutex_);
      if (info.check == DlcCheck::kOk) {
        const bool known = std::any_of(dlc_.begin(), dlc_.end(),
                                       [&](const Dlc& d) { return d.path == path; });
        if (!known) dlc_.push_back({path, info});
      } else {
        dlc_notice_ = (info.name.empty() ? path.filename().string() : info.name) + ": " +
                      mgr_install::Describe(info.check);
      }
      --dlc_checking_;
    }
  });
}

uint64_t SetupWizard::RequiredBytes() {
  uint64_t total = mgr_install::TotalBytes(mgr_install::Manifest());
  std::lock_guard lock(dlc_mutex_);
  for (const auto& d : dlc_) total += d.info.size;
  return total;
}

void SetupWizard::StartInstall() {
  if (worker_.joinable()) worker_.join();
  std::vector<Dlc> dlc;
  {
    std::lock_guard lock(dlc_mutex_);
    dlc = dlc_;
  }
  total_bytes_ = RequiredBytes();
  cancel_ = false;
  confirm_cancel_ = false;
  done_bytes_ = 0;
  worker_done_ = false;
  Go(Screen::kInstall);
  worker_ = std::thread([this, dlc = std::move(dlc)] {
    auto result = mgr_install::InstallFrom(
        *source_, data_root_, [this](uint64_t done, uint64_t) { done_bytes_ = done; }, cancel_);
    for (const auto& d : dlc) {
      if (!result.ok || cancel_) break;
      result = mgr_install::QueueDlcPackage(d.path, data_root_);
      done_bytes_ += d.info.size;
    }
    result_ = result;
    worker_done_ = true;
  });
}

void SetupWizard::Act(const std::string& id) {
  if (id == "intro-next") Go(Screen::kGame);
  else if (id == "add-files") Pick(false);
  else if (id == "add-folder") Pick(true);
  else if (id == "game-next") Go(Screen::kDlc);
  else if (id == "add-dlc") PickDlc();
  else if (id == "dlc-next") Go(Screen::kSpace);
  else if (id == "install") StartInstall();
  else if (id == "retry") Go(Screen::kGame);
  else if (id == "cancel") { confirm_cancel_ = true; focus_ = 0; }
  else if (id == "keep") { confirm_cancel_ = false; focus_ = 0; }
  else if (id == "stop") { cancel_ = true; confirm_cancel_ = false; }
  else if (id == "play") Finish(true);
  else if (id == "quit") Finish(false);
}

void SetupWizard::Back() {
  switch (screen_) {
    case Screen::kGame: Go(Screen::kIntro); break;
    case Screen::kDlc: Go(Screen::kGame); break;
    case Screen::kSpace: Go(Screen::kDlc); break;
    case Screen::kInstall:
      confirm_cancel_ = !confirm_cancel_;
      focus_ = 0;
      break;
    default: break;
  }
}

void SetupWizard::Content(ui::Panel& p) {
  switch (screen_) {
    case Screen::kIntro:
      p.Heading("Kept you waiting, huh?");
      if (reason_ == MgrSetupReason::kDamaged) {
        p.Paragraph("Your game files didn't pass the launch check. Add your copy again to repair "
                    "them.");
      } else if (reason_ == MgrSetupReason::kRequested) {
        p.Paragraph("The game is already installed.");
        p.Option("play", "Play");
        p.Option("intro-next", "Reinstall", {}, true);
        break;
      } else {
        p.Paragraph("You'll need an Xbox 360 copy of Metal Gear Rising: Revengeance in order to "
                    "proceed with the installation.");
      }
      p.Option("intro-next", "Next");
      p.Option("quit", "Quit", {}, true);
      break;
    case Screen::kGame: {
      p.Paragraph("Add the source for the game.");
      const bool ok = picked_ && xex_ == XexCheck::kOk;
      p.Checks({{"Game", picked_ ? Shorten(copy_name_, 40) : "", !picked_ ? 0 : ok ? 1 : 2}});
      if (picked_ && !ok) p.Alert(mgr_install::Describe(xex_));
      if (ok) p.Option("game-next", "Next");
      p.Option("add-files", "Add Files", ".iso", ok);
      p.Option("add-folder", "Add Folder", {}, ok);
      break;
    }
    case Screen::kDlc: {
      p.Paragraph("Add the sources for the DLC.");
      std::lock_guard lock(dlc_mutex_);
      std::vector<ui::CheckRow> rows;
      for (const auto& d : dlc_) rows.push_back({Shorten(d.info.name, 40), GiB(d.info.size), 1});
      if (dlc_checking_ > 0) rows.push_back({"Checking…", {}, 0});
      if (!rows.empty()) p.Checks(rows);
      if (!dlc_notice_.empty()) p.Alert(dlc_notice_);
      if (dlc_checking_ > 0) break;
      if (dlc_.empty()) {
        p.Option("add-dlc", "Add Files");
        p.Option("dlc-next", "Skip", {}, true);
      } else {
        p.Option("dlc-next", "Next");
        p.Option("add-dlc", "Add Files", {}, true);
      }
      break;
    }
    case Screen::kSpace: {
      p.Paragraph("The content will be installed to:");
      p.Paragraph(Tilde(data_root_), ui::kAsh, ui::GetFonts().mono);
      const uint64_t required = RequiredBytes() + kSpaceMargin;
      std::error_code ec;
      const auto space = fs::space(data_root_.parent_path(), ec);
      p.Paragraph("Required space: " + GiB(required) + "\nAvailable space: " +
                  (ec ? std::string("unknown") : GiB(space.available)));
      if (!ec && space.available < required) {
        p.Alert("There isn't enough free space.");
        break;
      }
      p.Option("install", "Next");
      break;
    }
    case Screen::kInstall: {
      p.Paragraph("Please wait while the content is being installed...");
      const uint64_t done = std::min<uint64_t>(done_bytes_, total_bytes_);
      const float fraction = total_bytes_ ? float(double(done) / double(total_bytes_)) : 0;
      char pct[16];
      std::snprintf(pct, sizeof(pct), "%d%%", int(fraction * 100));
      p.Progress(fraction, pct, {});
      if (confirm_cancel_) {
        p.Alert("Are you sure you want to cancel the installation?");
        p.Option("keep", "No");
        p.Option("stop", "Yes", {}, true);
      } else {
        p.Option("cancel", "Cancel", {}, true);
      }
      break;
    }
    case Screen::kFailed:
      p.Paragraph("Installation failed.");
      p.Alert(result_.error);
      p.Option("retry", "Retry");
      p.Option("quit", "Quit", {}, true);
      break;
    case Screen::kDone:
      p.Paragraph("Installation complete!");
      p.Option("play", "Play");
      break;
  }
}

void SetupWizard::OnDraw(ImGuiIO& io) {
  if (finished_) return;
  // Install worker finished: move on.
  if (screen_ == Screen::kInstall && worker_done_) {
    worker_.join();
    worker_done_ = false;
    if (result_.ok) Go(Screen::kDone);
    else if (result_.cancelled) Go(Screen::kSpace);
    else Go(Screen::kFailed);
  }

  // Input: keyboard and mouse through ImGui, controllers through SDL. Prompts
  // follow whichever was used last.
  const uint32_t pad = pad_edges_.exchange(0);
  const bool mouse_moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
  if (pad) pad_mode_ = true;
  else if (mouse_moved || ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
           ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Enter))
    pad_mode_ = false;
  const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                       ImGui::IsKeyPressed(ImGuiKey_Space) || (pad & kPadA);
  const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape) || (pad & kPadB);

  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##mgr-setup", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Lay out in design units, then scale every vertex and clip rect we added.
  const float scale = std::max(1.0f, io.DisplaySize.y / ui::kDesignHeight);
  const ImVec2 size{io.DisplaySize.x / scale, io.DisplaySize.y / scale};
  ui::SetScale(scale);
  const int first_vertex = dl->VtxBuffer.Size;
  const int first_command = dl->CmdBuffer.Size;
  const std::string activated = DrawScreen(dl, size, pad, confirm);
  for (int i = first_vertex; i < dl->VtxBuffer.Size; ++i) {
    dl->VtxBuffer[i].pos.x *= scale;
    dl->VtxBuffer[i].pos.y *= scale;
  }
  for (int i = first_command; i < dl->CmdBuffer.Size; ++i) {
    auto& clip = dl->CmdBuffer[i].ClipRect;
    clip = {clip.x * scale, clip.y * scale, clip.z * scale, clip.w * scale};
  }
  ImGui::End();

  if (back) Back();
  if (!activated.empty()) Act(activated);
}

std::string SetupWizard::DrawScreen(ImDrawList* dl, ImVec2 size, uint32_t pad, bool confirm) {
  backdrop_.Draw(dl, size, immediate_);
  const char* header = screen_ == Screen::kInstall ? "INSTALLING"
                       : screen_ == Screen::kDone  ? "COMPLETE"
                                                   : "INSTALLER";
  std::string activated =
      panel_.Draw(dl, size, header, [this](ui::Panel& p) { Content(p); }, focus_, confirm, true);
  const int count = panel_.option_count();
  if (count > 0) {
    if (pad & kPadDown || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) focus_ = (focus_ + 1) % count;
    if (pad & kPadUp || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) focus_ = (focus_ + count - 1) % count;
  }
  const bool with_back = screen_ != Screen::kIntro && screen_ != Screen::kDone &&
                         screen_ != Screen::kFailed;
  ui::Prompts(dl, panel_.FooterAnchor(), pad_mode_, with_back);
  return activated;
}

}  // namespace

void MgrShowSetupWizard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                        const fs::path& data_root, MgrSetupReason reason,
                        std::function<void(bool)> done) {
  // ImGuiDialog registers itself with the drawer and deletes itself on Close().
  new SetupWizard(drawer, immediate, data_root, reason, std::move(done));
}
