#include "input.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "config.hpp"
#include "fakejni.hpp"
#include "mainloop.hpp"
#include "textoverlay.hpp"
#include "window.hpp"
#include "script.hpp"
#include "shims.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[input] " fmt, ##__VA_ARGS__)

using u8 = uint8_t;
constexpr const char* kInput = "com/roblox/engine/jni/NativeInputInterface";
constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";

// The natives, typed as the C side declares them: (JNIEnv*, jclass, args...).
struct Natives {
  void (*mouse_move)(void*, void*, float, float, float, float) = nullptr;
  void (*mouse_button)(void*, void*, float, float, u8, int) = nullptr;
  void (*mouse_wheel)(void*, void*, float, float, float) = nullptr;
  void (*touch)(void*, void*, int, float, float, int, int, int) = nullptr;
  void (*tap)(void*, void*, float, float) = nullptr;
  void (*key)(void*, void*, u8, int, int, u8) = nullptr;
  void (*text)(void*, void*, int64_t, void*, u8, int) = nullptr;
  void (*return_pressed)(void*, void*, int64_t) = nullptr;
  void (*release_focus)(void*, void*, int64_t) = nullptr;
  void (*sync)(void*, void*, void*, int) = nullptr;  // syncTextboxTextAndCursorPosition2
  void* (*textbox_info)(void*, void*) = nullptr;      // nativeGetTextBoxInfo
  u8 (*mouse_locked)(void*, void*) = nullptr;           // nativeGetMainWindowIsMouseLockedCenter (shift lock, first person)
  // Gamepads (as mk/e passes Android's InputDevice events on)
  void (*pad_connect)(void*, void*, int, int) = nullptr;           // (deviceId, gamepadType)
  void (*pad_disconnect)(void*, void*, int) = nullptr;             // (deviceId)
  void (*pad_button)(void*, void*, int, int, int) = nullptr;       // (deviceId, KEYCODE_*, 1 down / 0 up)
  void (*pad_axis)(void*, void*, int, int, float, float, float) = nullptr;  // (deviceId, AXIS_*, x, y, z)
  void (*pad_key_supported)(void*, void*, int, int, u8, int) = nullptr;     // (deviceId, KEYCODE_*, has, type)
  void (*pad_motion_supported)(void*, void*, int, int, int, u8, int) = nullptr;  // (deviceId, AXIS_*, direction, has, type)
};
Natives g_n;
std::atomic<bool> g_enabled{false};
bool g_touch = false;

// The focused TextBox, set from the library's threads (showKeyboard /
// hideKeyboard), read on the main thread.
std::mutex g_mu;
int64_t g_textbox = 0;
std::string g_text;   // the TextBox's text as we last told it (UTF-8)

float g_x = 0, g_y = 0;  // last mouse position, scaled

// Anti-AFK (input_tick): when you last gave input, and when tonic last nudged.
Uint32 g_last_input = 0, g_last_nudge = 0;

// Sensitivity multipliers (settings: Controls), read twice a second.
struct Sens {
  float mouse = 1, move = 1, look = 1;
};
Sens sens() {
  static Sens s;
  static Uint32 checked = 0;
  const Uint32 now = SDL_GetTicks();
  if (!checked || now - checked > 500) {
    checked = now;
    auto get = [](const char* k) { return std::clamp(config_int(k, 100), 10, 500) / 100.f; };
    s.mouse = get("sens_mouse"), s.move = get("sens_pad_move"), s.look = get("sens_pad_look");
  }
  return s;
}
bool g_finger_down = false;
float g_down_x = 0, g_down_y = 0;

void* lookup(const ElfImage& img, const char* cls, const char* name, const char* desc) {
  if (uint64_t fn = jni_registered_native(cls, name, desc)) return reinterpret_cast<void*>(fn);
  if (void* p = img.find_symbol(jni_short_name(cls, name).c_str())) return p;
  return img.find_symbol(jni_long_name(cls, name, desc).c_str());
}

template <typename F>
void find(const ElfImage& img, F& slot, const char* cls, const char* name, const char* desc) {
  slot = reinterpret_cast<F>(lookup(img, cls, name, desc));
  if (!slot) LOG("%s.%s%s not found; that input is ignored\n", cls, name, desc);
}

// UTF-16 length of UTF-8 text: Java's cursor positions count UTF-16 units.
int utf16_len(const std::string& s) {
  int n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = s[i];
    if ((c & 0xC0) == 0x80) continue;
    n += c >= 0xF0 ? 2 : 1;
  }
  return n;
}

