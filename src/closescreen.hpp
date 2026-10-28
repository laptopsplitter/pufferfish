// closescreen.hpp - "tonic is going for a shutdown", drawn in the game
// window itself (over Roblox's last frames), so it has the window's size and
// place on every desktop (Wayland does not let a second window be put on
// top of the first). Logo, the loading bar, and Cancel while it can still be
// cancelled. Drawn on the CPU: OpenGL uploads it, Vulkan copies it in.
#pragma once

#include <cstdint>
#include <vector>

namespace tonic {

void closescreen_set(int state);  // 0 off, 1 Cancel offered, 2 Roblox saving and leaving
int closescreen_state();

// The screen at W x H as RGBA, top row first; GEN changes when the picture
// does (redrawn at most 30 times a second). False when it is off.
bool closescreen_image(int w, int h, const std::vector<uint8_t>*& rgba, long& gen);

// Cancel's place in window pixels (top-left origin).
bool closescreen_cancel_hit(int x, int y, int w, int h);

}  // namespace tonic
