// menu.hpp - the loading screen and tonic's settings, in their own window
// while Roblox starts (SDL's software renderer, drawn on the window thread, so
// it stays smooth whatever the game does). Roblox starts when Play or a game
// under "Jump back in" is pressed; the settings are there before that. The
// menu closes by itself once Roblox draws at speed.
// Settings (config.hpp): frame rate cap, VSync, stats overlay, fullscreen,
// recent games with quick join, log out. TONIC_NO_LOADING_SCREEN=1: none of it.
#pragma once

#include <SDL.h>

#include <string>

namespace tonic {

bool menu_open(int w, int h);  // false: no menu (show the game window yourself)
bool menu_active();
void menu_links_tick();  // Roblox links handed over by another tonic (links.hpp), from window_pump
void menu_frame();                   // draws; closes the menu when it is time
void menu_event(const SDL_Event& e);

// Blocks until Play (or a recent game) is pressed; at once when there is no
// menu. The library thread calls it before Roblox starts.
void menu_wait_start();

// No Roblox installed: the menu only gets it (apk.hpp: the newest from
// APKPure) and starts tonic again once it is installed. Before menu_open.
void menu_set_setup();
// Why (shown on that page), when an installed Roblox failed its check.
void menu_set_setup_reason(const std::string& why);

// Closing while Roblox runs: "tonic is going for a shutdown" in the game
// window (closescreen.hpp), with Cancel for a moment, then the shutdown.
bool menu_roblox_started();
bool menu_closing_begin();  // false: Roblox not started, or already closing (close at once)
int menu_closing_state();   // 0 not closing, 1 cancel possible, 3 that moment is over, 2 shutting down
void menu_closing_go();     // shutting down: the screen stays, without Cancel
void menu_closing_cancel(); // Cancel pressed: back to the game

// Starts tonic again with the same arguments (the library loads once, at
// start). PLAY: go straight into Roblox instead of stopping at the launcher.
void restart_tonic(const char* why, bool play = false);

}  // namespace tonic