// Window points -> pixels / dpiScale(1.0), the coordinates Android passes.
void scaled(Uint32 window_id, float x, float y, float& ox, float& oy) {
  SDL_Window* w = SDL_GetWindowFromID(window_id);
  int pw = 0, ph = 0, ww = 0, wh = 0;
  if (w) SDL_GetWindowSizeInPixels(w, &pw, &ph), SDL_GetWindowSize(w, &ww, &wh);
  const float sx = ww > 0 ? static_cast<float>(pw) / ww : 1.f, sy = wh > 0 ? static_cast<float>(ph) / wh : 1.f;
  float rx, ry;  // the game may draw (and lay out) at fewer pixels: resolution cap
  window_render_scale(rx, ry);
  ox = x * sx * rx;
  oy = y * sy * ry;
}
void pixel_size(Uint32 window_id, int& w, int& h) {
  w = h = 0;
  if (SDL_Window* win = SDL_GetWindowFromID(window_id)) SDL_GetWindowSizeInPixels(win, &w, &h);
  float rx, ry;
  window_render_scale(rx, ry);
  w = static_cast<int>(w * rx);
  h = static_cast<int>(h * ry);
}

void* env() { return jni_env(); }
void* cls(const char* c) { return jni_class(c); }

// --- keys -------------------------------------------------------------------

struct KeyMap {
  SDL_Scancode sdl;
  int android;  // KeyEvent.KEYCODE_*
  int linux_;   // input-event-codes.h KEY_* (what Android's getScanCode returns)
};
const KeyMap kKeys[] = {
    {SDL_SCANCODE_A, 29, 30}, {SDL_SCANCODE_B, 30, 48}, {SDL_SCANCODE_C, 31, 46}, {SDL_SCANCODE_D, 32, 32},
    {SDL_SCANCODE_E, 33, 18}, {SDL_SCANCODE_F, 34, 33}, {SDL_SCANCODE_G, 35, 34}, {SDL_SCANCODE_H, 36, 35},
    {SDL_SCANCODE_I, 37, 23}, {SDL_SCANCODE_J, 38, 36}, {SDL_SCANCODE_K, 39, 37}, {SDL_SCANCODE_L, 40, 38},
    {SDL_SCANCODE_M, 41, 50}, {SDL_SCANCODE_N, 42, 49}, {SDL_SCANCODE_O, 43, 24}, {SDL_SCANCODE_P, 44, 25},
    {SDL_SCANCODE_Q, 45, 16}, {SDL_SCANCODE_R, 46, 19}, {SDL_SCANCODE_S, 47, 31}, {SDL_SCANCODE_T, 48, 20},
    {SDL_SCANCODE_U, 49, 22}, {SDL_SCANCODE_V, 50, 47}, {SDL_SCANCODE_W, 51, 17}, {SDL_SCANCODE_X, 52, 45},
    {SDL_SCANCODE_Y, 53, 21}, {SDL_SCANCODE_Z, 54, 44},
    {SDL_SCANCODE_1, 8, 2}, {SDL_SCANCODE_2, 9, 3}, {SDL_SCANCODE_3, 10, 4}, {SDL_SCANCODE_4, 11, 5},
    {SDL_SCANCODE_5, 12, 6}, {SDL_SCANCODE_6, 13, 7}, {SDL_SCANCODE_7, 14, 8}, {SDL_SCANCODE_8, 15, 9},
    {SDL_SCANCODE_9, 16, 10}, {SDL_SCANCODE_0, 7, 11},
    {SDL_SCANCODE_RETURN, 66, 28}, {SDL_SCANCODE_ESCAPE, 111, 1}, {SDL_SCANCODE_BACKSPACE, 67, 14},
    {SDL_SCANCODE_TAB, 61, 15}, {SDL_SCANCODE_SPACE, 62, 57}, {SDL_SCANCODE_MINUS, 69, 12},
    {SDL_SCANCODE_EQUALS, 70, 13}, {SDL_SCANCODE_LEFTBRACKET, 71, 26}, {SDL_SCANCODE_RIGHTBRACKET, 72, 27},
    {SDL_SCANCODE_BACKSLASH, 73, 43}, {SDL_SCANCODE_SEMICOLON, 74, 39}, {SDL_SCANCODE_APOSTROPHE, 75, 40},
    {SDL_SCANCODE_GRAVE, 68, 41}, {SDL_SCANCODE_COMMA, 55, 51}, {SDL_SCANCODE_PERIOD, 56, 52},
    {SDL_SCANCODE_SLASH, 76, 53}, {SDL_SCANCODE_CAPSLOCK, 115, 58},
    {SDL_SCANCODE_F1, 131, 59}, {SDL_SCANCODE_F2, 132, 60}, {SDL_SCANCODE_F3, 133, 61}, {SDL_SCANCODE_F4, 134, 62},
    {SDL_SCANCODE_F5, 135, 63}, {SDL_SCANCODE_F6, 136, 64}, {SDL_SCANCODE_F7, 137, 65}, {SDL_SCANCODE_F8, 138, 66},
    {SDL_SCANCODE_F9, 139, 67}, {SDL_SCANCODE_F10, 140, 68}, {SDL_SCANCODE_F11, 141, 87}, {SDL_SCANCODE_F12, 142, 88},
    {SDL_SCANCODE_PRINTSCREEN, 120, 99}, {SDL_SCANCODE_SCROLLLOCK, 116, 70}, {SDL_SCANCODE_PAUSE, 121, 119},
    {SDL_SCANCODE_INSERT, 124, 110}, {SDL_SCANCODE_HOME, 122, 102}, {SDL_SCANCODE_PAGEUP, 92, 104},
    {SDL_SCANCODE_DELETE, 112, 111}, {SDL_SCANCODE_END, 123, 107}, {SDL_SCANCODE_PAGEDOWN, 93, 109},
    {SDL_SCANCODE_RIGHT, 22, 106}, {SDL_SCANCODE_LEFT, 21, 105}, {SDL_SCANCODE_DOWN, 20, 108},
    {SDL_SCANCODE_UP, 19, 103}, {SDL_SCANCODE_NUMLOCKCLEAR, 143, 69}, {SDL_SCANCODE_KP_DIVIDE, 154, 98},
    {SDL_SCANCODE_KP_MULTIPLY, 155, 55}, {SDL_SCANCODE_KP_MINUS, 156, 74}, {SDL_SCANCODE_KP_PLUS, 157, 78},
    {SDL_SCANCODE_KP_ENTER, 160, 96}, {SDL_SCANCODE_KP_1, 145, 79}, {SDL_SCANCODE_KP_2, 146, 80},
    {SDL_SCANCODE_KP_3, 147, 81}, {SDL_SCANCODE_KP_4, 148, 75}, {SDL_SCANCODE_KP_5, 149, 76},
    {SDL_SCANCODE_KP_6, 150, 77}, {SDL_SCANCODE_KP_7, 151, 71}, {SDL_SCANCODE_KP_8, 152, 72},
    {SDL_SCANCODE_KP_9, 153, 73}, {SDL_SCANCODE_KP_0, 144, 82}, {SDL_SCANCODE_KP_PERIOD, 158, 83},
    {SDL_SCANCODE_LCTRL, 113, 29}, {SDL_SCANCODE_LSHIFT, 59, 42}, {SDL_SCANCODE_LALT, 57, 56},
    {SDL_SCANCODE_LGUI, 117, 125}, {SDL_SCANCODE_RCTRL, 114, 97}, {SDL_SCANCODE_RSHIFT, 60, 54},
    {SDL_SCANCODE_RALT, 58, 100}, {SDL_SCANCODE_RGUI, 118, 126}, {SDL_SCANCODE_APPLICATION, 82, 127},
};

