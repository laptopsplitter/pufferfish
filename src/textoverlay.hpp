// textoverlay.hpp - draws the text being typed over the focused TextBox (on
// Android a real EditText does), with the app's own fonts. Passwords as dots.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tonic {

// A NativeTextBoxInfo's constructor arguments (jni_ctor_args): from
// showKeyboard, or nativeGetTextBoxInfo after the box moved.
void textoverlay_set_info(const std::vector<uint64_t>& ctor_args);

// Called on the render thread with the GL context current, before
// eglSwapBuffers. W x H is the surface size in pixels.
void textoverlay_draw(int w, int h, float scale = 1.f);  // SCALE: window pixels per game pixel

// Resolution cap: the game drew RW x RH; scale it up to the window's WW x WH.
// Render thread, before textoverlay_draw and eglSwapBuffers. False: it cannot
// be done here (the cap is then turned off).
bool textoverlay_upscale(int rw, int rh, int ww, int wh);

// A picture filter or sharpening is on (mod panel): every frame goes
// through textoverlay_upscale, at the same size when there is no cap.
bool textoverlay_postfx_on();

// The window's frame as RGBA, bottom row first (for screenshots). Render
// thread, before eglSwapBuffers.
bool textoverlay_read_frame(int w, int h, std::vector<uint8_t>& rgba);

// Vulkan (vkshim.cpp): the same overlays drawn on the CPU into the image
// being presented. Rects are window pixels, top-left origin; BLEND rects
// need the game's pixels under them (read back first), the others do not.
struct OverlayRect {
  int x, y, w, h;
  bool blend;
};
void textoverlay_presented();  // counts a presented frame (stats, loading)
// SCALE: window pixels per game pixel (resolution cap).
std::vector<OverlayRect> textoverlay_cpu_rects(int w, int h, float scale = 1.f);
void textoverlay_cpu_paint(const OverlayRect& r, uint8_t* px, int stride, bool bgra, float scale = 1.f);  // PX: R's pixels, top row first

// True once the game presents frames at speed (past startup's slow frames):
// the loading window (window.cpp) closes then. Any thread.
bool textoverlay_ready();

// TEXT as white pixels with coverage in alpha (RGBA, W x H; ADVANCE is the
// text's width), in Roblox's UI font from the APK's assets when there is
// one. For the loading window. Any thread.
bool ui_text_bitmap(const char* text, int px, bool bold, std::vector<uint8_t>& rgba, int& w, int& h, int& advance,
                    bool mono = false);  // MONO: a monospaced font, for code and logs

// Frames presented in the last second, and the longest frame time in the
// last quarter second (ms). Any thread.
void textoverlay_stats(int& fps, float& worst_ms);

}  // namespace tonic
