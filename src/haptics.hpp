#pragma once
// Rumble: when a game vibrates "the phone" (Roblox's HapticProtocol on the
// message bus, as Roblox's Android app answers it: hm/b), your controllers
// rumble. Config: rumble (on), rumble_strength (percent, 100).
#include "elfimg.hpp"

namespace tonic {

void haptics_init();                        // before the library runs (listens for the requests)
void haptics_enable(const ElfImage& img);   // once the app is up (main loop thread)

}  // namespace tonic
