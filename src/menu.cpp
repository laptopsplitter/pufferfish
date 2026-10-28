#include "menu.hpp"
#include "appicon.hpp"
#include "profile.hpp"
#include "platform.hpp"
#include "vkshim.hpp"
#include "panel.hpp"
#include "closescreen.hpp"
#include "weblogin.hpp"
#include "links.hpp"
#ifdef __FreeBSD__
#include "bsdsys.hpp"
#endif

#include <png.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include <vector>

#include "apk.hpp"
#include "config.hpp"
#include "cookies.hpp"
#include "game.hpp"
#include "recent.hpp"
#include "textoverlay.hpp"
#include "usersettings.hpp"
#include "window.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[menu] " fmt, ##__VA_ARGS__)

using Clock = std::chrono::steady_clock;

// Colours: the loading screen's, and the logo's pale yellow-green as accent.
constexpr uint32_t kBg = 0x101216, kPanel = 0x181B21, kRow = 0x20242C, kRowHover = 0x2A2F39, kTrack = 0x2A2E36,
                   kText = 0xF2F4F8, kDim = 0x8A909C, kAccent = 0xA6B05C, kAccentText = 0x101216, kDanger = 0xD9675C;

SDL_Window* g_win = nullptr;
SDL_Renderer* g_r = nullptr;
bool g_active = false;
bool g_settings = false;        // the settings page is open
int g_tab = 0;                  // its page: 0 General, else panel.hpp's PanelTab

Clock::time_point g_since;
long long g_join = 0;           // quick join chosen: place id
std::string g_join_name;
JoinLink g_link;                // a game link waiting: joined once signed in (links.hpp)
std::string g_toast;            // a short note under Play ("not a game link")
Clock::time_point g_toast_at;
int g_links_on = -1;            // links_handler_on(), looked up once
std::string g_links_error;
bool g_setup = false;           // no Roblox installed yet: only the page that gets it
std::string g_setup_reason;     // why, when the installed one failed its check
bool g_kmod = false;            // FreeBSD without tonic's kernel module: the page that loads it, first
bool g_kmod_autoplay = false;   // TONIC_AUTOPLAY waits until that page is passed
#ifdef __FreeBSD__
Clock::time_point g_kmod_check, g_copied_at;
int g_copied = 0;               // which command was just copied (1, 2)
#endif
bool g_dirty = true;            // the launcher needs drawing again
Clock::time_point g_drawn_at;
int g_logout_step = 0;          // 1: asked to confirm
int g_forget_step = 0;          // the same, for forgetting the APK
int g_mx = -1, g_my = -1;       // mouse, in renderer pixels

struct Text {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0, advance = 0;
};
std::map<std::string, Text> g_texts;
SDL_Texture* g_logo = nullptr;
int g_logo_w = 0, g_logo_h = 0;

struct Hit {
  SDL_Rect r;
  std::function<void()> fn;
};
std::vector<Hit> g_hits;  // clickable areas of the last frame

void set_color(uint32_t c) { SDL_SetRenderDrawColor(g_r, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255); }
void fill(int x, int y, int w, int h, uint32_t c) {
  set_color(c);
  SDL_Rect r{x, y, w, h};
  SDL_RenderFillRect(g_r, &r);
}
bool hover(const SDL_Rect& r) { return g_mx >= r.x && g_mx < r.x + r.w && g_my >= r.y && g_my < r.y + r.h; }

Text text(const std::string& s, int px, bool bold) {
  const std::string key = std::to_string(px) + (bold ? "b" : "r") + s;
  auto it = g_texts.find(key);
  if (it != g_texts.end()) return it->second;
  Text t;
  std::vector<uint8_t> rgba;
  if (ui_text_bitmap(s.c_str(), px, bold, rgba, t.w, t.h, t.advance)) {
    t.tex = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t.w, t.h);
    if (t.tex) {
      SDL_UpdateTexture(t.tex, nullptr, rgba.data(), t.w * 4);
      SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    }
  }
  return g_texts[key] = t;
}

// Draws S with its left edge at X (or centred on X), vertically centred on CY.
int put(const std::string& s, int px, bool bold, int x, int cy, uint32_t c, bool centre = false) {
  const Text t = text(s, px, bold);
  if (!t.tex) return 0;
  SDL_SetTextureColorMod(t.tex, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
  SDL_Rect r{centre ? x - t.advance / 2 : x, cy - t.h / 2, t.w, t.h};
  SDL_RenderCopy(g_r, t.tex, nullptr, &r);
  return t.advance;
}

// S shortened with "..." to fit MAXW pixels.
std::string fit(const std::string& s, int px, bool bold, int maxw) {
  if (maxw <= 0) return "";
  if (text(s, px, bold).advance <= maxw) return s;
  std::string t = s;
  while (!t.empty()) {
    do t.pop_back();
    while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (text(t + "...", px, bold).advance <= maxw) return t + "...";
  }
  return "";
}

void button(const SDL_Rect& r, const std::string& label, int px, bool primary, std::function<void()> fn,
            uint32_t fg_override = 0, int bold = -1) {
  const bool h = hover(r);
  fill(r.x, r.y, r.w, r.h, primary ? (h ? 0xB8C26C : kAccent) : (h ? kRowHover : kRow));
  put(label, px, bold < 0 ? primary : bold == 1, r.x + r.w / 2, r.y + r.h / 2, fg_override ? fg_override : primary ? kAccentText : kText, true);
  g_hits.push_back({r, std::move(fn)});
}

void load_logo() {
  png_image img{};
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, share_file("assets/logo.png").c_str())) {
    LOG("assets/logo.png not loaded (%s)\n", img.message);
    return;
  }
  img.format = PNG_FORMAT_RGBA;
  std::vector<uint8_t> px(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, nullptr, px.data(), 0, nullptr)) {
    png_image_free(&img);
    return;
  }
  g_logo_w = static_cast<int>(img.width), g_logo_h = static_cast<int>(img.height);
  g_logo = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, g_logo_w, g_logo_h);
  if (!g_logo) return;
  SDL_UpdateTexture(g_logo, nullptr, px.data(), g_logo_w * 4);
  SDL_SetTextureBlendMode(g_logo, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(g_logo, SDL_ScaleModeBest);
}