const KeyMap* key_for(SDL_Scancode s) {
  for (const KeyMap& k : kKeys)
    if (k.sdl == s) return &k;
  return nullptr;
}

// --- text -------------------------------------------------------------------

// RbxKeyboard.k(): the EditText's whole text and cursor, sent before each
// nativePassText and before Enter is reported.
void sync_text() {
  std::string text;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_textbox) return;
    text = g_text;
  }
  if (g_n.sync) g_n.sync(env(), cls(kGL), jni_new_string(text), utf16_len(text));
}

// RbxKeyboard's TextWatcher.onTextChanged: sync, then nativePassText.
void send_text(bool done) {
  int64_t tb;
  std::string text;
  {
    std::lock_guard<std::mutex> l(g_mu);
    tb = g_textbox;
    text = g_text;
  }
  if (!tb || !g_n.text) return;
  if (!done) sync_text();
  static int shown = 0;  // lengths only, never the text
  if (shown < 2) ++shown, LOG("passing text to TextBox %#llx (%d chars%s)\n", static_cast<unsigned long long>(tb),
                              utf16_len(text), done ? ", done" : "");
  g_n.text(env(), cls(kGL), tb, jni_new_string(text), done, utf16_len(text));
}

void pop_codepoint(std::string& s) {
  while (!s.empty()) {
    const unsigned char c = s.back();
    s.pop_back();
    if ((c & 0xC0) != 0x80) break;
  }
}

