#include "config.hpp"
#include "menu.hpp"
#include "profile.hpp"
#include "textoverlay.hpp"
#include "window.hpp"
#include "appicon.hpp"

#include <condition_variable>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <SDL.h>
#include <SDL_syswm.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>
#include <png.h>
#include <ctime>
#include <thread>
#include <memory>
#include <filesystem>

#include "fakejni.hpp"
#include "audio.hpp"
#include "closescreen.hpp"
#include "overlaywin.hpp"
#include "game.hpp"
#include "gamemode.hpp"
#include "input.hpp"
#include "mainloop.hpp"
#include "shims.hpp"

namespace tonic {

namespace {

// EGL types and constants, declared here so we need no EGL headers.
using EGLint = int32_t;
using EGLBoolean = unsigned;
using EGLenum = unsigned;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLSurface = void*;
constexpr EGLint kEglNone = 0x3038;
constexpr EGLenum kPlatformX11 = 0x31D5;      // EGL_PLATFORM_X11_KHR
constexpr EGLenum kPlatformWayland = 0x31D8;  // EGL_PLATFORM_WAYLAND_KHR

#define LOG(fmt, ...) fprintf(stderr, "[window] " fmt, ##__VA_ARGS__)

// What the library holds as an ANativeWindow*. Opaque to it: it only passes
// the pointer to ANativeWindow_* and EGL, which are ours.
struct NativeWindow {
  uint32_t magic = 0x5f776e64;  // "_wnd"
  std::atomic<int> refs{0};
};
NativeWindow g_native;

SDL_Window* g_sdl = nullptr;
bool g_wayland = false;
void* g_host_display = nullptr;  // Display* or wl_display*
void* g_host_window = nullptr;   // X11 Window id (as a pointer) or wl_egl_window*
void* g_wl_surface = nullptr;    // Wayland: the window's wl_surface (Vulkan draws to it)
std::atomic<int> g_w{0}, g_h{0};    // the window, in pixels
std::atomic<int> g_rw{0}, g_rh{0};  // what the game draws at: the window, or less with a resolution cap
void* g_surface = nullptr;          // the game's EGL surface on our window

// Resolution cap (config "res_cap": the most pixels the game draws in height;
// 0 none): the game is told a smaller window, draws that, and each frame is
// scaled up to the real window (textoverlay_upscale). Resizing works as usual.
std::atomic<bool> g_cap_off{false};  // the upscale failed: no cap for this run
std::atomic<int> g_wmm{0}, g_hmm{0};  // the window's physical size, as last told to the game
void game_surface_changed_again() { game_surface_changed(g_wmm, g_hmm); }
void compute_render_size() {
  const int w = g_w, h = g_h, cap = g_cap_off ? 0 : config_int("res_cap", 0);
  if (cap > 0 && h > cap) {
    g_rh = cap;
    g_rw = std::max(1, static_cast<int>(static_cast<long long>(w) * cap / h));
  } else {
    g_rw = w;
    g_rh = h;
  }
}

void* (*wl_egl_window_create_fn)(void*, int, int) = nullptr;
void (*wl_egl_window_resize_fn)(void*, int, int, int, int) = nullptr;

template <typename T>
T host(const char* name) {
  return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

// Android-only EGL attributes Mesa does not know. Passing them makes
// eglChooseConfig/eglCreateWindowSurface fail with EGL_BAD_ATTRIBUTE.
bool android_only(EGLint key) {
  return (key >= 0x3140 && key <= 0x314F) ||  // EGL_RECORDABLE_ANDROID, EGL_FRAMEBUFFER_TARGET_ANDROID, ...
         (key >= 0x3430 && key <= 0x343F);    // EGL_TIMESTAMPS_ANDROID and friends
}

std::vector<EGLint> filter_attribs(const EGLint* in, const char* who) {
  std::vector<EGLint> out;
  if (in)
    for (; in[0] != kEglNone; in += 2) {
      if (android_only(in[0])) {
        LOG("%s: dropping Android-only attribute %#x = %d\n", who, in[0], in[1]);
        continue;
      }
      out.push_back(in[0]);
      out.push_back(in[1]);
    }
  out.push_back(kEglNone);
  return out;
}

// --- ANativeWindow ----------------------------------------------------------

void* sh_ANativeWindow_fromSurface(void* env, void* surface) {
  (void)env;
  const std::string what = jni_describe(surface);
  if (!g_sdl) {
    LOG("ANativeWindow_fromSurface(%s): no window open (run with --window) -> null\n", what.c_str());
    return nullptr;
  }
  ++g_native.refs;
  LOG("ANativeWindow_fromSurface(%s) -> our window, %dx%d\n", what.c_str(), g_w.load(), g_h.load());
  return &g_native;
}
void sh_ANativeWindow_acquire(void* w) {
  if (w == &g_native) ++g_native.refs;
}
void sh_ANativeWindow_release(void* w) {
  if (w == &g_native) --g_native.refs;
}
int32_t sh_ANativeWindow_getWidth(void* w) { return w == &g_native ? g_rw.load() : -22; }  // -EINVAL
int32_t sh_ANativeWindow_getHeight(void* w) { return w == &g_native ? g_rh.load() : -22; }

// --- EGL --------------------------------------------------------------------

EGLDisplay sh_eglGetDisplay(void* native) {
  static auto get_display = host<EGLDisplay (*)(void*)>("eglGetDisplay");
  static auto get_platform = host<EGLDisplay (*)(EGLenum, void*, const intptr_t*)>("eglGetPlatformDisplay");
  if (!native && g_sdl && get_platform) {  // EGL_DEFAULT_DISPLAY
    EGLDisplay d = get_platform(g_wayland ? kPlatformWayland : kPlatformX11, g_host_display, nullptr);
    LOG("eglGetDisplay(EGL_DEFAULT_DISPLAY) -> %s display %p\n", g_wayland ? "Wayland" : "X11", d);
    return d;
  }
  return get_display(native);
}

EGLBoolean sh_eglChooseConfig(EGLDisplay d, const EGLint* attribs, EGLConfig* configs, EGLint size, EGLint* n) {
  static auto real = host<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>("eglChooseConfig");
  const std::vector<EGLint> a = filter_attribs(attribs, "eglChooseConfig");
  const EGLBoolean r = real(d, a.data(), configs, size, n);
  LOG("eglChooseConfig -> %s, %d config(s)\n", r ? "ok" : "FAILED", n ? *n : -1);
  return r;
}

EGLSurface sh_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void* win, const EGLint* attribs) {
  static auto real = host<EGLSurface (*)(EGLDisplay, EGLConfig, void*, const EGLint*)>("eglCreateWindowSurface");
  static auto get_error = host<EGLint (*)()>("eglGetError");
  if (win != &g_native) {
    LOG("eglCreateWindowSurface on a window that is not ours (%p)\n", win);
    return real(d, c, win, attribs);
  }
  const std::vector<EGLint> a = filter_attribs(attribs, "eglCreateWindowSurface");
  EGLSurface s = real(d, c, g_host_window, a.data());
  if (s) LOG("eglCreateWindowSurface -> %p (%dx%d, game draws %dx%d)\n", s, g_w.load(), g_h.load(), g_rw.load(), g_rh.load());
  if (s) g_surface = s;
  else LOG("eglCreateWindowSurface FAILED, eglGetError %#x\n", get_error ? get_error() : 0);
  return s;
}

// Counts presented frames so a run shows whether the engine draws at all. On
// Wayland the window only appears once the first frame is swapped.
std::atomic<long> g_frames{0};

// The swap interval to use: TONIC_SWAP_INTERVAL, else the menu's VSync
// setting (default off on Wayland, where there is no tearing either way, and
// on elsewhere), else ENGINE's own choice (-1: none yet).
int swap_interval_wanted(int engine) {
  const char* force = getenv("TONIC_SWAP_INTERVAL");
  if (force && *force) return atoi(force);
  const std::string v = config_get("vsync", "");
  if (v == "1") return 1;
  if (v == "0") return 0;
  return g_wayland ? 0 : engine;
}

// While the game window is being shown (the loading window closing), the
// game's next frame waits here: on Wayland a window must not get a frame
// between being given its role and the compositor's first configure, or the
// compositor drops the window (nothing appeared).
std::mutex g_hold_mu;
std::condition_variable g_hold_cv;
bool g_hold = false, g_parked = false;

void swap_gate() {
  std::unique_lock<std::mutex> l(g_hold_mu);
  if (!g_hold) return;
  g_parked = true;
  g_hold_cv.notify_all();
  g_hold_cv.wait(l, [] { return !g_hold; });
  g_parked = false;
}

// --- mod panel extras ---------------------------------------------------------------

std::atomic<bool> g_shot{false};          // F12 pressed: save the next frame
std::atomic<bool> g_focused{true};        // one of tonic's windows has the keyboard
std::atomic<bool> g_game_shown{false};    // the game window is up (the launcher closed)
std::mutex g_shot_mu;
std::string g_shot_last;

// RGBA rows, bottom row first when BOTTOM_UP (GL), to Pictures/tonic.
void save_png_async(std::shared_ptr<std::vector<uint8_t>> px, int w, int h, bool bottom_up) {
  std::thread([px, w, h, bottom_up] {  // PNG writing off the render thread
    const char* home = getenv("HOME");
    const std::string dir = std::string(home ? home : ".") + "/Pictures/tonic";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[64];
    const time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    strftime(name, sizeof name, "tonic-%Y%m%d-%H%M%S.png", &lt);
    const std::string path = dir + "/" + name;
    for (size_t i = 3; i < px->size(); i += 4) (*px)[i] = 255;  // the window's alpha is not the picture's
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = w, img.height = h, img.format = PNG_FORMAT_RGBA;
    // Rows bottom up (GL): a negative stride tells libpng so (the buffer
    // pointer stays at the start of the memory).
    const bool ok = png_image_write_to_file(&img, path.c_str(), 0, px->data(), bottom_up ? -w * 4 : w * 4, nullptr);
    LOG("screenshot %s %s\n", ok ? "saved:" : "failed:", path.c_str());
    std::lock_guard<std::mutex> l(g_shot_mu);
    g_shot_last = ok ? path : "";
  }).detach();
}

void save_screenshot() {
  const int w = g_w, h = g_h;
  auto px = std::make_shared<std::vector<uint8_t>>();
  if (!textoverlay_read_frame(w, h, *px)) {
    LOG("screenshot failed\n");
    return;
  }
  save_png_async(px, w, h, true);
}

// Background frame rate (config mod_bg_fps): while none of tonic's windows
// has the keyboard, frames are spaced out to save power and heat.
void background_pace() {
  static auto last = std::chrono::steady_clock::now();
  static int fps = 0;
  static auto checked = last;
  auto now = std::chrono::steady_clock::now();
  if (now - checked > std::chrono::milliseconds(250)) {
    checked = now;
    fps = config_int("mod_bg_fps", 0);
  }
  if (!g_focused && fps > 0) {
    const auto next = last + std::chrono::microseconds(1000000 / fps);
    if (next > now) std::this_thread::sleep_until(next);
    now = std::chrono::steady_clock::now();
  }
  last = now;
}

#if defined(__FreeBSD__) || defined(__OpenBSD__)
// TONIC_STATS=1 (FreeBSD): where each frame's time goes, every 5 seconds:
// the whole frame, tonic's overlay, and presenting it (Mesa copying the
// finished picture to the X server).
struct SwapStats {
  bool on = [] { const char* e = getenv("TONIC_STATS"); return e && *e && *e != '0'; }();
  std::chrono::steady_clock::time_point last{}, t0{};
  double frame = 0, frame_worst = 0, overlay = 0, present = 0, present_worst = 0;
  long n = 0;
  void add(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b,
           std::chrono::steady_clock::time_point c) {
    using ms = std::chrono::duration<double, std::milli>;
    if (last.time_since_epoch().count()) {
      const double f = ms(c - last).count();
      frame += f;
      frame_worst = std::max(frame_worst, f);
    } else {
      t0 = c;
    }
    last = c;
    overlay += ms(b - a).count();
    const double p = ms(c - b).count();
    present += p;
    present_worst = std::max(present_worst, p);
    ++n;
    if (c - t0 >= std::chrono::seconds(5) && n > 1) {
      LOG("frame time %.1f ms on average (worst %.1f): tonic's overlay %.2f ms, presenting %.2f ms (worst %.1f)\n",
          frame / (n - 1), frame_worst, overlay / n, present / n, present_worst);
      frame = frame_worst = overlay = present = present_worst = 0;
      n = 0;
      t0 = c;
      last = {};
    }
  }
} g_swap_stats;
#endif

EGLBoolean sh_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  static auto real = host<EGLBoolean (*)(EGLDisplay, EGLSurface)>("eglSwapBuffers");
  swap_gate();
  // Wayland: no waiting on the compositor between frames (see eglSwapInterval
  // below), from the first frame on, also when the game never sets an
  // interval. A hidden window gets no frame callbacks; waiting would stall.
  // VSync from the menu (config "vsync"), applied whenever it changes.
  static int applied = -1;
  const int want = swap_interval_wanted(-1);
  if (want >= 0 && want != applied) {
    applied = want;
    if (auto si = host<EGLBoolean (*)(EGLDisplay, EGLint)>("eglSwapInterval")) si(d, want);
  }
  if (s == g_surface && (g_rw != g_w || g_rh != g_h || textoverlay_postfx_on()) && !textoverlay_upscale(g_rw, g_rh, g_w, g_h) &&
      !g_cap_off) {
    g_cap_off = true;  // the game goes back to drawing the whole window
    compute_render_size();
    main_post([] { game_surface_changed_again(); });
  }
  if (s == g_surface && g_shot.exchange(false)) save_screenshot();  // F12: the picture as you see it, without tonic's overlays
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  const auto ta = std::chrono::steady_clock::now();
#endif
  textoverlay_draw(g_w, g_h, static_cast<float>(g_w) / std::max(1, g_rw.load()));  // typed text, stats
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  const auto tb = std::chrono::steady_clock::now();
#endif
  const EGLBoolean r = real(d, s);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  if (g_swap_stats.on && s == g_surface) g_swap_stats.add(ta, tb, std::chrono::steady_clock::now());
#endif
  if (!r) {
    static auto get_error = host<EGLint (*)()>("eglGetError");
    static int failures = 0;
    if (failures++ < 5) LOG("eglSwapBuffers failed, error %#x\n", get_error ? get_error() : 0);
  }
  if (s == g_surface) background_pace();
  frame_done(r != 0);
  return r;
}

}  // namespace