// The sliding bar: a segment moving across the track, eased at the ends.
void bar(int x, int y, int w, int h) {
  fill(x, y, w, h, kTrack);
  const float t = std::fmod(std::chrono::duration<float>(Clock::now() - g_since).count(), 1.6f) / 1.6f;
  const float e = t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2.f) / 2;
  const int seg = w * 3 / 10;
  const int x0 = std::max(x, static_cast<int>(x - seg + (w + seg) * e));
  const int x1 = std::min(x + w, static_cast<int>(x + (w + seg) * e));
  if (x1 > x0) fill(x0, y, x1 - x0, h, kAccent);
}

std::string status_line() {
  if (g_join) return "Joining " + (g_join_name.empty() ? g_join > 0 ? "place " + std::to_string(g_join) : std::string("a friend's game") : g_join_name);
  return textoverlay_ready() ? "Roblox is ready" : "Loading Roblox";
}

// --- the loading page -------------------------------------------------------------

// Roblox starts only when Play (or a game under "Jump back in") is pressed:
// main.cpp's library thread waits in menu_wait_start until then.
std::mutex g_start_mu;
std::condition_variable g_start_cv;
bool g_start = false;

void start_roblox() {
  {
    std::lock_guard<std::mutex> l(g_start_mu);
    if (g_start) return;
    g_start = true;
  }
  g_since = Clock::now();
  g_settings = false;
  LOG("starting Roblox%s\n", g_join ? " (quick join)" : "");
  g_start_cv.notify_all();
}

bool started() {
  std::lock_guard<std::mutex> l(g_start_mu);
  return g_start;
}

void toast(const std::string& s) {
  g_toast = s;
  g_toast_at = Clock::now();
}

// Starts Roblox and joins L (a link, or a game under "Jump back in").
void join_now(const JoinLink& l, const std::string& name = "") {
  g_join = l.place > 0 ? l.place : -1;
  g_join_name = name;
  if (name.empty() && l.place > 0)
    for (const RecentGame& g : recent_list())
      if (g.place == l.place) g_join_name = g.name;
  if (!l.link_code.empty()) g_join_name = (g_join_name.empty() ? "place " + std::to_string(l.place) : g_join_name) + " (private server)";
  game_quick_join(l.place, l.user, l.link_code);
  start_roblox();
}

// A link from the browser, the clipboard or the command line.
void take_link(const JoinLink& l) {
  if (started()) return;
  if (g_kmod) {  // after the kernel module page (menu_frame)
    g_link = l;
    return;
  }
  if (cookies_have_login() && !g_setup) return join_now(l);
  g_link = l;  // once signed in (menu_frame)
  if (!g_setup && !weblogin_running()) weblogin_start();
  toast("Sign in first: then tonic joins " + link_describe(l));
}

// Settings, bottom right (only before Roblox starts), and the Ctrl+V hint.
void settings_button(int W, int H, bool hint) {
  const int bpx = std::max(13, H * 22 / 1000), bw = bpx * 7, bh = bpx * 2 + 4, m = std::max(12, H / 40);
  if (hint) put("Ctrl+V: join a copied game link", bpx, false, m, H - m - bh / 2, kDim);
  button({W - bw - m, H - bh - m, bw, bh}, "Settings", bpx, false, [] {
    g_settings = true;
    g_forget_step = 0;
  });
}