// Returns true when the key was text editing for the focused TextBox (and so
// is not also passed as a key, as Android's EditText keeps it).
bool text_key(const SDL_KeyboardEvent& k) {
  int64_t tb;
  {
    std::lock_guard<std::mutex> l(g_mu);
    tb = g_textbox;
  }
  if (!tb) return false;
  if (k.type != SDL_KEYDOWN) return true;
  switch (k.keysym.scancode) {
    case SDL_SCANCODE_BACKSPACE: {
      std::lock_guard<std::mutex> l(g_mu);
      pop_codepoint(g_text);
      break;
    }
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      LOG("Enter in TextBox %#llx\n", static_cast<unsigned long long>(tb));
      sync_text();  // onEditorAction: k(), return pressed, then the final text
      if (g_n.return_pressed) g_n.return_pressed(env(), cls(kGL), tb);
      send_text(true);
      return true;
    case SDL_SCANCODE_ESCAPE:
      LOG("Esc: releasing TextBox %#llx\n", static_cast<unsigned long long>(tb));
      if (g_n.release_focus) g_n.release_focus(env(), cls(kGL), tb);
      {
        std::lock_guard<std::mutex> l(g_mu);
        g_textbox = 0;
      }
      return true;
    case SDL_SCANCODE_V:
      if (!(k.keysym.mod & KMOD_CTRL)) return true;
      if (char* clip = SDL_GetClipboardText()) {
        std::string add = clip;
        SDL_free(clip);
        for (char& c : add)
          if (c == '\n' || c == '\r') c = ' ';
        std::lock_guard<std::mutex> l(g_mu);
        g_text += add;
      }
      break;
    default:
      return true;  // characters arrive as SDL_TEXTINPUT
  }
  send_text(false);
  return true;
}

void on_java_call(const std::string& method, const std::vector<uint64_t>& raw) {
  // NativeGLJavaInterface.showKeyboard(long textBox, boolean showNativeInput,
  // byte[] text, NativeTextBoxInfo) and NativeHelper.gameActivity_showKeyboard
  // (same arguments).
  if ((method == "showKeyboard" || method == "gameActivity_showKeyboard") && raw.size() >= 3) {
    const std::string text = jni_text(reinterpret_cast<void*>(raw[2]));
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_textbox = static_cast<int64_t>(raw[0]);
      g_text = text;
    }
    if (raw.size() >= 4) textoverlay_set_info(jni_ctor_args(reinterpret_cast<void*>(raw[3])));
    // Never log the text itself: it may be a password.
    LOG("TextBox %#llx focused (%zu bytes of text); type, Enter to submit, Esc to leave, Ctrl+V to paste\n",
        static_cast<unsigned long long>(raw[0]), text.size());
  } else if (method == "hideKeyboard" || method == "gameActivity_hideKeyboard") {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_textbox) LOG("TextBox %#llx unfocused\n", static_cast<unsigned long long>(g_textbox));
    g_textbox = 0;
  } else if (method == "onLuaTextBoxPropertyChangedCallback" || method == "gameActivity_onLuaTextBoxPropertyChanged") {
    // The box moved or changed size: Java asks for its info again (game/b.c).
    if (g_n.textbox_info)
      main_post([] {
        if (void* info = g_n.textbox_info(jni_env(), jni_class(kGL))) textoverlay_set_info(jni_ctor_args(info));
      });
  } else if ((method == "onLuaTextBoxChangedCallback" || method == "gameActivity_onLuaTextBoxChanged") &&
             !raw.empty()) {
    // The game changed the focused TextBox's text itself.
    std::lock_guard<std::mutex> l(g_mu);
    if (g_textbox) g_text = jni_text(reinterpret_cast<void*>(raw[0]));
  }
}

}  // namespace

void input_init(bool touch) {
  g_touch = touch;
  set_jni_observer(on_java_call);
}

bool input_focused_text(std::string& text) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_textbox) return false;
  text = g_text;
  return true;
}

void input_enable(const ElfImage& img) {
  find(img, g_n.mouse_move, kInput, "nativePassMouseMove", "(FFFF)V");
  find(img, g_n.mouse_button, kInput, "nativePassMouseButton", "(FFZI)V");
  find(img, g_n.mouse_wheel, kInput, "nativePassMouseWheel", "(FFF)V");
  find(img, g_n.touch, kInput, "nativePassInput", "(IFFIII)V");
  find(img, g_n.tap, kInput, "nativePassTapGesture", "(FF)V");
  find(img, g_n.key, kGL, "nativePassKeyEvent", "(ZIIZ)V");
  find(img, g_n.text, kGL, "nativePassText", "(JLjava/lang/String;ZI)V");
  find(img, g_n.return_pressed, kGL, "nativeReturnPressedFromOnScreenKeyboard", "(J)V");
  find(img, g_n.release_focus, kGL, "nativeReleaseFocus", "(J)V");
  find(img, g_n.sync, kGL, "syncTextboxTextAndCursorPosition2", "(Ljava/lang/String;I)V");
  find(img, g_n.mouse_locked, kInput, "nativeGetMainWindowIsMouseLockedCenter", "()Z");
  find(img, g_n.textbox_info, kGL, "nativeGetTextBoxInfo", "()Lcom/roblox/engine/jni/model/NativeTextBoxInfo;");
  find(img, g_n.pad_connect, kInput, "nativeGamepadConnectEventWithGamepadType", "(II)V");
  find(img, g_n.pad_disconnect, kInput, "nativeGamepadDisconnectEvent", "(I)V");
  find(img, g_n.pad_button, kInput, "nativeGamepadButtonEvent", "(III)V");
  find(img, g_n.pad_axis, kInput, "nativeGamepadAxisEvent", "(IIFFF)V");
  find(img, g_n.pad_key_supported, kInput, "nativeSetGamepadSupportedKeyWithGamepadType", "(IIZI)V");
  find(img, g_n.pad_motion_supported, kInput, "nativeSetGamepadSupportedMotionWithGamepadType", "(IIIZI)V");
  g_enabled = true;
  LOG("enabled (%s); click in the window to use it\n", g_touch ? "left button = touch" : "mouse");
}

