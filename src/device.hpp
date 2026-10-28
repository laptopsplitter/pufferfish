#pragma once
// What tonic tells Roblox about "the phone" (Settings > Look): ui_size (display
// density), ui_layout (phone|tablet), roblox_theme. Applied at Play.
#include <string>

namespace tonic {

void device_apply_settings();  // after --values, and again when Play is pressed
std::string device_roblox_theme();
float device_dpi_scale();  // the density Roblox was told (1.0 = 100 %)

// Margin from the window's edges (config ui_margin: dp, -1 auto): told to
// Roblox as Android's safe-area insets (Screen.safeAreaInsets, as the app's
// wh/w0 sends them), so its UI keeps off the edges. Auto: none at 100 %, 16 dp
// when the UI is bigger. Once the app is up (game.cpp) and at each game start.
int device_ui_margin();
void device_send_insets(const class ElfImage& img);  // "dark" | "light": what Roblox last had, else the setting

// Roblox's icons come in 1x, 2x and 3x sheets; it uses the 1x one up to 150 %
// and the 2x one up to 250 % and stretches it, so neighbouring icons bleed in
// (a "background" around them). Only sizes where it shrinks a bigger sheet
// are offered: 100, 160-200, 260-300 %. Under 100 % Roblox draws its rounded
// corners square, so none smaller.
int device_ui_size();             // the setting, snapped to those
int device_ui_size_step(int dir);  // the next one up or down

}  // namespace tonic