#if defined(__FreeBSD__) || defined(__OpenBSD__)
namespace {
// TONIC_FPS_AVG=1 (FreeBSD): the average frame rate in a game, over 3 minutes
// (or TONIC_FPS_AVG=<seconds>). Measuring starts 30 s after a game is
// launched, so loading is left out, and repeats until tonic closes. Each result
// goes to the log and is added to fps-results.txt in the folder tonic was
// started from, with the settings that change speed, to compare runs.
struct FpsAverage {
  int secs = [] {
    const char* e = getenv("TONIC_FPS_AVG");
    if (!e || !*e || *e == '0') return 0;
    const int v = atoi(e);
    return v > 1 ? v : 180;
  }();
  int launches = 0;
  std::chrono::steady_clock::time_point launch{}, start{}, last{};
  std::vector<float> times;  // frame times, ms
  void frame() {
    if (!secs) return;
    const auto now = std::chrono::steady_clock::now();
    if (const int l = game_launches(); l != launches) {  // a (new) game: start over
      launches = l;
      launch = now;
      start = {};
      times.clear();
      LOG("fps average: measuring starts in 30 s, for %d s (stay in the game)\n", secs);
      return;
    }
    if (!launches || now - launch < std::chrono::seconds(30)) return;
    if (start.time_since_epoch().count() == 0) {
      start = last = now;
      times.reserve(static_cast<size_t>(secs) * 70);
      LOG("fps average: measuring now\n");
      return;
    }
    times.push_back(std::chrono::duration<float, std::milli>(now - last).count());
    last = now;
    const double total = std::chrono::duration<double>(now - start).count();
    if (total < secs) return;
    std::vector<float> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]; };
    const double avg = times.size() / total;
    const double low1 = 1000.0 / pct(0.99), low01 = 1000.0 / pct(0.999);
    const double worst = sorted.back();
    char settings[256];
    const char* gt = getenv("mesa_glthread");
    const char* ck = getenv("TONIC_CLOCK");
    const char* lp = getenv("LP_NUM_THREADS");
    snprintf(settings, sizeof settings, "mesa_glthread=%s TONIC_CLOCK=%s LP_NUM_THREADS=%s window %dx%d draws %dx%d",
             gt ? gt : "-", ck ? ck : "-", lp ? lp : "-", g_w.load(), g_h.load(), g_rw.load(), g_rh.load());
    LOG("fps average over %.0f s: %.1f fps, 1%% low %.1f, 0.1%% low %.1f, worst frame %.0f ms (%s)\n", total, avg, low1,
        low01, worst, settings);
    if (FILE* f = fopen("fps-results.txt", "a")) {
      const std::time_t t = std::time(nullptr);
      char when[32];
      std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&t));
      fprintf(f, "%s  %5.1f fps avg  %5.1f 1%%-low  %5.1f 0.1%%-low  worst %4.0f ms  over %.0f s  %s\n", when, avg, low1,
              low01, worst, total, settings);
      fclose(f);
    }
    start = last = now;  // and the next 3 minutes
    times.clear();
  }
} g_fps_avg;
}  // namespace
#endif