namespace {

// --- gamepads ---------------------------------------------------------------------------
//
// SDL's game controller layer gives every pad (Xbox, PlayStation, ...) the
// same layout; Roblox gets it the way the Android app passes InputDevice
// events (mk/e): buttons as KeyEvent keycodes, sticks and triggers as
// MotionEvent axes (the D-pad as the hat axes, as Android reports Xbox pads),
// the pad type from its name (Xbox 3, DualSense 2, DualShock 1).

struct Pad {
  SDL_GameController* gc = nullptr;
  int id = 0;  // the Android device id we use
  int type = 0;
  bool announced = false;
  float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};
std::map<SDL_JoystickID, Pad> g_pads;

int pad_type(SDL_GameController* gc) {
  switch (SDL_GameControllerGetType(gc)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE: return 3;
    case SDL_CONTROLLER_TYPE_PS5: return 2;
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4: return 1;
    default: break;
  }
  std::string n = SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "";
  for (char& c : n) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  if (n.find("XBOX") != std::string::npos) return 3;
  if (n.find("DUALSENSE") != std::string::npos || n.find("PS5") != std::string::npos) return 2;
  if (n.find("DUALSHOCK") != std::string::npos || n.find("PS4") != std::string::npos || n.find("PLAYSTATION") != std::string::npos)
    return 1;
  return 0;
}

// Tells Roblox about the pad (on its first input, as Android's app does).
void announce(Pad& p) {
  if (p.announced || !g_n.pad_connect) return;
  p.announced = true;
  void* e = env();
  void* c = cls(kInput);
  static const int keys[] = {96, 97, 99, 100, 19, 20, 21, 22, 103, 102, 106, 107, 109, 108};  // A B X Y, D-pad, R1 L1, sticks, Select Start
  if (g_n.pad_key_supported)
    for (int k : keys) g_n.pad_key_supported(e, c, p.id, k, 1, p.type);
  if (g_n.pad_motion_supported)
    for (int a : {0, 1, 11, 14, 17, 18, 15, 16, 22, 23}) {
      const u8 has = a != 22 && a != 23;  // (brake/gas: the triggers come as 17/18)
      g_n.pad_motion_supported(e, c, p.id, a, -1, has, p.type);
      if (a == 15 || a == 16) g_n.pad_motion_supported(e, c, p.id, a, 1, has, p.type);
    }
  g_n.pad_connect(e, c, p.id, p.type);
  LOG("gamepad %d connected: %s (type %d)\n", p.id, SDL_GameControllerName(p.gc) ? SDL_GameControllerName(p.gc) : "?", p.type);
}

float stick(Sint16 v) {
  const float f = std::clamp(v / 32767.f, -1.f, 1.f);
  return std::fabs(f) > 0.12f ? f : 0.f;  // dead zone (Android: the axis' flat)
}

void pad_event(const SDL_Event& e) {
  switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED: {
      SDL_GameController* gc = SDL_GameControllerOpen(e.cdevice.which);
      if (!gc) return;
      const SDL_JoystickID jid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
      if (g_pads.count(jid)) return;
      Pad p;
      p.gc = gc;
      p.id = 100 + static_cast<int>(jid);
      p.type = pad_type(gc);
      g_pads[jid] = p;
      LOG("gamepad found: %s\n", SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?");
      return;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {
      auto it = g_pads.find(e.cdevice.which);
      if (it == g_pads.end()) return;
      if (it->second.announced && g_n.pad_disconnect && g_enabled) g_n.pad_disconnect(env(), cls(kInput), it->second.id);
      SDL_GameControllerClose(it->second.gc);
      LOG("gamepad %d disconnected\n", it->second.id);
      g_pads.erase(it);
      return;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
      auto it = g_pads.find(e.cbutton.which);
      if (it == g_pads.end() || !g_enabled) return;
      Pad& p = it->second;
      announce(p);
      const int down = e.type == SDL_CONTROLLERBUTTONDOWN;
      auto hat = [&](int axis, float v) {
        if (g_n.pad_axis) g_n.pad_axis(env(), cls(kInput), p.id, axis, 0, 0, axis == 16 ? -v : v);
      };
      switch (e.cbutton.button) {  // the D-pad: hat axes, as Android sends an Xbox pad's
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return hat(15, down ? -1.f : 0.f);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return hat(15, down ? 1.f : 0.f);
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return hat(16, down ? -1.f : 0.f);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return hat(16, down ? 1.f : 0.f);
        default: break;
      }
      int key = -1;
      switch (e.cbutton.button) {  // KeyEvent.KEYCODE_BUTTON_*
        case SDL_CONTROLLER_BUTTON_A: key = 96; break;
        case SDL_CONTROLLER_BUTTON_B: key = 97; break;
        case SDL_CONTROLLER_BUTTON_X: key = 99; break;
        case SDL_CONTROLLER_BUTTON_Y: key = 100; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: key = 102; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: key = 103; break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: key = 106; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: key = 107; break;
        case SDL_CONTROLLER_BUTTON_BACK: key = 109; break;   // View / Create / Share: Select
        case SDL_CONTROLLER_BUTTON_START: key = 108; break;  // Menu / Options: Start
        case SDL_CONTROLLER_BUTTON_GUIDE: key = 110; break;  // Xbox / PS button: Mode
        default: break;
      }
      if (key >= 0 && g_n.pad_button) g_n.pad_button(env(), cls(kInput), p.id, key, down);
      return;
    }
    case SDL_CONTROLLERAXISMOTION: {
      auto it = g_pads.find(e.caxis.which);
      if (it == g_pads.end() || !g_enabled || !g_n.pad_axis) return;
      Pad& p = it->second;
      announce(p);
      const float v = e.caxis.value;
      void* en = env();
      void* c = cls(kInput);
      switch (e.caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
        case SDL_CONTROLLER_AXIS_LEFTY: {
          (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? p.lx : p.ly) = stick(e.caxis.value);
          const float m = sens().move;  // movement: as before, only reaching full speed sooner (or later)
          const float x = std::clamp(p.lx * m, -1.f, 1.f), y = std::clamp(-p.ly * m, -1.f, 1.f);
          g_n.pad_axis(en, c, p.id, 0, x, y, 0);  // AXIS_X / AXIS_Y, up positive
          g_n.pad_axis(en, c, p.id, 1, x, y, 0);
          break;
        }
        case SDL_CONTROLLER_AXIS_RIGHTX:
        case SDL_CONTROLLER_AXIS_RIGHTY: {
          (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX ? p.rx : p.ry) = stick(e.caxis.value);
          const float m = sens().look;  // camera: turn speed follows the stick, so not held to 1
          g_n.pad_axis(en, c, p.id, 11, p.rx * m, -p.ry * m, 0);  // AXIS_Z / AXIS_RZ
          g_n.pad_axis(en, c, p.id, 14, p.rx * m, -p.ry * m, 0);
          break;
        }
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
          p.lt = std::clamp(v / 32767.f, 0.f, 1.f);
          g_n.pad_axis(en, c, p.id, 17, 0, 0, p.lt);  // AXIS_LTRIGGER
          break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
          p.rt = std::clamp(v / 32767.f, 0.f, 1.f);
          g_n.pad_axis(en, c, p.id, 18, 0, 0, p.rt);  // AXIS_RTRIGGER
          break;
        default: break;
      }
      return;
    }
    default: return;
  }
}

}  // namespace

// make test-gamepad: an SDL virtual gamepad that presses A and pushes the left
// stick up, once input is on (window thread, from window_pump).
void input_test_virtual_pad() {
  static int step = 0;
  static SDL_Joystick* js = nullptr;
  static Uint32 t0 = 0;
  if (step == 0) {
    SDL_VirtualJoystickDesc d{};
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "tonic test pad (Xbox)";
    const int idx = SDL_JoystickAttachVirtualEx(&d);
    js = idx >= 0 ? SDL_JoystickOpen(idx) : nullptr;
    if (!js) LOG("test pad: %s\n", SDL_GetError());
    step = 1;
    return;
  }
  if (!js || !g_enabled) return;
  if (!t0) t0 = SDL_GetTicks();
  const Uint32 dt = SDL_GetTicks() - t0;
  if (step == 1 && dt > 300) SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 1), ++step;
  else if (step == 2 && dt > 600) SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 0), ++step;
  else if (step == 3 && dt > 900) SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTY, -32767), ++step;
  else if (step == 4 && dt > 1200) SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_DPAD_UP, 1), ++step;
}

