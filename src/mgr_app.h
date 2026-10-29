// mgr - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>

#include "mgr_bundle_paths.h"
#include "mgr_game_install.h"
#include "mgr_options_dialog.h"
#include "mgr_settings.h"
#include "mgr_setup.h"
#include "mgr_game_options.h"
#include "mgr_native_options.h"
#include "mgr_menu_fonts.h"
#include "mgr_test_script.h"
#if MGR_NATIVE_RENDERER
#include "native/mgr_native.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

void MgrStartSessionLog(const char* build);  // mgr_perf.cpp
void MgrPromptDeviceInstall();               // mgr_prompt_device.cpp
void MgrPromptDeviceUninstall();
void MgrApplySettings();                     // mgr_settings.cpp
void MgrStopSessionLog();

class MgrApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MgrApp>(new MgrApp(ctx, "mgr",
        PPCImageConfig));
  }

 protected:
  // MGR options feed the GPU/runtime cvars before the plugin loads.
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if MGR_NATIVE_RENDERER
    mgrn::MgrNativeConfigure(config);
#else
    (void)config;
#endif
    // Menu repaint ownership is session state, never a boot-time preference.
    rex::cvar::SetFlagByName("imgui_guest_paced", "false");
    if (rex::cvar::GetFlagSource("mnk_mode") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("mnk_mode", "true");
    MgrApplySettings();
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (mgr_bundle::IsAppBundle(rex::filesystem::GetExecutableFolder()))
      ApplyBundlePaths(paths);
    // Diagnostic: MGR_CONFIG_PATH=<file> replaces <exe dir>/mgr.toml, so test
    // runs never read or save the user's configuration.
    if (const char* config = std::getenv("MGR_CONFIG_PATH"); config && *config)
      paths.config_path = config;
    config_path_ = paths.config_path;
    MgrTestScriptSetSaveHandler([path = config_path_] { MgrSaveConfig(path); });
    game_root_ = paths.game_data_root;
  }

  void OnConfigureLogging(rex::LogConfig& config) override {
    if (bundle_layout_) config.log_dir = bundle_layout_->logs;
  }

  // The packaged app installs the player's game files on first launch, when
  // they fail the quick check, or when Option is held. Dev runs and an
  // explicit --game_data_root never see setup.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    if (!bundle_layout_ || defaults.game_data_root != bundle_layout_->game) return defaults;
    const bool installed = mgr_install::IsInstalled(defaults.game_data_root);
    if (installed && !MgrSetupRequested()) return defaults;
    const auto reason = installed ? MgrSetupReason::kRequested
                        : std::filesystem::exists(defaults.game_data_root)
                            ? MgrSetupReason::kDamaged
                            : MgrSetupReason::kFirstRun;
    MgrRunSetup(defaults.game_data_root.parent_path(), reason,
                [this, defaults, resume = std::move(resume)](bool installed_now) {
                  if (installed_now || mgr_install::IsInstalled(defaults.game_data_root))
                    resume(defaults);
                  else
                    app_context().QuitFromUIThread();
                });
    return std::nullopt;
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    if (std::getenv("MGR_DEVELOPER_UI")) MgrConfigureMenuFonts(atlas, game_root_);
  }

  // Native guest Options owns settings access; F2 is a developer fallback.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override { InstallGameUi(drawer); }

#if MGR_NATIVE_RENDERER
  // The native renderer has no SDK presenter, so ReXApp never builds overlays
  // or calls OnCreateDialogs; its device and the game UI come up here, after
  // the runtime exists and before the guest main thread starts.
  void OnPreLaunchModule() override {
    if (!mgrn::NativeRendererActive()) return;
    if (!mgrn::MgrNativeStart(window())) {
      app_context().QuitFromUIThread();
      return;
    }
    InstallGameUi(nullptr);
  }