// After every frame the game presents (OpenGL or Vulkan): counts, logs.
void frame_done(bool ok) {
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  g_fps_avg.frame();
#endif
  const long n = ++g_frames;
  if (n == 1 || n == 60) LOG("frame %ld presented%s\n", n, ok ? "" : " (FAILED)");
  if (n == 50 && getenv("TONIC_TEST_SHOT")) g_shot = true;  // make test-upscale: a screenshot of a scaled-up frame
  // Frames per second, every 5 seconds when it changes by more than 5%.
  static auto t0 = std::chrono::steady_clock::now();
  static long n0 = n;
  static double last_fps = 0;
  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - t0).count();
  if (secs >= 5) {
    const double fps = (n - n0) / secs;
    if (std::fabs(fps - last_fps) > last_fps * 0.05)
      LOG("%.0f frames per second (%zu Java objects)\n", fps, jni_object_count());
    last_fps = fps;
    t0 = now;
    n0 = n;
  }
}

namespace {

// The game asks its surface's size: with a resolution cap, the size it draws.
EGLBoolean sh_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint* value) {
  static auto real = host<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*)>("eglQuerySurface");
  const EGLBoolean r = real(d, s, attr, value);
  if (r && value && s == g_surface) {
    if (attr == 0x3057) *value = g_rw;  // EGL_WIDTH
    if (attr == 0x3056) *value = g_rh;  // EGL_HEIGHT
  }
  return r;
}