// Right button held: Roblox turns the camera and keeps its cursor where the
// press was. The desktop's pointer is held there too (relative mode: only
// movement arrives), so on release the cursor does not jump to wherever the
// pointer had wandered.
bool g_dragging = false;
float g_drag_x = 0, g_drag_y = 0;  // the press, in game pixels
int g_drag_wx = 0, g_drag_wy = 0;  // and in window points (to put the pointer back)
Uint32 g_drag_win = 0;

// Shift lock and first person: Roblox locks its cursor in the window's centre.
// As Roblox's Android app does (mk/e: pointer capture while the engine says
// it is locked), the desktop's pointer is held while it is, and put back at
// the centre after, so the cursor does not jump to where it had wandered.
bool g_captured = false;
Uint32 g_capture_win = 0;


bool mouse_locked() { return g_n.mouse_locked && g_n.mouse_locked(env(), cls(kInput)); }

void set_capture(bool on, Uint32 window_id) {
  if (on == g_captured) return;
  g_captured = on;
  SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
  if (on) g_capture_win = window_id;
  if (!g_dragging) SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
  if (!on)
    if (SDL_Window* w = SDL_GetWindowFromID(g_capture_win)) {
      int ww = 0, wh = 0;
      SDL_GetWindowSize(w, &ww, &wh);
      SDL_WarpMouseInWindow(w, ww / 2, wh / 2);  // where Roblox's cursor was
    }
}