void draw_loading(int W, int H) {
  const bool going = started();
  const int title_px = std::max(28, H * 9 / 100), sub_px = std::max(14, H * 25 / 1000);
  const int logo_h = g_logo ? std::min(H * 34 / 100, g_logo_h) : 0;
  const int logo_w = g_logo ? g_logo_w * logo_h / std::max(1, g_logo_h) : 0;
  const Text title = text("tonic", title_px, true);
  const int title_h = title.tex ? title.h : title_px, gap = H * 3 / 100, bar_h = std::max(3, H / 200);
  const auto games = going || !cookies_have_login() ? std::vector<RecentGame>{} : recent_list();
  const int chip_px = std::max(13, H * 22 / 1000), chip_h = chip_px * 2 + 6;
  const int play_px = std::max(16, H * 30 / 1000), play_h = play_px * 2 + 12, play_w = std::max(play_px * 9, 180);
  // Below the title: Play (and the recent games), or once started the bar and status.
  const int under = going ? H * 4 / 100 + bar_h + H * 25 / 1000 + sub_px
                          : H * 4 / 100 + play_h + (games.empty() ? 0 : H * 5 / 100 + chip_px + 10 + chip_h);
  const int block = logo_h + (logo_h ? gap : 0) + title_h + under;
  const int top = std::max(0, (H - block) / 2), cx = W / 2;
  if (g_logo && logo_h > 0) {
    SDL_Rect r{cx - logo_w / 2, top, logo_w, logo_h};
    SDL_RenderCopy(g_r, g_logo, nullptr, &r);
  }
  const int title_top = top + logo_h + (logo_h ? gap : 0);
  put("tonic", title_px, true, cx, title_top + title_h / 2, kText, true);
  const int below = title_top + title_h + H * 4 / 100;
  if (going) {
    const int bar_w = std::min(W * 40 / 100, 320);
    bar(cx - bar_w / 2, below, bar_w, bar_h);
    put(status_line(), sub_px, false, cx, below + bar_h + H * 25 / 1000 + sub_px / 2, kDim, true);
    return;  // no settings once Roblox runs: they apply at its start
  }
  if (!g_toast.empty() && Clock::now() - g_toast_at < std::chrono::seconds(5))
    put(g_toast, sub_px, false, cx, std::max(sub_px, top - sub_px * 2), kText, true);
  // Not signed in: the big button signs you in first (on roblox.com,
  // weblogin.hpp), then becomes Play.
  if (!cookies_have_login()) {
    const bool open = weblogin_running();
    button({cx - play_w / 2, below, play_w, play_h}, open ? "Signing in..." : "Sign in", play_px, !open, [] {
      if (!weblogin_running()) weblogin_start();
    });
    const std::string st = weblogin_status();
    put(open ? "Sign in on roblox.com in the new window" : !st.empty() ? st : "Sign in to your Roblox account to play", sub_px, false, cx,
        below + play_h + H * 3 / 100, kDim, true);
    settings_button(W, H, false);  // (settings work signed out too)
    return;
  }
  button({cx - play_w / 2, below, play_w, play_h}, "Play", play_px, true, [] { start_roblox(); });
  // Jump back in: the last few games; one click starts Roblox and joins.
  if (!games.empty()) {
    int y = below + play_h + H * 5 / 100;
    put("Jump back in", chip_px, false, cx, y + chip_px / 2, kDim, true);
    y += chip_px + 10;
    struct Chip { long long place; std::string name; int w; };
    std::vector<Chip> chips;
    int total = 0;
    const int pad = chip_px, space = 8, max_w = std::min(W - 2 * pad, 760);
    for (const RecentGame& g : games) {
      if (chips.size() >= 3) break;
      std::string name = g.name.empty() ? "Place " + std::to_string(g.place) : g.name;
      if (name.size() > 26) name = name.substr(0, 24) + "...";
      const int w = text(name, chip_px, false).advance + 2 * pad;
      if (total + w + (chips.empty() ? 0 : space) > max_w) break;
      total += w + (chips.empty() ? 0 : space);
      chips.push_back({g.place, name, w});
    }
    int x = cx - total / 2;
    for (const Chip& c : chips) {
      const long long place = c.place;
      const std::string name = c.name;
      button({x, y, c.w, chip_h}, c.name, chip_px, false, [place, name] {
        JoinLink l;
        l.place = place;
        join_now(l, name);
      }, 0, 0);
      x += c.w + space;
    }
  }
  settings_button(W, H, true);
}

// --- the settings page -------------------------------------------------------------

// Above 240 also needs the engine's frame rate target raised (usersettings.cpp).
const int kCaps[] = {0, 30, 60, 75, 120, 144, 165, 200, 240, 360, 480};

void step_cap(int dir) {
  const int cur = config_int("fps_cap", 0);
  const int n = sizeof kCaps / sizeof kCaps[0];
  // From a typed value, step to the next listed one in that direction.
  int i = dir > 0 ? n - 1 : 0;
  if (dir > 0) {
    for (int k = 0; k < n; ++k)
      if (kCaps[k] > cur) { i = k; break; }
  } else {
    for (int k = n - 1; k >= 0; --k)
      if (kCaps[k] < cur) { i = k; break; }
  }
  config_set("fps_cap", std::to_string(kCaps[i]));
}

// Typing a frame rate: click the value, type digits, Enter (or click
// elsewhere) sets it, Esc leaves it as it was. Empty or 0: Roblox's own.
bool g_typing = false;
std::string g_typed;
SDL_Rect g_field{};  // where the number is typed
void commit_typed() {
  if (!g_typing) return;
  g_typing = false;
  const int v = g_typed.empty() ? 0 : std::clamp(atoi(g_typed.c_str()), 0, 2000);
  config_set("fps_cap", std::to_string(v));
}

// Resolution cap: the most pixels the game draws in height, scaled up to
// the window (window.cpp). 0 (last): none.
const int kRes[] = {360, 480, 540, 720, 900, 1080, 1440, 2160, 0};
void step_res(int dir) {
  const int cur = config_int("res_cap", 0);
  const int n = sizeof kRes / sizeof kRes[0];
  int i = n - 1;
  for (int k = 0; k < n; ++k)
    if (kRes[k] == cur) i = k;
  i = std::clamp(i + dir, 0, n - 1);
  config_set("res_cap", std::to_string(kRes[i]));
}

const int kRam[] = {0, 512, 768, 1024, 1280, 1536, 2048, 3072, 4096, 6144, 8192};  // MB
std::string ram_text(int mb) {
  if (!mb) return "Off";
  if (mb < 1024) return std::to_string(mb) + " MB";
  char buf[16];
  snprintf(buf, sizeof buf, mb % 1024 ? "%.3g GB" : "%.0f GB", mb / 1024.0);
  return buf;
}
void step_ram(int dir) {
  const int cur = config_int("ram_limit_mb", 0);
  const int n = sizeof kRam / sizeof kRam[0];
  int i = 0;
  for (int k = 0; k < n; ++k)
    if (kRam[k] == cur) i = k;
  i = std::clamp(i + dir, 0, n - 1);
  config_set("ram_limit_mb", std::to_string(kRam[i]));
  memory_limit_apply();
}

void toggle(const char* name, bool fallback) { config_set(name, config_bool(name, fallback) ? "0" : "1"); }

bool g_logged_out = false;

// --- Roblox from your APK (apk.hpp) ---------------------------------------------------

std::mutex g_inst_mu;
std::string g_inst_status, g_inst_error;
bool g_installing = false, g_restart_now = false;

}  // namespace