// The engine picks its own swap interval (1 = wait for every vblank, 2 = every
// other one, 0 = do not wait). On Wayland 0 is used instead: the compositor
// still shows only whole frames (no tearing), and waiting on each vblank as
// well as the engine's own frame pacing held 200 Hz screens to ~100 fps.
// Logged when it changes; TONIC_SWAP_INTERVAL=N decides instead.
EGLBoolean sh_eglSwapInterval(EGLDisplay d, EGLint interval) {
  static auto real = host<EGLBoolean (*)(EGLDisplay, EGLint)>("eglSwapInterval");
  static int last = -100;
  const char* force = getenv("TONIC_SWAP_INTERVAL");
  const EGLint use = swap_interval_wanted(interval);
  if (interval != last) {
    last = interval;
    if (use != interval)
      LOG("eglSwapInterval(%d) from the engine, using %d (%s)\n", interval, use,
          force && *force ? "TONIC_SWAP_INTERVAL" : "Wayland: no tearing either way");
    else LOG("eglSwapInterval(%d) from the engine\n", interval);
  }
  return real(d, use);
}

// Physical size in mm of PW x PH pixels (W x H window points): the monitor's
// DPI when SDL knows a believable one, else 96 dpi per window point.
void physical_mm(int pw, int ph, int w, int h, int& wmm, int& hmm, float& hdpi, float& vdpi) {
  hdpi = vdpi = 0;
  const int disp = SDL_GetWindowDisplayIndex(g_sdl);
  if (disp >= 0 && SDL_GetDisplayDPI(disp, nullptr, &hdpi, &vdpi) == 0 && hdpi > 50 && hdpi < 400 && vdpi > 50 &&
      vdpi < 400) {
    wmm = static_cast<int>(pw / hdpi * 25.4f + 0.5f);
    hmm = static_cast<int>(ph / vdpi * 25.4f + 0.5f);
  } else {
    wmm = static_cast<int>(w / 96.f * 25.4f + 0.5f);
    hmm = static_cast<int>(h / 96.f * 25.4f + 0.5f);
  }
}