void input_event(const SDL_Event& e) {
  switch (e.type) {  // you, at the controls (anti-AFK waits for these to stop)
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
    case SDL_CONTROLLERAXISMOTION: case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
      g_last_input = SDL_GetTicks();
      break;
  }
  if (e.type >= SDL_CONTROLLERAXISMOTION && e.type <= SDL_CONTROLLERDEVICEREMAPPED) return pad_event(e);
  if (!g_enabled) return;
  switch (e.type) {
    case SDL_MOUSEMOTION: {
      if (!g_touch) {  // shift lock / first person: held at the centre, only movement
        const bool lock = mouse_locked();
        if (lock != g_captured) set_capture(lock, e.motion.windowID);
        if (g_captured && !g_dragging) {
          int w = 0, h = 0;
          pixel_size(e.motion.windowID, w, h);
          float dx, dy;
          scaled(e.motion.windowID, static_cast<float>(e.motion.xrel), static_cast<float>(e.motion.yrel), dx, dy);
          const float m = sens().mouse;
          g_x = w / 2.f, g_y = h / 2.f;
          if (g_n.mouse_move) g_n.mouse_move(env(), cls(kInput), g_x, g_y, dx * m, dy * m);
          break;
        }
      }
      if (g_dragging) {  // only the movement: the cursor stays at the press
        float dx, dy;
        scaled(e.motion.windowID, static_cast<float>(e.motion.xrel), static_cast<float>(e.motion.yrel), dx, dy);
        const float m = sens().mouse;
        if (g_n.mouse_move) g_n.mouse_move(env(), cls(kInput), g_drag_x, g_drag_y, dx * m, dy * m);
        break;
      }
      float x, y;
      scaled(e.motion.windowID, static_cast<float>(e.motion.x), static_cast<float>(e.motion.y), x, y);
      const float dx = x - g_x, dy = y - g_y;
      g_x = x, g_y = y;
      if (g_touch) {
        if (g_finger_down && g_n.touch) {
          int w, h;
          pixel_size(e.motion.windowID, w, h);
          g_n.touch(env(), cls(kInput), 0, x, y, 1, w, h);
        }
      } else if (g_n.mouse_move) {
        const float m = sens().mouse;  // the camera turns by the movement (dx, dy)
        g_n.mouse_move(env(), cls(kInput), x, y, dx * m, dy * m);
      }
      break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
      const bool down = e.type == SDL_MOUSEBUTTONDOWN;
      float x, y;
      scaled(e.button.windowID, static_cast<float>(e.button.x), static_cast<float>(e.button.y), x, y);
      g_x = x, g_y = y;
      LOG("button %d %s at %.0f,%.0f\n", e.button.button, down ? "down" : "up", x, y);
      if (g_touch && e.button.button == SDL_BUTTON_LEFT) {
        int w, h;
        pixel_size(e.button.windowID, w, h);
        if (g_n.touch) g_n.touch(env(), cls(kInput), 0, x, y, down ? 0 : 2, w, h);
        if (down) {
          g_finger_down = true, g_down_x = x, g_down_y = y;
        } else {
          g_finger_down = false;
          if (g_n.tap && std::hypot(x - g_down_x, y - g_down_y) < 10) g_n.tap(env(), cls(kInput), x, y);
        }
        break;
      }
      // Android's MotionEvent.getActionButton() - 1: primary 0, secondary 1,
      // tertiary (middle) 3.
      int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
            : e.button.button == SDL_BUTTON_MIDDLE ? 3 : -1;
      if (g_captured) x = g_x, y = g_y;  // shift lock: clicks at the centre, where Roblox's cursor is
      if (b == 1 && down && !g_dragging) {
        g_dragging = true;
        g_drag_x = x, g_drag_y = y, g_drag_wx = e.button.x, g_drag_wy = e.button.y, g_drag_win = e.button.windowID;
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
        SDL_SetRelativeMouseMode(SDL_TRUE);
      } else if (b == 1 && !down && g_dragging) {
        g_dragging = false;
        x = g_drag_x, y = g_drag_y, g_x = x, g_y = y;
        if (!g_captured) {  // (shift lock keeps holding it)
          SDL_SetRelativeMouseMode(SDL_FALSE);
          if (SDL_Window* w = SDL_GetWindowFromID(g_drag_win)) SDL_WarpMouseInWindow(w, g_drag_wx, g_drag_wy);
        }
      }
      if (b >= 0 && g_n.mouse_button) g_n.mouse_button(env(), cls(kInput), x, y, down, b);
      break;
    }
    case SDL_WINDOWEVENT:
      // Focus gone mid-drag (Alt+Tab): let go of the pointer, and of the button in Roblox.
      if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && (g_dragging || g_captured)) {
        if (g_dragging && g_n.mouse_button) g_n.mouse_button(env(), cls(kInput), g_drag_x, g_drag_y, false, 1);
        g_dragging = false;
        g_captured = false;  // (taken again on the next movement, if still locked)
        SDL_SetRelativeMouseMode(SDL_FALSE);
      }
      break;
    case SDL_MOUSEWHEEL:
      // AXIS_VSCROLL: positive is away from the user, like SDL's y.
      if (g_n.mouse_wheel && e.wheel.y)
        g_n.mouse_wheel(env(), cls(kInput), g_x > 0 ? g_x : 0, g_y > 0 ? g_y : 0, static_cast<float>(e.wheel.y));
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      static bool shown = false;  // which keys is never logged: it could spell a password
      if (!shown) shown = true, LOG("key events are arriving\n");
      if (text_key(e.key)) break;
      const KeyMap* k = key_for(e.key.keysym.scancode);
      if (k && g_n.key) g_n.key(env(), cls(kGL), e.type == SDL_KEYDOWN, k->linux_, k->android, e.key.repeat != 0);
      break;
    }
    case SDL_TEXTINPUT: {
      static bool shown = false;
      if (!shown) shown = true, LOG("text input is arriving\n");
      {
        std::lock_guard<std::mutex> l(g_mu);
        if (!g_textbox) break;
        g_text += e.text.text;
      }
      send_text(false);
      break;
    }
    default:
      break;
  }
}

