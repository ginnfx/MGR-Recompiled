#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::input { class InputSystem; }
namespace rex::ui { class Window; }

// The integrated instance follows the original OPTIONS object. The F2 instance
// remains a shortcut. Both edit the same cvars and configuration file.
class MgrOptionsDialog : public rex::ui::ImGuiDialog,
                         public rex::ui::WindowInputListener,
                         public rex::ui::WindowListener {
 public:
  MgrOptionsDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                   std::filesystem::path config_path, rex::ui::Window* window,
                   bool integrated = false);
  ~MgrOptionsDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent&) override;
  void OnLostFocus(rex::ui::UISetupEvent&) override;

 private:
  enum class Page { kSystem, kGraphics, kKeyboard };
  bool DrawOutputResolution();
  void DrawIntegrated(ImGuiIO& io);
  bool DrawKeyboard();
  void SelectPage(Page page);
  void SetInputBlocked(bool blocked);
  void SavePending();
  void StartCapture(const char* name, bool append);
  void FinishCapture(std::string key);
  void PollController(ImGuiIO& io);
  void ReleaseController(ImGuiIO& io);
  static bool WatchController(void* self, SDL_Event* event);

  rex::input::InputSystem* input_;
  rex::ui::Window* window_;
  std::filesystem::path config_path_;
  std::string last_output_selection_;
  int output_width_ = 0;
  int output_height_ = 0;
  bool custom_output_ = false;
  bool saved_ = false;
  bool integrated_ = false;
  bool input_blocked_ = false;
  bool native_visible_ = false;
  bool dirty_ = false;
  uint32_t session_ = 0;
  Page page_ = Page::kSystem;
  std::string capturing_bind_;
  std::string pending_modifier_;
  bool capture_append_ = false;
  int capture_started_frame_ = 0;
  int saved_nav_flags_ = 0;
  bool nav_owned_ = false;
  bool saved_backend_gamepad_ = false;
  bool focus_first_ = false;
  bool has_focus_ = true;
  std::atomic<SDL_JoystickID> controller_{0};
  std::atomic<uint32_t> controller_buttons_{0};
  std::atomic<int16_t> controller_x_{0}, controller_y_{0};
  uint32_t previous_controller_ = 0;
  uint32_t injected_controller_ = 0;
};
