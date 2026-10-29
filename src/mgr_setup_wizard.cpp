#include "mgr_setup_wizard.h"

#include <SDL3/SDL.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

constexpr float kCheckRowSeconds = 0.26f;
const std::vector<std::string> kSteps = {"Your copy", "Check", "DLC", "Install", "Play"};

std::string Gb(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f GB", double(bytes) / 1e9);
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

// Row of the copy check where each verdict stops.
int FailedRow(XexCheck check) {
  switch (check) {
    case XexCheck::kNotXex: return 0;
    case XexCheck::kOtherGame: return 1;
    case XexCheck::kOtherVersion: return 2;
    case XexCheck::kModified: return 4;
    case XexCheck::kOk: return -1;
  }
  return 0;
}

// Controller buttons, latched by an SDL event watch and consumed per frame.
enum PadBits : uint32_t { kPadUp = 1, kPadDown = 2, kPadA = 4, kPadB = 8, kPadAny = 16 };

class SetupWizard final : public rex::ui::ImGuiDialog {
 public:
  SetupWizard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, fs::path data_root,
              MgrSetupReason reason, std::function<void(bool)> done)
      : ImGuiDialog(drawer), immediate_(immediate), data_root_(std::move(data_root)),
        reason_(reason), done_(std::move(done)),
        screen_(reason == MgrSetupReason::kFirstRun ? Screen::kTitle : Screen::kWelcome) {
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
  enum class Screen { kTitle, kWelcome, kCheck, kDlc, kInstall, kFailed, kDone };

  struct Dlc {
    fs::path path;
    mgr_install::DlcPackage info;
  };

  static bool SDLCALL WatchPad(void* self, SDL_Event* event);
  void Go(Screen screen, std::string notice = {});
  void Finish(bool installed);
  void Pick(bool folder);
  void PickDlc();
  void StartInstall();
  void Content(ui::Panel& panel);
  void DrawTitle(ImDrawList* dl, ImVec2 size);
  std::string DrawScreen(ImDrawList* dl, ImVec2 size, uint32_t pad, bool confirm);
  void Act(const std::string& id);
  void Back();
  float Since() const;

  rex::ui::ImmediateDrawer* immediate_;
  fs::path data_root_;
  MgrSetupReason reason_;
  std::function<void(bool)> done_;
  Screen screen_;
  std::string notice_;
  int focus_ = 0;
  bool pad_mode_ = false;
  bool finished_ = false;
  ui::Backdrop backdrop_;
  ui::Panel panel_;
  std::chrono::steady_clock::time_point screen_start_ = std::chrono::steady_clock::now();

  std::atomic<uint32_t> pad_edges_{0};
  std::vector<SDL_Gamepad*> pads_;

  // The chosen copy and its check.
  std::unique_ptr<mgr_install::GameSource> source_;
  std::string copy_name_;
  XexCheck xex_ = XexCheck::kNotXex;
  bool copy_is_folder_ = false;

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
  std::chrono::steady_clock::time_point install_start_;
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

float SetupWizard::Since() const {
  return std::chrono::duration<float>(std::chrono::steady_clock::now() - screen_start_).count();
}

void SetupWizard::Go(Screen screen, std::string notice) {
  screen_ = screen;
  notice_ = std::move(notice);
  focus_ = 0;
  screen_start_ = std::chrono::steady_clock::now();
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
  copy_is_folder_ = folder;
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
  Go(Screen::kCheck);
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

void SetupWizard::StartInstall() {
  if (worker_.joinable()) worker_.join();
  std::vector<Dlc> dlc;
  {
    std::lock_guard lock(dlc_mutex_);
    dlc = dlc_;
  }
  total_bytes_ = mgr_install::TotalBytes(mgr_install::Manifest());
  for (const auto& d : dlc) total_bytes_ += d.info.size;
  cancel_ = false;
  confirm_cancel_ = false;
  done_bytes_ = 0;
  worker_done_ = false;
  install_start_ = std::chrono::steady_clock::now();
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
  if (id == "start") Go(Screen::kWelcome);
  else if (id == "pick-disc") Pick(false);
  else if (id == "pick-folder") Pick(true);
  else if (id == "back") Go(Screen::kWelcome);
  else if (id == "next") Go(Screen::kDlc);
  else if (id == "pick-dlc") PickDlc();
  else if (id == "install") StartInstall();
  else if (id == "cancel") { confirm_cancel_ = true; focus_ = 0; }
  else if (id == "keep") { confirm_cancel_ = false; focus_ = 0; }
  else if (id == "stop") { cancel_ = true; confirm_cancel_ = false; }
  else if (id == "play") Finish(true);
  else if (id == "quit") Finish(false);
}

void SetupWizard::Back() {
  switch (screen_) {
    case Screen::kCheck:
    case Screen::kDlc: Go(Screen::kWelcome); break;
    case Screen::kInstall:
      confirm_cancel_ = !confirm_cancel_;
      focus_ = 0;
      break;
    default: break;
  }
}

void SetupWizard::DrawTitle(ImDrawList* dl, ImVec2 size) {
  const auto& f = ui::GetFonts();
  const float spacing = f.title ? f.title->LegacySize * 0.32f : 20;
  const float w = ui::SpacedWidth(f.title, "MGR.R.", spacing) - spacing;
  const float y = size.y * 0.40f;
  ui::SpacedText(dl, f.title, {(size.x - w) / 2, y - 60}, ui::kRed, "MGR.R.", spacing);
  const char* sub = "METAL GEAR RISING: REVENGEANCE · RECOMPILED";
  const float sw = ui::SpacedWidth(f.mono_small, sub, 3.3f);
  ui::SpacedText(dl, f.mono_small, {(size.x - sw) / 2, y + 70}, ui::kAsh, sub, 3.3f);
  const char* press = pad_mode_ ? "PRESS ANY BUTTON" : "PRESS ENTER OR CLICK";
  const float pw = ui::SpacedWidth(f.option, press, 4.2f);
  const float pulse = 0.25f + 0.75f * (0.5f + 0.5f * std::cos(Since() * 3.9f));
  ui::SpacedText(dl, f.option, {(size.x - pw) / 2, size.y * 0.72f},
                 IM_COL32(235, 232, 227, int(255 * pulse)), press, 4.2f);
}

void SetupWizard::Content(ui::Panel& p) {
  using F = ui::Fact;
  const std::string goes_to = Tilde(data_root_ / "game");
  switch (screen_) {
    case Screen::kTitle:
      break;
    case Screen::kWelcome: {
      if (reason_ == MgrSetupReason::kDamaged) {
        p.Eyebrow("SOMETHING'S OFF");
        p.Heading("Your game files took a hit");
        p.Paragraph("They didn't pass the launch check. Pick your copy again and they'll get "
                    "patched up. Saves and settings stay put.");
      } else if (reason_ == MgrSetupReason::kRequested) {
        p.Eyebrow("OPTION HELD");
        p.Heading("Fresh install?");
        p.Paragraph("Your files look fine. Want to reinstall anyway, or just play?");
      } else {
        p.Eyebrow("FIRST LAUNCH");
        p.Heading("Kept you waiting, huh?");
        p.Paragraph("Before you start cutting, the game needs your copy. Pick your Xbox 360 disc "
                    "image or the folder you dumped it to. The files get copied and checked "
                    "once, then you're in.");
      }
      p.Facts({{"WORKS WITH", "Xbox 360 disc, USA/Europe · 4B4E080A · 6CC18291 · v4"},
               {"NEEDS", Gb(mgr_install::TotalBytes(mgr_install::Manifest()) + (256ull << 20)) + " free"},
               {"GOES TO", goes_to, true}});
      if (!notice_.empty()) p.Alert(notice_, ui::kIce);
      if (reason_ == MgrSetupReason::kRequested) p.Option("play", "Play", "Jump in");
      p.Option("pick-disc", "Pick disc image…", ".iso");
      p.Option("pick-folder", "Pick game folder…", "has default.xex");
      if (reason_ != MgrSetupReason::kRequested) p.Option("quit", "Quit", {}, true);
      break;
    }
    case Screen::kCheck: {
      p.Eyebrow("CHECKING YOUR COPY");
      p.Heading(copy_is_folder_ ? "Game folder" : "Disc image");
      p.Paragraph(Shorten(copy_name_, 70), ui::kAsh, ui::GetFonts().mono);
      const int failed = FailedRow(xex_);
      const int revealed = std::min(5, int(Since() / kCheckRowSeconds));
      std::vector<ui::CheckRow> rows = {{"Xbox 360 executable", "XEX2"},
                                        {"Game", "4B4E080A · MGR:R"},
                                        {"Disc", "media 6CC18291"},
                                        {"Version", "4 · no title update"},
                                        {"default.xex fingerprint", "XXH3 c2e4869d5db4fbc6"}};
      if (!source_ && copy_is_folder_) rows[0].value = "no default.xex";
      const int last = failed < 0 ? 4 : failed;
      for (int i = 0; i < int(rows.size()); ++i) {
        if (i >= revealed || i > last) continue;
        rows[i].state = i == failed ? 2 : 1;
        if (i == failed && i == 1) rows[i].value = "another game";
        if (i == failed && i == 2) rows[i].value = "another release";
        if (i == failed && i == 4) rows[i].value = "modified";
      }
      p.Checks(rows);
      if (revealed <= last) break;
      if (failed >= 0) {
        p.Alert(!source_ && copy_is_folder_
                    ? "No default.xex in here. Pick the folder that has default.xex and media/."
                    : mgr_install::Describe(xex_));
        p.Option("pick-disc", "Pick a different copy");
        p.Option("back", "Back", {}, true);
      } else {
        p.Paragraph("Legit copy. Nice.");
        std::error_code ec;
        const auto space = fs::space(data_root_.parent_path(), ec);
        p.Facts({{"FILES", std::to_string(mgr_install::Manifest().size()) + " · " +
                               Gb(mgr_install::TotalBytes(mgr_install::Manifest()))},
                 {"FREE", ec ? "unknown" : Gb(space.available)}});
        p.Option("next", "Next", "DLC, if you have it");
        p.Option("pick-disc", "Pick a different copy", {}, true);
      }
      break;
    }
    case Screen::kDlc: {
      p.Eyebrow("OPTIONAL");
      p.Heading("Got any DLC?");
      p.Paragraph("Jetstream, Blade Wolf and VR Missions came as separate Xbox Live downloads. "
                  "If you've got those packages, toss them in. No DLC? No problem.");
      std::lock_guard lock(dlc_mutex_);
      std::vector<ui::CheckRow> rows;
      for (const auto& d : dlc_) rows.push_back({Shorten(d.info.name, 40), Gb(d.info.size), 1});
      if (dlc_checking_ > 0) rows.push_back({"Checking package…", "SHA-1", 0});
      if (!rows.empty()) p.Checks(rows);
      if (!dlc_notice_.empty()) p.Alert(dlc_notice_);
      if (dlc_checking_ > 0) break;
      if (dlc_.empty()) {
        p.Option("pick-dlc", "Add DLC…", "Xbox Live packages");
        p.Option("install", "Skip, just install", "about 30 s", true);
      } else {
        p.Option("install", "Install", "about 30 s");
        p.Option("pick-dlc", "Add more DLC…", {}, true);
      }
      break;
    }
    case Screen::kInstall: {
      p.Eyebrow("INSTALLING");
      p.Heading("Hang tight");
      const uint64_t done = std::min<uint64_t>(done_bytes_, total_bytes_);
      const float fraction = total_bytes_ ? float(double(done) / double(total_bytes_)) : 0;
      // Which manifest file the byte count is in, for the log.
      const auto manifest = mgr_install::Manifest();
      size_t file = 0;
      for (uint64_t sum = 0; file < manifest.size() && sum + manifest[file].size <= done; ++file)
        sum += manifest[file].size;
      char pct[16], caption[48];
      std::snprintf(pct, sizeof(pct), "%d%%", int(fraction * 100));
      std::snprintf(caption, sizeof(caption), "FILE %zu OF %zu",
                    std::min(file + 1, manifest.size()), manifest.size());
      p.Progress(fraction, pct, caption);
      const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                                         install_start_).count();
      const double rate = seconds > 0.5f ? double(done) / seconds : 0;
      char speed[32] = "—", left[32] = "—";
      if (rate > 0) {
        std::snprintf(speed, sizeof(speed), "%.0f MB/s", rate / 1e6);
        std::snprintf(left, sizeof(left), "%.0f s", double(total_bytes_ - done) / rate);
      }
      p.Stats({{"COPIED", Gb(done) + " / " + Gb(total_bytes_)}, {"SPEED", speed}, {"LEFT", left}});
      std::vector<std::pair<std::string, bool>> log;
      for (size_t i = file >= 3 ? file - 3 : 0; i <= file && i < manifest.size(); ++i)
        log.emplace_back(manifest[i].path, i == file);
      p.Log(log);
      if (confirm_cancel_) {
        p.Alert("Stop now? Whatever's been copied gets thrown out. Nothing else changes.");
        p.Option("keep", "Keep going");
        p.Option("stop", "Stop", {}, true);
      } else {
        p.Option("cancel", "Cancel", "Tosses what's copied so far", true);
      }
      break;
    }
    case Screen::kFailed:
      p.Eyebrow("INSTALL STOPPED");
      p.Heading("This copy's busted");
      p.Alert(result_.error);
      p.Paragraph("Nothing got installed and the half-done copy is gone. Grab a fresh dump and "
                  "try again.");
      p.Option("pick-disc", "Pick a different copy");
      p.Option("quit", "Quit", {}, true);
      break;
    case Screen::kDone: {
      p.Eyebrow("ALL SET");
      p.Heading("Ready when you are");
      std::string text = std::to_string(mgr_install::Manifest().size()) + " files copied and checked";
      {
        std::lock_guard lock(dlc_mutex_);
        if (!dlc_.empty()) text += ", plus " + std::to_string(dlc_.size()) + " DLC (unpacked on first launch)";
      }
      p.Paragraph(text + ". You won't see this again unless something breaks or you hold Option "
                  "while opening the app.");
      p.Facts({{"SAVES", Tilde(data_root_ / "user"), true},
               {"SETTINGS", "Options → Video, in game"}});
      p.Option("play", "Play");
      break;
    }
  }
}

void SetupWizard::OnDraw(ImGuiIO& io) {
  if (finished_) return;
  // Install worker finished: move on.
  if (screen_ == Screen::kInstall && worker_done_) {
    worker_.join();
    worker_done_ = false;
    if (result_.ok) Go(Screen::kDone);
    else if (result_.cancelled) Go(Screen::kWelcome, "Install stopped. The half-done copy's been cleaned up.");
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
  if (screen_ == Screen::kTitle) {
    DrawTitle(dl, size);
    if (confirm || (pad & kPadAny) || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return "start";
    return {};
  }

  const int step = screen_ == Screen::kWelcome ? 0
                   : screen_ == Screen::kCheck ? 1
                   : screen_ == Screen::kDlc   ? 2
                   : screen_ == Screen::kDone  ? 4
                                               : 3;
  std::string activated =
      panel_.Draw(dl, size, step, kSteps, [this](ui::Panel& p) { Content(p); }, focus_, confirm, true);
  const int count = panel_.option_count();
  if (count > 0) {
    if (pad & kPadDown || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) focus_ = (focus_ + 1) % count;
    if (pad & kPadUp || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) focus_ = (focus_ + count - 1) % count;
  }
  ui::Prompts(dl, panel_.FooterAnchor(), pad_mode_, screen_ != Screen::kWelcome && screen_ != Screen::kDone);
  return activated;
}

}  // namespace

void MgrShowSetupWizard(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                        const fs::path& data_root, MgrSetupReason reason,
                        std::function<void(bool)> done) {
  // ImGuiDialog registers itself with the drawer and deletes itself on Close().
  new SetupWizard(drawer, immediate, data_root, reason, std::move(done));
}