// Caffeinate, anti-AFK (Settings > Controls, off by default; config "anti_afk"). Roblox disconnects a player
// after 20 minutes without input. After a few minutes with none from you,
// tonic moves the mouse one pixel and straight back, through the same native
// your mouse uses: the cursor and the camera end up exactly where they were,
// and no button or key is ever pressed. Again every few minutes while you are
// away; nothing while you play. TONIC_AFK_SECS=N: after N seconds instead (to
// try it out).
void input_tick() {
  if (!g_enabled || !g_n.mouse_move) return;
  static bool on = false;
  static Uint32 checked = 0, after_ms = 0;
  const Uint32 now = SDL_GetTicks();
  if (!checked || now - checked > 1000) {
    checked = now;
    on = config_bool("anti_afk", false);
    const char* e = getenv("TONIC_AFK_SECS");
    after_ms = (e && atoi(e) >= 5 ? static_cast<Uint32>(atoi(e)) : 300u) * 1000u;
  }
  if (!on) return;
  if (!g_last_input) g_last_input = now;  // (counted from when the game starts taking input)
  const Uint32 since = std::max(g_last_input, g_last_nudge);
  if (now - since < after_ms) return;
  if (g_dragging || g_finger_down) return;  // (a button held: you are there)
  g_last_nudge = now;
  void* e = env();
  void* c = cls(kInput);
  g_n.mouse_move(e, c, g_x + 1, g_y, 1, 0);
  g_n.mouse_move(e, c, g_x, g_y, -1, 0);
  const Uint32 away_s = (now - g_last_input) / 1000;
  if (away_s < 120) LOG("caffeinate: no input for %u s, mouse nudged one pixel and back\n", away_s);
  else LOG("caffeinate: no input for %u min, mouse nudged one pixel and back\n", away_s / 60);
}

}  // namespace tonic