void restart_tonic(const char* why, bool play) {
  if (play) setenv("TONIC_AUTOPLAY", "1", 1);  // straight back into Roblox, no launcher stop
  // Start again with the same arguments: the library is loaded once, at start.
  std::vector<std::string> parts = self_cmdline();
  std::vector<char*> argv;
  for (auto& s : parts) argv.push_back(s.data());
  argv.push_back(nullptr);
  LOG("%s; starting again\n", why);
  fflush(stderr);
  fflush(stdout);
  execv(self_exec_path().c_str(), argv.data());
  LOG("restart failed: %s (start tonic again yourself)\n", strerror(errno));
}

namespace {

// No Roblox yet: the newest x86_64 one from APKPure (apk.hpp), checked and
// installed. Started by itself on the setup page.
bool g_fetch_tried = false;
void begin_fetch() {
  {
    std::lock_guard<std::mutex> l(g_inst_mu);
    if (g_installing) return;
    g_installing = true;
    g_inst_error.clear();
    g_inst_status = "Asking APKPure for the newest Roblox";
  }
  g_fetch_tried = true;
  LOG("downloading the newest Roblox from APKPure\n");
  apk_fetch_latest(
      [](const std::string& s) {
        std::lock_guard<std::mutex> l(g_inst_mu);
        g_inst_status = s;
      },
      [](const std::string& version, const std::string& error) {
        std::lock_guard<std::mutex> l(g_inst_mu);
        g_installing = false;
        if (version.empty()) {
          g_inst_error = error;
          g_inst_status.clear();
          return;
        }
        config_set("apk_version", version);
        g_inst_status = "Roblox " + version + " installed";
        g_restart_now = true;
      });
}


void install_status(std::string& status, std::string& error, bool& busy) {
  std::lock_guard<std::mutex> l(g_inst_mu);
  status = g_inst_status, error = g_inst_error, busy = g_installing;
}

void draw_setup(int W, int H) {
  if (!g_fetch_tried && !getenv("TONIC_NO_APK_DOWNLOAD")) begin_fetch();  // first: try fetching it
  const int title_px = std::max(28, H * 9 / 100), px = std::max(15, H * 28 / 1000), small = std::max(13, H * 22 / 1000);
  const int logo_h = g_logo ? std::min(H * 30 / 100, g_logo_h) : 0;
  const int logo_w = g_logo ? g_logo_w * logo_h / std::max(1, g_logo_h) : 0;
  const int bh = px * 2 + 12, bw = std::max(px * 10, 200);
  const int block = logo_h + H * 3 / 100 + title_px + H * 4 / 100 + px + 8 + small + H * 4 / 100 + bh + H * 3 / 100 + small * 2;
  int y = std::max(0, (H - block) / 2);
  const int cx = W / 2;
  if (g_logo && logo_h > 0) {
    SDL_Rect r{cx - logo_w / 2, y, logo_w, logo_h};
    SDL_RenderCopy(g_r, g_logo, nullptr, &r);
  }
  y += logo_h + H * 3 / 100;
  put("tonic", title_px, true, cx, y + title_px / 2, kText, true);
  y += title_px + H * 4 / 100;
  std::string status, error;
  bool busy;
  install_status(status, error, busy);
  put(busy ? "Getting Roblox" : error.empty() ? "Get Roblox to start" : "Could not get Roblox", px, false, cx, y + px / 2, kText, true);
  y += px + 8;
  put(fit(!g_setup_reason.empty() && error.empty() ? g_setup_reason  // (while it is being fetched)
                                                    : "The newest x86_64 build from APKPure, checked to be Roblox's own before it is installed",
          small, false, W - 40),
      small, false, cx, y + small / 2, kDim, true);
  y += small + H * 4 / 100;
  if (busy) {
    const int bar_w = std::min(W * 40 / 100, 320), bar_h = std::max(3, H / 200);
    const float f = apk_progress_fraction();
    if (f >= 0) {  // how much is downloaded
      fill(cx - bar_w / 2, y + bh / 2 - bar_h / 2, bar_w, bar_h, kTrack);
      fill(cx - bar_w / 2, y + bh / 2 - bar_h / 2, static_cast<int>(bar_w * f), bar_h, kAccent);
    } else {
      bar(cx - bar_w / 2, y + bh / 2 - bar_h / 2, bar_w, bar_h);
    }
  } else {
    button({cx - bw / 2, y, bw, bh}, error.empty() ? "Download" : "Try again", px, true, begin_fetch);
  }
  y += bh + H * 3 / 100;
  if (!error.empty()) put(fit(error, small, false, W - 40), small, false, cx, y + small / 2, kDanger, true);
  else if (!status.empty()) put(status, small, false, cx, y + small / 2, kDim, true);

}

#ifdef __FreeBSD__
// FreeBSD, without tonic's kernel module (bsdsys.hpp): the two commands that
// load it, to copy into a terminal. The page goes on to the launcher by
// itself once the module is there (menu_frame looks every second).
void draw_kmod(int W, int H) {
  const int title_px = std::max(26, H * 7 / 100), px = std::max(15, H * 27 / 1000), small = std::max(13, H * 21 / 1000);
  const int logo_h = g_logo ? std::min(H * 16 / 100, g_logo_h) : 0;
  const int logo_w = g_logo ? g_logo_w * logo_h / std::max(1, g_logo_h) : 0;
  const int col_w = std::min(W - 2 * std::max(16, W / 30), std::max(560, px * 44));
  const int x = (W - col_w) / 2, cx = W / 2;
  const int box_h = px * 2 + 12, copy_w = px * 5, lab_gap = small + 10;
  const int block = logo_h + H * 2 / 100 + title_px + H * 3 / 100 + px + 8 + small + H * 4 / 100 +
                    2 * (lab_gap + box_h + H * 3 / 100) + small * 2;
  int y = std::max(std::max(8, H / 40), (H - block) / 2);
  if (g_logo && logo_h > 0) {
    SDL_Rect r{cx - logo_w / 2, y, logo_w, logo_h};
    SDL_RenderCopy(g_r, g_logo, nullptr, &r);
  }
  y += logo_h + H * 2 / 100;
  put("One more step on FreeBSD", title_px, true, cx, y + title_px / 2, kText, true);
  y += title_px + H * 3 / 100;
  const BsdModule st = bsdsys_state();
  put(st == BsdModule::old ? "An older version of tonic's kernel module is loaded: it needs replacing"
                           : "tonic needs its small kernel module loaded",
      px, false, cx, y + px / 2, kText, true);
  y += px + 8;
  put(fit("It lets Roblox's own system checks work on FreeBSD. In a terminal, run these two:", small, false, col_w), small,
      false, cx, y + small / 2, kDim, true);
  y += small + H * 4 / 100;
  const bool fresh = Clock::now() - g_copied_at < std::chrono::seconds(2);
  auto command = [&](int n, const std::string& label, const std::string& cmd) {
    put(label, small, true, x, y + small / 2, kDim);
    y += lab_gap;
    const int tw = col_w - copy_w - 8;
    fill(x, y, tw, box_h, kPanel);
    // The whole command when it fits (smaller type if need be); Copy always copies all of it.
    int cpx = px;
    while (cpx > 11 && text(cmd, cpx, false).advance > tw - 24) --cpx;
    put(fit(cmd, cpx, false, tw - 24), cpx, false, x + 12, y + box_h / 2, kText);
    const bool done = fresh && g_copied == n;
    button({x + tw + 8, y, copy_w, box_h}, done ? "Copied" : "Copy", px, !done, [n, cmd] {
      SDL_SetClipboardText(cmd.c_str());
      g_copied = n;
      g_copied_at = Clock::now();
    });
    y += box_h + H * 3 / 100;
  };
  command(1, "1. Become root (it asks for the root password)", "su -");
  command(2, "2. Load the module", bsdsys_load_command());
  if (bsdsys_sources_missing()) {
    put(fit("The module is not built yet and the kernel sources (/usr/src) are missing: see tonic's README", small, false,
            col_w),
        small, false, cx, y + small / 2, kDanger, true);
  } else if (st == BsdModule::failed) {
    put("The module is loaded but would not turn on: see the log", small, false, cx, y + small / 2, kDanger, true);
  } else {
    const int bar_w = std::min(W * 30 / 100, 240), bar_h = std::max(3, H / 200);
    bar(cx - bar_w / 2, y + small / 2 - bar_h / 2, bar_w, bar_h);
    y += bar_h + 10;
    put("tonic carries on by itself once it is loaded", small, false, cx, y + small / 2, kDim, true);
  }
  // Going on without it, bottom left.
  const int bpx = std::max(13, H * 21 / 1000), bh = bpx * 2 + 4, m = std::max(12, H / 40);
  const std::string skip = "Continue without it";
  const int bw = text(skip, bpx, false).advance + bpx * 2;
  button({m, H - bh - m, bw, bh}, skip, bpx, false, [] {
    LOG("going on without the kernel module\n");
    g_kmod = false;
  });
  put("Roblox disconnects after about a minute without it", bpx, false, m + bw + 12, H - m - bh / 2, kDim);
}
#endif

void log_out() {
  recent_set_owner("");  // logged out: no recent games
  weblogin_forget();
  cookies_logout();  // Roblox has not started: it simply finds no saved login
  g_logged_out = true;
  g_logout_step = 0;
}

void draw_settings(int W, int H) {
  const int px = std::max(14, H * 26 / 1000), small = std::max(12, H * 21 / 1000), big = std::max(20, H * 45 / 1000);
  const int m = std::max(16, W / 30), gap = 6, footer_h = px * 2 + 10, tab_h = px * 2 + 4;
  // Ten rows, the tabs and the Back button: rows get shorter in a short
  // window, so nothing overlaps.
  const int row_h = std::max(px + small + 8, std::min(px * 2 + 10, (H - m - big - 16 - tab_h - 12 - footer_h - m - 10) / 10 - gap));
  const int col_w = std::min(std::max(660, px * 48), W - 2 * m);
  const int x = (W - col_w) / 2;
  int y = m;
  put("Settings", big, true, x, y + big / 2 + 4, kText);
  y += big + 16;
  // Pages
  {
    static const char* tabs[] = {"General", "Performance", "Picture", "Sound", "Look", "Controls", "Instances"};
    const int n = 7, tw = (col_w - (n - 1) * 4) / n;
    for (int i = 0; i < n; ++i)
      button({x + i * (tw + 4), y, tw, tab_h}, tabs[i], small + 1, g_tab == i, [i] {
        g_tab = i;
        g_forget_step = g_logout_step = 0;
      });
    y += tab_h + 12;
  }
  const int bw = px * 5, fy_min = H - m - footer_h;
  if (g_tab != 0) {
    panel_draw(g_r, g_tab, x, y, col_w, fy_min - y - 8, px, small, row_h, g_mx, g_my);
    button({x + col_w - bw * 2 - 10, fy_min, bw * 2 + 10, footer_h}, "Back", px, true, [] { g_settings = false; });
    return;
  }

  // RESERVE: the width of the controls on the right; the text stays left of them.
  auto row = [&](const std::string& label, const std::string& note, int reserve) {
    fill(x, y, col_w, row_h, kPanel);
    const int tw = col_w - 14 - reserve - 14 - 16;
    put(fit(label, px, false, tw), px, false, x + 14, y + row_h / 2 - (note.empty() ? 0 : px / 2), kText);
    if (!note.empty()) put(fit(note, small, false, tw), small, false, x + 14, y + row_h / 2 + px / 2 + 2, kDim);
  };
  auto next = [&] { y += row_h + gap; };
  const int bh = row_h - 14;
  const int stepper = bh + px * 6 + bh;  // < value >

  // Frame rate cap: < value >
  {
    row("Frame rate cap", g_typing ? "Type a number, Enter to set (empty: Roblox's own)" : "Click the number to type one", stepper);
    const int cap = config_int("fps_cap", 0);
    const int rx = x + col_w - 14 - bh - px * 6 - bh;
    button({rx, y + 7, bh, bh}, "<", px, false, [] { commit_typed(); step_cap(-1); });
    const SDL_Rect field{rx + bh + 4, y + 7, px * 6 - 8, bh};
    if (g_typing) {
      fill(field.x, field.y, field.w, field.h, kRowHover);
      const int tw = put(g_typed, px, true, field.x + field.w / 2, y + row_h / 2, kText, true);
      if ((std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() / 500) % 2 == 0)
        fill(field.x + field.w / 2 + tw / 2 + 1, y + row_h / 2 - px / 2, 2, px, kAccent);
      g_field = field;
      g_hits.push_back({field, [] {}});  // clicks inside keep typing
    } else {
      button(field, cap ? std::to_string(cap) + " fps" : "Roblox's", px, false, [cap] {
        g_typing = true;
        g_typed = cap ? std::to_string(cap) : "";
      }, 0, 1);
    }
    button({rx + bh + px * 6, y + 7, bh, bh}, ">", px, false, [] { commit_typed(); step_cap(1); });
    next();
  }
  auto onoff = [&](const char* name, bool fallback, const std::string& label, const std::string& note) {
    row(label, note, bw);
    const bool on = config_bool(name, fallback);
    std::string key = name;
    button({x + col_w - 14 - bw, y + 7, bw, bh}, on ? "On" : "Off", px, on, [key, fallback] { toggle(key.c_str(), fallback); });
    next();
  };
  // Resolution cap: < value >
  {
    row("Resolution cap", "Fewer pixels, scaled up: more FPS, softer picture", stepper);
    const int res = config_int("res_cap", 0);
    const int rx = x + col_w - 14 - bh - px * 6 - bh;
    button({rx, y + 7, bh, bh}, "<", px, false, [] { step_res(-1); });
    put(res ? std::to_string(res) + "p" : "Off", px, true, rx + bh + px * 3, y + row_h / 2, kText, true);
    button({rx + bh + px * 6, y + 7, bh, bh}, ">", px, false, [] { step_res(1); });
    next();
  }
  // RAM limit: < value >
  {
    row("RAM limit (not recommended)", "Soft: past it Roblox slows down, no crash", stepper);
    const int mb = config_int("ram_limit_mb", 0);
    const int rx = x + col_w - 14 - bh - px * 6 - bh;
    button({rx, y + 7, bh, bh}, "<", px, false, [] { step_ram(-1); });
    put(ram_text(mb), px, true, rx + bh + px * 3, y + row_h / 2, kText, true);
    button({rx + bh + px * 6, y + 7, bh, bh}, ">", px, false, [] { step_ram(1); });
    next();
  }
  // Renderer: Vulkan (falls back to OpenGL by itself when it does not work)
  {
    const bool vk = renderer_setting() == "vulkan", failed = config_bool("vulkan_failed", false);
    const int rw = px * 6;
    row("Renderer", vk ? (failed ? "Vulkan did not work here, so OpenGL. Click to try Vulkan again"
                                 : "Vulkan; OpenGL by itself if Vulkan does not work")
                       : "OpenGL (overlays, filters and the resolution cap all work)",
        rw);
    button({x + col_w - 14 - rw, y + 7, rw, bh}, vk && !failed ? "Vulkan" : "OpenGL", px, vk && !failed, [vk, failed] {
      if (vk && failed) {  // try Vulkan again
        config_set("vulkan_failed", "0");
        return;
      }
      config_set("renderer", vk ? "opengl" : "vulkan");
      config_set("vulkan_failed", "0");
    });
    next();
  }
  onoff("vsync", false, "VSync", "Wait for the screen's refresh (off: highest frame rate)");
  onoff("stats", false, "Stats overlay", "Frame rate, average, ping, CPU and frame times in a corner (F9 in game)");
  onoff("fullscreen", false, "Start in fullscreen", "F11 switches at any time");
  // Roblox links: Play on roblox.com (and roblox:// links) open tonic.
  {
    if (g_links_on < 0) g_links_on = links_handler_on();
    row("Open Roblox links in tonic", !g_links_error.empty() ? g_links_error
                                      : g_links_on ? "Play on roblox.com starts tonic and joins (or Ctrl+V a link here)"
                                                   : "Play on roblox.com in your browser opens tonic (instead of any other client)",
        bw);
    button({x + col_w - 14 - bw, y + 7, bw, bh}, g_links_on ? "On" : "Off", px, g_links_on == 1, [] {
      g_links_error.clear();
      if (!links_handler_set(!g_links_on, g_links_error)) g_links_error = "Could not: " + g_links_error;
      g_links_on = links_handler_on();
    });
    next();
  }
  // Roblox: remove it and download it again (the newest, from APKPure)
  {
    RobloxInstall cur;
    const bool have = apk_current(cur);
    const int fw = px * 8;
    row("Roblox", g_forget_step ? "Click again: remove it and download the newest"
                  : have ? "Version " + cur.version + ", checked at each start"
                         : "None", fw);
    button({x + col_w - 14 - fw, y + 7, fw, bh}, g_forget_step ? "Confirm" : "Download again", px, false, [] {
      if (g_forget_step++ == 0) return;
      apk_forget();
      restart_tonic("Roblox removed: downloading it again");
    }, kDanger);
    next();
  }
  // Account
  if (weblogin_running() || !cookies_have_login()) {
    // Roblox's own sign-in can ask for a device check Linux cannot pass:
    // signing in on roblox.com works instead (weblogin.hpp).
    const std::string st = weblogin_status();
    const int sw = text("Sign in on roblox.com", px, true).advance + px * 2;
    row("Account", !st.empty() ? st : g_logged_out ? "Logged out. Sign in on Roblox's website, or in Roblox when it starts"
                                                  : "Not logged in. Sign in on Roblox's website, or in Roblox when it starts", sw);
    button({x + col_w - 14 - sw, y + 7, sw, bh}, weblogin_running() ? "Window open..." : "Sign in on roblox.com", px,
           !weblogin_running(), [] {
             if (!weblogin_running()) weblogin_start();
           });
    next();
  } else if (weblogin_status().rfind("Signed in", 0) == 0 && g_logout_step == 0) {
    g_logged_out = false;
    row("Account", weblogin_status(), bw + px * 2);
    button({x + col_w - 14 - bw - px * 2, y + 7, bw + px * 2, bh}, "Log out", px, false, [] { g_logout_step = 1; }, kDanger);
    next();
  } else {
    row("Account", g_logged_out ? "Logged out: Roblox will ask you to log in" : g_logout_step ? "Click again to log out" : "Forget the saved login",
        bw + px * 2);
    button({x + col_w - 14 - bw - px * 2, y + 7, bw + px * 2, bh}, g_logged_out ? "Logged out" : g_logout_step ? "Confirm" : "Log out", px, false,
           [] {
             if (g_logged_out) return;
             if (g_logout_step++ == 0) return;
             log_out();
           },
           kDanger);
    next();
  }
  // Footer: Back.
  const int fy = std::max(y + 4, H - m - footer_h);
  button({x + col_w - bw * 2 - 10, fy, bw * 2 + 10, footer_h}, "Back", px, true, [] { g_settings = false; });
}

// --- closing: "tonic is going for a shutdown" ---------------------------------------

int g_closing = 0;  // 1: a moment to cancel, 2: Roblox is saving and leaving
Clock::time_point g_close_t0;
constexpr auto kCloseGrace = std::chrono::milliseconds(1500);

void cancel_closing() {
  if (g_closing != 1) return;
  g_closing = 0;
  closescreen_set(0);
  if (!getenv("TONIC_SYSTEM_CURSOR")) SDL_ShowCursor(SDL_DISABLE);  // the game draws its own again
  LOG("closing cancelled\n");
}

void close_menu(const char* why) {
  // Hidden, not destroyed: SDL's renderer may draw through EGL on the same
  // Wayland display as the game, and destroying it terminates that display
  // (eglTerminate) under the game: its frames then fail.
  SDL_HideWindow(g_win);
  g_active = false;
  LOG("closed (%s)\n", why);
  window_show_game();
}

}  // namespace

