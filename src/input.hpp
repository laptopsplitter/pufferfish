// input.hpp - mouse, keyboard, gamepad and text input, passed to the library
// through the same natives the Android app uses (NativeInputInterface).
// Coordinates are window pixels divided by the dpi scale.
#pragma once

#include <SDL.h>

#include <string>

#include "elfimg.hpp"

namespace tonic {

// Starts watching the library's calls into Java for TextBox focus. Call once,
// before the library runs.
void input_init(bool touch);

// Looks up the input natives in IMG and starts passing events. Call after the
// app has started (the library ignores or mishandles input before that). Any
// thread.
void input_enable(const ElfImage& img);

// Handles one SDL event. Main thread (the window's thread) only; it calls into
// the library on that thread, as Android's UI thread does.
void input_event(const SDL_Event& e);

// The focused TextBox's text (false when none has focus), for the overlay
// that shows it (textoverlay.hpp). Any thread.
bool input_focused_text(std::string& text);

// Caffeinate, anti-AFK (Settings > Controls): once you have given no input for a few
// minutes, a one-pixel mouse movement and back. Main thread, often.
void input_tick();

void input_test_virtual_pad();  // TONIC_TEST_PAD=1 (make test-gamepad)

}  // namespace tonic
