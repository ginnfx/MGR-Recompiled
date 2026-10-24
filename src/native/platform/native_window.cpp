/**
 * @file    platform/native_window.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR (macOS only): instead of re-executing the binary with
 * DYLD_LIBRARY_PATH, the Vulkan loader staged by the SDK build
 * (<exe>/vulkan/lib) is opened by absolute path and handed to volk, and the
 * MoltenVK ICD is selected through VK_DRIVER_FILES before plume starts.
 */
#include "platform/platform.h"

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_video.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include <volk.h>

#include <rex/system.h>
#include <rex/ui/window.h>

#include "core/logging.h"

namespace mgrn::platform {

namespace {

void SetenvIfUnset(const char *name, const std::string &value) {
  if (const char *cur = std::getenv(name); !cur || !cur[0])
    setenv(name, value.c_str(), 1);
}

std::string ExecutableDir() {
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0)
    return {};
  const std::string exe(buffer);
  return exe.substr(0, exe.find_last_of('/'));
}

} // namespace

bool PrepareVulkanLoader() {
  if (vkGetInstanceProcAddr)
    return true;
  // One Metal queue and synchronous submits, as re:Blue runs MoltenVK.
  SetenvIfUnset("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1");
  SetenvIfUnset("MVK_CONFIG_SEMAPHORE_SUPPORT_STYLE", "3");
  SetenvIfUnset("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1");

  const std::string dir = ExecutableDir() + "/vulkan";
  const std::string icd = dir + "/share/vulkan/icd.d/MoltenVK_icd.json";
  SetenvIfUnset("VK_DRIVER_FILES", icd);
  SetenvIfUnset("VK_ICD_FILENAMES", icd);

  const std::string loader = dir + "/lib/libvulkan.1.dylib";
  void *module = dlopen(loader.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    MGRN_ERROR("Cannot open the Vulkan loader {}: {}", loader, dlerror());
    return false;
  }
  auto proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
  if (!proc) {
    MGRN_ERROR("{} exports no vkGetInstanceProcAddr", loader);
    return false;
  }
  volkInitializeCustom(proc);
  return true;
}

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  // The SDK owns the single SDL window.
  (void)window;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!sdl_window) {
    MGRN_ERROR("No SDL window available for the Vulkan surface");
    return false;
  }

  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
  out.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);

  // The view is a subview of the SDL window and dies with it, so it is cached
  // against the window it was made for.
  static SDL_Window *view_owner = nullptr;
  static SDL_MetalView metal_view = nullptr;
  if (!metal_view || view_owner != sdl_window) {
    metal_view = SDL_Metal_CreateView(sdl_window);
    view_owner = sdl_window;
  }
  if (!metal_view) {
    MGRN_ERROR("SDL_Metal_CreateView failed: {}", SDL_GetError());
    return false;
  }
  out.view = SDL_Metal_GetLayer(metal_view);

  if (!out.window || !out.view) {
    MGRN_ERROR("SDL window exposed no NSWindow/CAMetalLayer for the Metal surface");
    return false;
  }
  return true;
}

void ShowFatalError(std::string_view title, std::string_view body) {
  MGRN_CRITICAL("{}: {}", title, body);
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                            std::string(title) + "\n\n" + std::string(body));
}

} // namespace mgrn::platform