bool menu_open(int w, int h) {
  g_win = SDL_CreateWindow(profile_title().c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_ALLOW_HIGHDPI);
  app_icon_set(g_win);
  if (g_win) g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
  if (!g_win || !g_r) {
    LOG("no loading window (%s)\n", SDL_GetError());
    if (g_win) SDL_DestroyWindow(g_win);
    g_win = nullptr;
    return false;
  }
  SDL_ShowCursor(SDL_ENABLE);  // the menu needs one; the game draws its own
  SDL_SetCursor(SDL_GetDefaultCursor());
  recent_fetch_names();        // names of recent games not looked up yet
  g_since = Clock::now();
  g_active = true;
  load_logo();
  JoinLink link;
  if (link_take_startup(link)) {
    unsetenv("TONIC_AUTOPLAY");
    take_link(link);
  }
#ifdef __FreeBSD__
  {
    const BsdModule st = bsdsys_state();
    g_kmod = st == BsdModule::missing || st == BsdModule::old || st == BsdModule::failed;
    g_kmod_check = Clock::now();
  }
#endif
  if (getenv("TONIC_AUTOPLAY") && !g_setup) {  // restarted from the mod panel
    unsetenv("TONIC_AUTOPLAY");
    if (g_kmod) g_kmod_autoplay = true;
    else start_roblox();
  }
  menu_frame();
  return true;
}

