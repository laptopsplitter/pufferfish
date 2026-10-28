// audio.hpp - FMOD's Java AudioTrack device (org/fmod/AudioDevice) on SDL
// audio, plus the display refresh rates Java reports. TONIC_NO_AUDIO=1: silent.
#pragma once

#include "elfimg.hpp"

namespace tonic {

// Before the library runs.
void audio_init();

// One of tonic's windows has the keyboard (for "mute when away"). Any thread.
void audio_set_focused(bool focused);

// Once the app has started (window mode): the display's refresh rates.
void display_rates_report(const ElfImage& img);
// Thermal status and battery, as Android's PowerManager / BatteryManager
// observers report them (wh/y0, wh/h): no throttling, mains power.
void power_state_report(const ElfImage& img);

}  // namespace tonic
