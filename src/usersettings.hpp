// usersettings.hpp - Roblox's own saved settings (the in-game Settings menu):
// GlobalBasicSettings_13.xml, which the library writes under ./data. With
// TONIC_FPS_CAP=N its FramerateCap ("Maximum Frame Rate") is set to N
// before the library starts, as choosing it in the menu would.
#pragma once

namespace tonic {

void usersettings_apply();

// The menu's RAM limit (config "ram_limit_mb", 0: none), a soft one: past it
// the kernel slows tonic down and pushes memory to swap instead of killing it
// (MemoryHigh). memory_scope_enter starts tonic again inside its own systemd
// user scope (first thing in main, windowed runs); memory_limit_apply sets
// the limit on it, also while running (the menu calls it on a change).
void memory_scope_enter(int argc, char** argv);
void memory_limit_apply();

// Newest modification time of Roblox's saved settings under data/ (0: none).
long long usersettings_mtime();

}  // namespace tonic
