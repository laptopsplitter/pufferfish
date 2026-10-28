// gamemode.hpp - asks Feral GameMode (libgamemode.so.0) for performance while
// the game window has focus. Nothing happens without it installed.
#pragma once

#include <string>

namespace tonic {

// Call often (window thread): WANT is whether GameMode should be on now.
void gamemode_want(bool want);

// "on", "off", "missing" (not installed) or "error: ..." for the panel.
std::string gamemode_status();

}  // namespace tonic