bool menu_active() { return g_active; }

void menu_links_tick() {
  if (!profile_id().empty()) return;  // links go to Main
  JoinLink l;
  if (!link_poll(l)) return;
  if (g_active && !started()) {
    SDL_RaiseWindow(g_win);
    g_dirty = true;
    return take_link(l);
  }
  // In a game (or starting one): Roblox leaves it and tonic starts again
  // straight into the new one.
  static bool going = false;
  if (going) return;
  going = true;
  std::thread([l] {
    game_shutdown();
    link_for_restart(l);
    restart_tonic("joining a game from a link");
  }).detach();
}

bool menu_roblox_started() { return started(); }

bool menu_closing_begin() {
  // Only over a running game (the launcher closes at once).
  if (!started() || g_closing || g_active) return false;
  g_closing = 1;
  g_close_t0 = Clock::now();
  closescreen_set(1);  // drawn in the game window itself: its size and place
  SDL_ShowCursor(SDL_ENABLE);
  LOG("closing (Cancel to stay)\n");
  return true;
}

void menu_closing_cancel() { cancel_closing(); }

int menu_closing_state() {
  if (g_closing == 1 && Clock::now() - g_close_t0 > kCloseGrace) return 3;  // the moment to cancel is over
  return g_closing;
}

void menu_closing_go() {
  g_closing = 2;
  closescreen_set(2);
}

