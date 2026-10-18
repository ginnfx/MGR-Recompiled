#pragma once

#include <rex/ui/imgui_dialog.h>

#include <filesystem>
#include <string>

namespace rex::ui {
class Window;
}

namespace rex::input {
class InputSystem;
}

// In-game "MGR Options" window (F2): the MGR cvars with friendly labels.
// Guest input is blocked while it is open.
class MgrOptionsDialog : public rex::ui::ImGuiDialog {
 public:
  MgrOptionsDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                   std::filesystem::path config_path, rex::ui::Window* window);
  ~MgrOptionsDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool DrawOutputResolution();
  rex::input::InputSystem* input_;
  rex::ui::Window* window_;
  std::filesystem::path config_path_;
  std::string last_output_selection_;
  int output_width_ = 0;
  int output_height_ = 0;
  bool custom_output_ = false;
  bool saved_ = false;
};
