// overlaywin.hpp - a surface of tonic's own on top of the game window, for
// the closing screen (closescreen.hpp): a Wayland sub-surface of the game
// window's surface, or an X11 child window. It shows whatever Roblox does
// (Roblox may stop drawing frames while it closes), with the game window's
// size and place. Clicks go through to the game window, where Cancel is
// handled. Window thread only.
#pragma once

namespace tonic {

// DISPLAY / PARENT: wl_display* + wl_surface*, or Display* + Window. W x H:
// the window's size (logical on Wayland). False: not possible here.
bool overlaywin_show(bool wayland, void* display, void* parent, int w, int h);
void overlaywin_frame();  // draws the closing screen when it changed
void overlaywin_hide();
bool overlaywin_on();

// Wayland: the game window's surface is opaque (the desktop never shows
// through where Roblox leaves alpha below 1). Window thread.
void overlaywin_opaque(void* display, void* surface);

}  // namespace tonic