// A resize is passed on like Android's surfaceChanged: once the size settles
// (one job pending at a time, it reads the latest size), on the main loop.
std::atomic<bool> g_resize_pending{false};

void update_size() {
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_sdl, &w, &h);
  if (w == g_w && h == g_h) return;
  g_w = w;
  g_h = h;
  compute_render_size();
  if (g_wayland && g_host_window && wl_egl_window_resize_fn) wl_egl_window_resize_fn(g_host_window, w, h, 0, 0);
  int ww = 0, wh = 0, wmm, hmm;
  float hd, vd;
  SDL_GetWindowSize(g_sdl, &ww, &wh);
  physical_mm(w, h, ww, wh, wmm, hmm, hd, vd);
  g_wmm = wmm, g_hmm = hmm;
  if (g_resize_pending.exchange(true)) return;
  main_post([wmm, hmm] {
    g_resize_pending = false;
    LOG("resized to %d x %d pixels (%d x %d mm)%s\n", g_w.load(), g_h.load(), wmm, hmm,
        g_rw != g_w ? (", game draws " + std::to_string(g_rw.load()) + "x" + std::to_string(g_rh.load())).c_str() : "");
    game_surface_changed(wmm, hmm);
  });
}

}  // namespace

void register_window_shims() {
  register_shim("ANativeWindow_fromSurface", reinterpret_cast<void*>(&sh_ANativeWindow_fromSurface));
  register_shim("ANativeWindow_acquire", reinterpret_cast<void*>(&sh_ANativeWindow_acquire));
  register_shim("ANativeWindow_release", reinterpret_cast<void*>(&sh_ANativeWindow_release));
  register_shim("ANativeWindow_getWidth", reinterpret_cast<void*>(&sh_ANativeWindow_getWidth));
  register_shim("ANativeWindow_getHeight", reinterpret_cast<void*>(&sh_ANativeWindow_getHeight));
  register_shim("eglGetDisplay", reinterpret_cast<void*>(&sh_eglGetDisplay));
  register_shim("eglChooseConfig", reinterpret_cast<void*>(&sh_eglChooseConfig));
  register_shim("eglCreateWindowSurface", reinterpret_cast<void*>(&sh_eglCreateWindowSurface));
  register_shim("eglSwapBuffers", reinterpret_cast<void*>(&sh_eglSwapBuffers));
  register_shim("eglSwapInterval", reinterpret_cast<void*>(&sh_eglSwapInterval));
  register_shim("eglQuerySurface", reinterpret_cast<void*>(&sh_eglQuerySurface));
}