#endif

 private:
  // The .app bundle is read-only: defaults move to Application Support, while
  // an explicit --flag or REX_* value still wins (#383: mgr.toml loads later).
  void ApplyBundlePaths(rex::PathConfig& paths) {
    const auto root = mgr_bundle::DataRoot(mgr_bundle::HomeDir());
    bundle_layout_ = mgr_bundle::Layout(root);
    const auto is_default = [](const char* name) {
      return rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault;
    };
    if (is_default("game_data_root")) paths.game_data_root = bundle_layout_->game;
    if (is_default("user_data_root")) paths.user_data_root = bundle_layout_->user;
    if (is_default("cache_root")) paths.cache_root = bundle_layout_->cache;
    paths.config_path = bundle_layout_->config;
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
      std::fprintf(stderr, "[mgr] cannot create %s: %s\n", root.c_str(), error.message().c_str());
  }

  // drawer is null without SDK overlays (native renderer): no F2 dialog then.
  void InstallGameUi(rex::ui::ImGuiDrawer* drawer) {
    MgrTestScriptAttachWindow();  // the window has just opened
    // Cvar callbacks may originate off the UI thread. The weak token prevents
    // queued resize work from accessing this app after shutdown.
    const std::weak_ptr<uint8_t> alive = settings_callbacks_alive_;
    auto* ui_context = &app_context();
    rex::cvar::RegisterChangeCallback(
        "mgr_output_resolution", [this, alive, ui_context](std::string_view, std::string_view) {
          if (alive.expired()) return;
          ui_context->CallInUIThread([this, alive] {
            if (!alive.expired()) MgrApplyOutputResolution(window());
          });
        });
    MgrApplyOutputResolution(window());
    // mgr_settings maps mgr_vsync to the allowed present mode; only a new
    // swapchain picks it up, and a reconnect recreates it at the same size.
    rex::cvar::RegisterChangeCallback(
        "mgr_vsync", [this, alive, ui_context](std::string_view, std::string_view) {
          if (alive.expired()) return;
          ui_context->CallInUIThread([this, alive] {
            if (alive.expired()) return;
            auto* gs = runtime() ? runtime()->graphics_system() : nullptr;
            if (auto* presenter = gs ? gs->presenter() : nullptr) presenter->OnSurfaceResizeFromUIThread();
          });
        });
    MgrGameOptionsInstall();
    MgrNativeOptionsConfigure(config_path_);
    // The retail Options lifecycle owns normal settings access. The legacy
    // host dialog is available only when explicitly enabling developer tools.
    if (!drawer || !std::getenv("MGR_DEVELOPER_UI")) return;
    rex::ui::RegisterBind("bind_mgr_options", "F2", "Toggle MGR options", [this, drawer] {
      if (options_dialog_) {
        options_dialog_.reset();
        return;
      }
      auto* input = runtime() ? static_cast<rex::input::InputSystem*>(runtime()->input_system())
                              : nullptr;
      options_dialog_ = std::make_unique<MgrOptionsDialog>(drawer, input, config_path_, window());
    });
  }

 protected:
  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override {
    MgrApplyOutputResolution(window());
  }

  // Diagnostic: MGR_CAPTURE_DIR=<dir> dumps the guest frontbuffer as PPM
  // every CAPTURE_INTERVAL. Pattern from XDanfr/RaymanOriginsRecomp
  // rex/src/rayman_app.h (c1bf7b9), with a joined thread instead of detached.
  void OnPostSetup() override {
    std::fprintf(stderr,
        "[mgr-build] %s runtime_menu_pacing=%s nonblocking_present=%s frame_rate=%s "
        "internal_scale=%s precision=%s output=%s\n",
        MGR_BUILD_ID, rex::cvar::GetFlagInfo("imgui_guest_paced") ? "available" : "MISSING",
        rex::cvar::GetFlagByName("vulkan_nonblocking_guest_present").c_str(),
        rex::cvar::GetFlagByName("mgr_frame_rate").c_str(),
        rex::cvar::GetFlagByName("mgr_resolution_scale").c_str(),
        rex::cvar::GetFlagByName("mgr_render_target_precision").c_str(),
        rex::cvar::GetFlagByName("vulkan_output_resolution").c_str());
    DumpGuestImageIfRequested();
    MgrPromptDeviceInstall();
    const char* perf_log = std::getenv("MGR_PERF_LOG");
    if (perf_log && perf_log[0] == '1') MgrStartSessionLog(MGR_BUILD_ID);
    const char* dir = std::getenv("MGR_CAPTURE_DIR");
    if (!dir || !*dir) return;
    capture_thread_ = std::jthread(
        [this, out_dir = std::string(dir)](std::stop_token stop) {
          CaptureLoop(stop, out_dir);
        });
  }

  void OnShutdown() override {
    settings_callbacks_alive_.reset();
    MgrPromptDeviceUninstall();
    rex::ui::UnregisterBind("bind_mgr_options");  // its callback captures this
    MgrGameOptionsUninstall();
    game_options_dialog_.reset();
    options_dialog_.reset();
    MgrStopSessionLog();
    capture_thread_.request_stop();
    if (capture_thread_.joinable()) capture_thread_.join();
  }

 private:
  static constexpr uint32_t IMAGE_BASE = 0x82000000;
  static constexpr uint32_t IMAGE_END = 0x83A40000;

  // Diagnostic: MGR_DUMP_IMAGE=<file> writes the loaded (decrypted,
  // decompressed) guest image for offline function-boundary analysis.
  void DumpGuestImageIfRequested() {
    const char* path = std::getenv("MGR_DUMP_IMAGE");
    if (!path || !*path || !runtime()) return;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
      std::fprintf(stderr, "[mgr-dump] cannot open %s\n", path);
      return;
    }
    const uint8_t* base = runtime()->virtual_membase() + IMAGE_BASE;
    size_t written = std::fwrite(base, 1, IMAGE_END - IMAGE_BASE, f);
    std::fclose(f);
    std::fprintf(stderr, "[mgr-dump] wrote %zu bytes to %s\n", written, path);
  }

  static constexpr auto CAPTURE_INTERVAL = std::chrono::seconds(10);
  static constexpr auto CAPTURE_POLL = std::chrono::milliseconds(200);
  static constexpr auto SCRIPTED_CAPTURE_POLL = std::chrono::milliseconds(5);

  void CaptureLoop(std::stop_token stop, const std::string& out_dir) {
    if (MgrTestScriptActive()) return ScriptedCaptureLoop(stop, out_dir);
    int index = 0;
    auto next = std::chrono::steady_clock::now() + CAPTURE_INTERVAL;
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(CAPTURE_POLL);
      if (std::chrono::steady_clock::now() < next) continue;
      next += CAPTURE_INTERVAL;
      rex::ui::RawImage image;
      if (!CaptureFrame(image)) {
        std::fprintf(stderr, "[mgr-capture] no guest output yet\n");
        continue;
      }
      WritePpm(out_dir + "/frame_" + std::to_string(index++) + ".ppm", image);
    }
  }

  // MGR_TEST_SCRIPT "capture" events write <out_dir>/<name>-<frame>.ppm.
  void ScriptedCaptureLoop(std::stop_token stop, const std::string& out_dir) {
    std::string name;
    while (!stop.stop_requested()) {
      if (!MgrTestScriptTakeCapture(name)) {
        std::this_thread::sleep_for(SCRIPTED_CAPTURE_POLL);
        continue;
      }
      rex::ui::RawImage image;
      if (CaptureFrame(image)) {
        WritePpm(out_dir + "/" + name + ".ppm", image);
      } else {
        std::fprintf(stderr, "[mgr-capture] %s: no guest output yet\n", name.c_str());
      }
      MgrTestScriptCaptureDone();
    }
  }

  // The guest frame from the SDK presenter, or the native renderer's present.
  bool CaptureFrame(rex::ui::RawImage& image) {
#if MGR_NATIVE_RENDERER
    if (mgrn::NativeRendererActive()) return mgrn::MgrNativeCapture(image);
#endif
    auto* gs = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = gs ? gs->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  }

  static void WritePpm(const std::string& path, const rex::ui::RawImage& image) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "[mgr-capture] cannot open %s\n", path.c_str());
      return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) std::fwrite(row + x * 4, 1, 3, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[mgr-capture] wrote %s (%ux%u)\n", path.c_str(), image.width,
                 image.height);
  }

  std::jthread capture_thread_;
  std::filesystem::path config_path_;
  std::filesystem::path game_root_;
  std::optional<mgr_bundle::DataLayout> bundle_layout_;
  std::unique_ptr<MgrOptionsDialog> options_dialog_;
  std::unique_ptr<MgrOptionsDialog> game_options_dialog_;
  std::shared_ptr<uint8_t> settings_callbacks_alive_ = std::make_shared<uint8_t>(0);
};
