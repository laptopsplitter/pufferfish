// appicon.hpp - tonic's icon (assets/icon.png) on its
// windows: title bar, taskbar, Alt+Tab. X11 takes it from the window
// (_NET_WM_ICON); Wayland desktops take it from a .desktop file named after the
// window's app id ("tonic"): `make desktop` writes one.
#pragma once

#include <string>

struct SDL_Window;

namespace tonic {

// Puts the logo on WINDOW, padded to a square. Nothing if the file is missing.
void app_icon_set(SDL_Window* window);

// The logo's absolute path (for .desktop files), or "" if it is missing.
std::string app_icon_path();

}  // namespace tonic