// --- for the Vulkan presenter (vkshim.cpp) ---------------------------------------------

void window_present_gate() { swap_gate(); }
void window_present_pace() { background_pace(); }
bool window_take_shot() { return g_shot.exchange(false); }
void window_save_shot(std::shared_ptr<std::vector<uint8_t>> rgba, int w, int h) { save_png_async(rgba, w, h, false); }
void window_vk_target(bool& wayland, void*& display, void*& surface) {
  wayland = g_wayland;
  display = g_host_display;
  surface = g_wayland ? g_wl_surface : g_host_window;
}
void window_size(int& w, int& h) {
  w = g_w;
  h = g_h;
}
int window_vsync() { return swap_interval_wanted(0) > 0; }
void window_set_vulkan() {}  // (the resolution cap works with Vulkan too: vkshim scales the frames)
void window_render_size(int& w, int& h) {
  w = g_rw;
  h = g_rh;
}
void window_cap_unsupported() {
  if (g_cap_off.exchange(true)) return;
  compute_render_size();
  main_post([] { game_surface_changed_again(); });
}

bool window_focused() { return g_focused; }

void window_render_scale(float& sx, float& sy) {
  sx = g_w > 0 ? static_cast<float>(g_rw) / g_w : 1.f;
  sy = g_h > 0 ? static_cast<float>(g_rh) / g_h : 1.f;
}

// The game window appears (the menu closed). Main thread.
void window_show_game() {
  {
    // Hold the game's frames while the window gets its role and first
    // configure: on Wayland a frame in between makes the compositor drop it.
    std::unique_lock<std::mutex> l(g_hold_mu);
    g_hold = true;
    g_hold_cv.wait_for(l, std::chrono::milliseconds(500), [] { return g_parked; });
  }
  SDL_ShowWindow(g_sdl);
  SDL_RaiseWindow(g_sdl);
  SDL_PumpEvents();
  {
    std::lock_guard<std::mutex> l(g_hold_mu);
    g_hold = false;
  }
  g_hold_cv.notify_all();
  if (config_bool("fullscreen", false)) SDL_SetWindowFullscreen(g_sdl, SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!getenv("TONIC_SYSTEM_CURSOR")) SDL_ShowCursor(SDL_DISABLE);
  update_size();
  g_game_shown = true;
  LOG("game window shown\n");
}

