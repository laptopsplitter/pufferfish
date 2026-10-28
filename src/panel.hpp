// panel.hpp - the settings pages beyond General, on the launcher's settings
// page (menu.cpp): Performance, Picture, Sound, Look and Instances. Mods like
// Bloxstrap's (mods.hpp): graphics turned down for frame rate, colour
// filters, sounds, cursor, font; and more tonics at once (profile.hpp). Only
// what you see and hear on your own client; nothing changes how a game plays.
// Like the rest of the settings, they are there before Roblox starts and
// apply when it does.
#pragma once

#include <SDL.h>

namespace tonic {

enum PanelTab { kTabPerformance = 1, kTabPicture, kTabSound, kTabLook, kTabControls, kTabInstances };

// Draws page TAB in R, inside the column X..X+W from Y down to Y+H. PX and
// SMALL are the text sizes, ROW_H the row height; MX, MY the mouse.
void panel_draw(SDL_Renderer* r, int tab, int x, int y, int w, int h, int px, int small, int row_h, int mx, int my);

// A click at MX, MY on the page drawn last: true when it hit something.
bool panel_click(int mx, int my);

}  // namespace tonic