void menu_set_setup() { g_setup = true; }
void menu_set_setup_reason(const std::string& why) { g_setup_reason = why; }

void menu_wait_start() {
  std::unique_lock<std::mutex> l(g_start_mu);
  if (!g_active) return;  // no menu: start at once
  g_start_cv.wait(l, [] { return g_start; });
}

void menu_frame() {
  if (!g_active) return;
  if (started() && textoverlay_ready()) return close_menu("the game is drawing");
  if (started() && Clock::now() - g_since > std::chrono::seconds(120)) return close_menu("timed out");
#ifdef __FreeBSD__
  // The kernel module page: looked again every second; gone once it is loaded.
  if (g_kmod && Clock::now() - g_kmod_check > std::chrono::seconds(1)) {
    g_kmod_check = Clock::now();
    if (bsdsys_retry() == BsdModule::on) {
      LOG("kernel module loaded: on to the launcher\n");
      g_kmod = false;
      g_dirty = true;
    }
  }
  if (!g_kmod && g_kmod_autoplay) {
    g_kmod_autoplay = false;
    if (!g_setup) start_roblox();
  }
#endif
  if (g_link.any() && !started() && !g_setup && !g_kmod && cookies_have_login() && !weblogin_running()) {
    const JoinLink l = g_link;
    g_link = {};
    join_now(l);
  }
  // Drawn when something changed (input, an animation, a status that may
  // have), else a few times a second: the launcher is drawn on the CPU.
  const bool moving = started() || g_typing || g_installing || weblogin_running() || g_kmod ||
                      (!g_toast.empty() && Clock::now() - g_toast_at < std::chrono::seconds(6));
  if (!g_dirty && !moving && Clock::now() - g_drawn_at < std::chrono::milliseconds(250)) return;
  g_dirty = false;
  g_drawn_at = Clock::now();
  int W = 0, H = 0;
  SDL_GetRendererOutputSize(g_r, &W, &H);
  if (W <= 0 || H <= 0) return;
  SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_NONE);
  set_color(kBg);
  SDL_RenderClear(g_r);
  g_hits.clear();
  if (g_texts.size() > 400) {  // labels change (names, values): keep the cache small, between frames
    for (auto& [k, v] : g_texts)
      if (v.tex) SDL_DestroyTexture(v.tex);
    g_texts.clear();
  }
  {
    std::lock_guard<std::mutex> l(g_inst_mu);
    if (g_restart_now) {
      g_restart_now = false;
      restart_tonic("Roblox installed from your APK");
    }
  }