bool window_open(int w, int h, std::string& err) {
  // The desktop's name for tonic's windows (Hyprland / KDE window rules: class "tonic").
  SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "tonic");
  SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "tonic");
  SDL_SetHint("SDL_APP_ID", "tonic");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    err = std::string("SDL_Init: ") + SDL_GetError();
    return false;
  }
  // Gamepads (input.cpp): Xbox, PlayStation and others, through SDL's mapping.
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) LOG("no gamepad support: %s\n", SDL_GetError());
  // No SDL_WINDOW_OPENGL: the library makes its own EGL context and surface;
  // SDL only provides the window.
  const bool loading = !getenv("TONIC_NO_LOADING_SCREEN");
  g_sdl = SDL_CreateWindow(profile_title().c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (loading ? SDL_WINDOW_HIDDEN : 0));
  if (!g_sdl) {
    err = std::string("SDL_CreateWindow: ") + SDL_GetError();
    return false;
  }
  app_icon_set(g_sdl);
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(g_sdl, &info)) {
    err = std::string("SDL_GetWindowWMInfo: ") + SDL_GetError();
    return false;
  }
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(g_sdl, &pw, &ph);
  g_w = pw;
  g_h = ph;
  compute_render_size();
  if (g_rw != g_w) LOG("resolution cap: the game draws %dx%d, scaled up to the window\n", g_rw.load(), g_h.load() > 0 ? g_rh.load() : 0);
  if (info.subsystem == SDL_SYSWM_X11) {
    g_host_display = info.info.x11.display;
    g_host_window = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
  } else if (info.subsystem == SDL_SYSWM_WAYLAND) {
    g_wayland = true;
    g_host_display = info.info.wl.display;
    g_wl_surface = info.info.wl.surface;
    overlaywin_opaque(g_host_display, g_wl_surface);
    void* lib = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (lib) {
      wl_egl_window_create_fn = reinterpret_cast<void* (*)(void*, int, int)>(dlsym(lib, "wl_egl_window_create"));
      wl_egl_window_resize_fn =
          reinterpret_cast<void (*)(void*, int, int, int, int)>(dlsym(lib, "wl_egl_window_resize"));
    }
    if (!wl_egl_window_create_fn) {
      err = "libwayland-egl.so.1 not found (or run with SDL_VIDEODRIVER=x11)";
      return false;
    }
    g_host_window = wl_egl_window_create_fn(info.info.wl.surface, pw, ph);
  } else {
    err = "unsupported SDL video driver " + std::string(SDL_GetCurrentVideoDriver());
    return false;
  }
  LOG("opened %dx%d (%d x %d pixels) on %s\n", w, h, pw, ph, SDL_GetCurrentVideoDriver());
  // Typed characters arrive as SDL_TEXTINPUT only while text input is on.
  // SDL2 turned it on by default; SDL3 (and sdl2-compat on top of it) does not.
  SDL_StartTextInput();
  // The game draws its own mouse cursor; hide the desktop's over the window
  // (TONIC_SYSTEM_CURSOR=1 keeps it).
  if (!getenv("TONIC_SYSTEM_CURSOR")) SDL_ShowCursor(SDL_DISABLE);
  if (loading && !menu_open(w, h)) SDL_ShowWindow(g_sdl);

  // Android's Java side tells the engine the screen's physical size in mm
  // (PlatformParams.viewportWidthMm/HeightMm, from DisplayMetrics); left at 0
  // the engine is told the screen has no size. Use the monitor's DPI when SDL
  // knows a believable one, else 96 dpi per window point.
  float hdpi = 0, vdpi = 0;
  int wmm, hmm;
  physical_mm(pw, ph, w, h, wmm, hmm, hdpi, vdpi);
  g_wmm = wmm, g_hmm = hmm;
  set_java_value_default("com/roblox/engine/jni/model/PlatformParams.viewportWidthMm:I", wmm);
  set_java_value_default("com/roblox/engine/jni/model/PlatformParams.viewportHeightMm:I", hmm);
  LOG("physical size %d x %d mm (monitor dpi %.0f x %.0f%s)\n", wmm, hmm, hdpi, vdpi,
      hdpi > 50 && hdpi < 400 ? "" : ", not usable: assuming 96");
  return true;
}

