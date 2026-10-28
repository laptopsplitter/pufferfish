// game.hpp - starting a game when the Lua app asks, as the Android Java side
// does: Play publishes a "Game.launch" message on the message bus ({placeId,
// userId, accessCode, linkCode, gameInstanceId, ...}); Java (sg/p.b, wh/i0,
// wh/h0.C) turns it into StartGameParams and calls
// NativeGLInterface.nativeAppBridgeV2StartGameWithParam on its own thread.
// Here the call runs on the app's main loop (mainloop.hpp).
#pragma once

#include <string>

#include "elfimg.hpp"

namespace tonic {

// Before the library runs: listens for the launch message and for the logged-in
// user (DID_LOG_IN notification: username, isUnder13).
void game_init();

// Subscribes to Game.launch. On an attached thread, before or after StartApp.
void game_enable(const ElfImage& img);

// The window changed size (Android's surfaceChanged): tells the app, and the
// game if one has started, the new surface and its physical size in mm.
// Main loop only.
void game_surface_changed(int width_mm, int height_mm);

// The window is closing: leave the game and stop the app the way Android
// does, so Roblox saves its settings. Waits up to a few seconds.
void game_shutdown();

// Joins PLACE as soon as the app is up and logged in (the menu's quick join).
// USER > 0: join that user's server (follow); LINK_CODE: a private server's
// link code (letters, digits, - and _ only; anything else is dropped).
// How many times a game has been launched this run (0: none yet). Any thread.
int game_launches();

void game_quick_join(long long place, long long user = 0, const std::string& link_code = "");

}  // namespace tonic