#ifdef __FreeBSD__
  if (g_kmod) draw_kmod(W, H);
  else
#endif
  if (g_setup) draw_setup(W, H);
  else if (g_settings) draw_settings(W, H);
  else draw_loading(W, H);
  SDL_RenderPresent(g_r);
}

void menu_event(const SDL_Event& e) {
  if (!g_active) return;
  g_dirty = true;
  // Ctrl+V: a copied game link (roblox.com/games/..., a private server link, a place id).
  if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_v && (e.key.keysym.mod & KMOD_CTRL) && !g_typing && !g_setup &&
      !g_kmod && !started()) {
    char* clip = SDL_GetClipboardText();
    const std::string text = clip ? clip : "";
    SDL_free(clip);
    JoinLink l;
    if (link_parse(text, l)) take_link(l);
    else toast("The clipboard has no Roblox game link");
    return;
  }
  // Window points -> renderer pixels (HiDPI).
  auto to_px = [](int x, int y, int& ox, int& oy) {
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GetRendererOutputSize(g_r, &pw, &ph);
    ox = x * pw / std::max(1, ww);
    oy = y * ph / std::max(1, wh);
  };
  if (e.type == SDL_MOUSEMOTION) to_px(e.motion.x, e.motion.y, g_mx, g_my);
  if (e.type == SDL_DROPFILE) {  // (files dropped onto the window: not used)
    SDL_free(e.drop.file);
    return;
  }
  if (g_typing) {
    if (e.type == SDL_TEXTINPUT) {
      for (const char* c = e.text.text; *c; ++c)
        if (*c >= '0' && *c <= '9' && g_typed.size() < 4) g_typed += *c;
      return;
    }
    if (e.type == SDL_KEYDOWN) {
      const auto k = e.key.keysym.scancode;
      if (k == SDL_SCANCODE_BACKSPACE && !g_typed.empty()) g_typed.pop_back();
      if (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) commit_typed();
      if (k == SDL_SCANCODE_ESCAPE) g_typing = false;
      return;
    }
  }
  if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
    to_px(e.button.x, e.button.y, g_mx, g_my);
    if (g_typing && !hover(g_field)) commit_typed();  // a click elsewhere sets it
    bool hit = false;
    for (const Hit& h : g_hits)
      if (hover(h.r)) {
        auto fn = h.fn;  // may change the page (and g_hits with it)
        fn();
        hit = true;
        break;
      }
    if (!hit && g_settings && g_tab != 0 && !g_kmod) panel_click(g_mx, g_my);  // the page's own buttons
  }
  if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g_settings = false;
  if (e.type == SDL_KEYDOWN && (e.key.keysym.scancode == SDL_SCANCODE_RETURN || e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) && !g_settings && !g_setup &&
      !g_kmod) {
    if (cookies_have_login()) start_roblox();
    else if (!weblogin_running()) weblogin_start();  // Enter = the big button
  }
}

}  // namespace tonic
