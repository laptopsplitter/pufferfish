// window.hpp - the SDL2 window standing in for the Android Surface, and the
// few ANativeWindow/EGL calls that need translating for the host's EGL.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace tonic {

// Registers the shims above. Call before relocating the library.
void register_window_shims();

// Opens the window (W x H pixels). Must run on the main thread, before the
// library is called. Uses whatever SDL picks (Wayland or X11); set
// SDL_VIDEODRIVER=x11 to force XWayland.
bool window_open(int w, int h, std::string& err);
bool window_is_open();

// Handles window events. Returns false once the window has been closed.
// Main thread only.
bool window_pump(int timeout_ms);

// Shows the game window (when the menu closes). Main thread.
void window_show_game();

// One of tonic's windows has the keyboard (you are in tonic, not elsewhere). Any thread.
bool window_focused();

// Game pixels per window pixel (below 1 with a resolution cap). Any thread.
void window_render_scale(float& sx, float& sy);

// --- for the Vulkan presenter (vkshim.cpp) ---
void frame_done(bool ok);         // after each presented game frame
void window_present_gate();       // before presenting: waits while the window is being shown
void window_present_pace();       // after presenting: the background frame rate
bool window_take_shot();          // F12 was pressed (cleared)
void window_save_shot(std::shared_ptr<std::vector<uint8_t>> rgba, int w, int h);  // rows top down
void window_vk_target(bool& wayland, void*& display, void*& surface);  // wl_display + wl_surface, or Display* + Window
void window_size(int& w, int& h);
int window_vsync();
void window_set_vulkan();
void window_render_size(int& w, int& h);  // what the game draws at (smaller with a resolution cap)
void window_cap_unsupported();    // the cap cannot be done: the game draws the whole window

// Where the last F12 screenshot went ("" none yet). Any thread.
std::string window_last_screenshot();

}  // namespace tonic
