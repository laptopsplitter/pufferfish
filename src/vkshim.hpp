// vkshim.hpp - Vulkan for Roblox: the desktop loader behind a thin layer that
// turns VK_KHR_android_surface into a Wayland/X11 surface, drops Android-only
// extensions, applies VSync and the resolution cap, and draws tonic's overlays.
// A crash before the first Vulkan frame makes the next start use OpenGL.
#pragma once

#include <string>

namespace tonic {

void register_vulkan_shims();  // dlopen/dlsym/dlclose for the library
void vulkan_decide();          // before Roblox starts (window open): Vulkan or OpenGL this run
void vulkan_exit();            // tonic closing normally
bool vulkan_enabled();         // offered to Roblox this run
std::string renderer_setting();  // "vulkan" (default) or "opengl"

}  // namespace tonic
