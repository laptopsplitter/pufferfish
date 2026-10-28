// mods.hpp - visual-only mods: low graphics, grey sky, cursors, your own death
// sound and font. Originals kept in data/mods/originals/VERSION.
#pragma once

#include <string>

namespace tonic {

// Files and rendering flags as the settings say; before Roblox starts, and
// again from the mod panel (files take effect when Roblox next loads them).
void mods_apply_files();
void mods_apply_flags();  // into data/settings/preload-overrides.json (read at start)

// A one-line summary of what is on, for logs.
std::string mods_summary();

}  // namespace tonic