bool window_is_open() { return g_sdl != nullptr; }

// The desktop's cursor: hidden only over the game (Roblox draws its own
// there), shown over the launcher and the mod panel. Decided each time the
// pointer enters one of tonic's windows; SDL's cursor setting is one for all
// of them.
static void cursor_for(Uint32 window_id) {
  const bool hide = !getenv("TONIC_SYSTEM_CURSOR") && g_sdl && window_id == SDL_GetWindowID(g_sdl) && !menu_active();
  SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
  if (!hide) SDL_SetCursor(SDL_GetDefaultCursor());  // Wayland: set the shape again, or it can stay blank
}

static void update_focus() {
  g_focused = SDL_GetKeyboardFocus() != nullptr;
  audio_set_focused(g_focused);
}

std::string window_last_screenshot() {
  std::lock_guard<std::mutex> l(g_shot_mu);
  return g_shot_last;
}

bool window_pump(int timeout_ms) {
  // GameMode while playing (mod panel), checked twice a second.
  {
    static auto checked = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - checked > std::chrono::milliseconds(500)) {
      checked = now;
      gamemode_want(g_game_shown && g_focused && config_bool("mod_gamemode", true));
    }
  }
  menu_links_tick();
  input_tick();
  if (menu_active()) {
    menu_frame();
    timeout_ms = std::min(timeout_ms, 16);
  }
  // Closing: the closing screen on a surface of its own over the game window
  // (Roblox may not draw frames meanwhile); in Roblox's frames if that fails.
  {
    static bool tried = false;  // once per closing
    if (closescreen_state() && g_sdl) {
      if (!overlaywin_on() && !tried) {
        tried = true;
        int lw = 0, lh = 0;
        if (g_wayland) SDL_GetWindowSize(g_sdl, &lw, &lh);
        else lw = g_w, lh = g_h;
        if (overlaywin_show(g_wayland, g_host_display, g_wayland ? g_wl_surface : g_host_window, lw, lh))
          LOG("closing screen over the game window\n");
      }
      overlaywin_frame();
      timeout_ms = std::min(timeout_ms, 33);
    } else {
      tried = false;
      if (overlaywin_on()) {
        overlaywin_hide();
        // X11 keeps no copy of what was under it: have the game draw again.
        if (!g_wayland) main_post([] { game_surface_changed_again(); });
      }
    }
  }
  if (getenv("TONIC_TEST_PAD")) {
    input_test_virtual_pad();
    timeout_ms = std::min(timeout_ms, 50);
  }
  SDL_Event e;
  if (!SDL_WaitEventTimeout(&e, timeout_ms)) return true;
  do {
    if (e.type == SDL_WINDOWEVENT &&
        (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST))
      update_focus();
    if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_ENTER || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED))
      cursor_for(e.window.windowID);
    if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {  // gamepads, whatever shows
      input_event(e);
      continue;
    }
    if (e.type == SDL_QUIT) return false;
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) return false;
    if (menu_active()) {  // nothing reaches the game while the menu shows
      menu_event(e);
      continue;
    }
    if (closescreen_state()) {  // closing: only Cancel (or Escape); nothing reaches the game
      if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        int ww = 1, wh = 1, pw = 1, ph = 1;
        SDL_GetWindowSize(g_sdl, &ww, &wh);
        SDL_GetWindowSizeInPixels(g_sdl, &pw, &ph);
        if (closescreen_cancel_hit(e.button.x * pw / std::max(1, ww), e.button.y * ph / std::max(1, wh), pw, ph)) menu_closing_cancel();
      }
      if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) menu_closing_cancel();
      if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_size();
      continue;
    }
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F9 && !e.key.repeat) {  // F9: stats overlay
      config_set("stats", config_bool("stats", false) ? "0" : "1");
      continue;
    }
    if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_F9) continue;
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12 && !e.key.repeat) {  // F12: screenshot
      g_shot = true;
      continue;
    }
    if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_F12) continue;
    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_size();
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {  // F11: fullscreen
      const bool full = SDL_GetWindowFlags(g_sdl) & SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_sdl, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
      continue;
    }
    if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_F11) continue;
    input_event(e);
  } while (SDL_PollEvent(&e));
  return true;
}

}  // namespace tonic
