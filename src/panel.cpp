#include "panel.hpp"
#include "spawn.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "account.hpp"
#include "apk.hpp"
#include "device.hpp"
#include "config.hpp"
#include "framegen.hpp"
#include "gamemode.hpp"
#include "mods.hpp"
#include "profile.hpp"
#include "textoverlay.hpp"
#include "vkshim.hpp"
#include "window.hpp"

namespace tonic {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPanel = 0x181B21, kRow = 0x20242C, kRowHover = 0x2A2F39, kText = 0xF2F4F8, kDim = 0x8A909C,
                   kAccent = 0xA6B05C, kAccentText = 0x101216, kDanger = 0xD9675C;

SDL_Renderer* g_r = nullptr;
int g_mx = -1, g_my = -1;
bool g_relist = false;
std::string g_delete_id = "-", g_instance_msg;

// File choosers answer on their own thread; applied when the page is drawn.
std::mutex g_mu;
std::vector<std::function<void()>> g_pending;
void later(std::function<void()> fn) {
  std::lock_guard<std::mutex> l(g_mu);
  g_pending.push_back(std::move(fn));
}

struct Text {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0, advance = 0;
};
std::map<std::string, Text> g_texts;
struct Hit {
  SDL_Rect r;
  std::function<void()> fn;
};
std::vector<Hit> g_hits;

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
  if (!s.empty() && ui_text_bitmap(s.c_str(), px, bold, rgba, t.w, t.h, t.advance, false)) {
    t.tex = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, t.w, t.h);
    if (t.tex) {
      SDL_UpdateTexture(t.tex, nullptr, rgba.data(), t.w * 4);
      SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    }
  }
  return g_texts[key] = t;
}

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

int put(const std::string& s, int px, int x, int cy, uint32_t c, bool bold = false, bool centre = false) {
  const Text t = text(s, px, bold);
  if (!t.tex) return 0;
  SDL_SetTextureColorMod(t.tex, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
  SDL_Rect r{centre ? x - t.advance / 2 : x, cy - t.h / 2, t.w, t.h};
  SDL_RenderCopy(g_r, t.tex, nullptr, &r);
  return t.advance;
}

void button(const SDL_Rect& r, const std::string& label, int px, bool on, std::function<void()> fn, uint32_t fg = 0) {
  fill(r.x, r.y, r.w, r.h, on ? (hover(r) ? 0xB8C26C : kAccent) : (hover(r) ? kRowHover : kRow));
  put(fit(label, px, on, r.w - 8), px, r.x + r.w / 2, r.y + r.h / 2, fg ? fg : on ? kAccentText : kText, on, true);
  g_hits.push_back({r, std::move(fn)});
}

void apply_files() {
  mods_apply_files();
  mods_apply_flags();
}

void choose(const char* title, const char* label, const char* patterns, const char* key) {
  if (!can_choose_file()) return;
  const std::string k = key;
  choose_file(title, label, patterns, [k](const std::string& path) {
    if (path.empty()) return;
    later([k, path] {
      config_set(k.c_str(), path);
      if (k == "mod_cursor_file") config_set("mod_cursor", "custom");
      apply_files();
    });
  });
}

std::string base(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

void panel_draw(SDL_Renderer* r, int tab, int x, int y0, int col_w, int h, int px, int small, int row_h, int mx, int my) {
  if (r != g_r) {  // textures belong to one renderer
    for (auto& [k, v] : g_texts)
      if (v.tex) SDL_DestroyTexture(v.tex);
    g_texts.clear();
    g_r = r;
  }
  if (g_texts.size() > 400) {
    for (auto& [k, v] : g_texts)
      if (v.tex) SDL_DestroyTexture(v.tex);
    g_texts.clear();
  }
  {
    std::vector<std::function<void()>> todo;
    {
      std::lock_guard<std::mutex> l(g_mu);
      todo.swap(g_pending);
    }
    for (auto& fn : todo) fn();
  }
  g_mx = mx, g_my = my;
  g_hits.clear();
  int y = y0;
  const int gap = 6, bh = row_h - 14, pad = 14;
  auto row = [&](const std::string& label, const std::string& note, int reserve) {
    fill(x, y, col_w, row_h, kPanel);
    const int tw = col_w - pad - reserve - pad - 16;
    put(fit(label, px, false, tw), px, x + pad, y + row_h / 2 - (note.empty() ? 0 : px / 2), kText);
    if (!note.empty()) put(fit(note, small, false, tw), small, x + pad, y + row_h / 2 + px / 2 + 2, kDim);
  };
  auto next = [&] { y += row_h + gap; };
  const int bw = px * 5;
  auto onoff = [&](const char* key, const std::string& label, const std::string& note, bool fallback = false,
                   std::function<void()> after = {}) {
    row(label, note, bw);
    const bool on = config_bool(key, fallback);
    const std::string k = key;
    button({x + col_w - pad - bw, y + 7, bw, bh}, on ? "On" : "Off", px, on, [k, on, after] {
      config_set(k.c_str(), on ? "0" : "1");
      if (after) after();
    });
    next();
  };
  auto stepper = [&](const std::string& label, const std::string& note, const std::string& value, std::function<void()> left,
                     std::function<void()> right, int vw) {
    const int sw = bh + vw + bh;
    row(label, note, sw);
    const int rx = x + col_w - pad - sw;
    button({rx, y + 7, bh, bh}, "<", px, false, left);
    put(fit(value, px, true, vw - 4), px, rx + bh + vw / 2, y + row_h / 2, kText, true, true);
    button({rx + bh + vw, y + 7, bh, bh}, ">", px, false, right);
    next();
  };
  auto step_int = [](const char* key, int fallback, int lo, int hi, int step, int dir) {
    config_set(key, std::to_string(std::clamp(config_int(key, fallback) + dir * step, lo, hi)));
  };
  // A file mod: Choose... / Reset.
  auto file_row = [&](const char* key, const std::string& label, const char* title, const char* flabel, const char* patterns) {
    const std::string cur = config_get(key);
    const int cw = px * 6, rw = px * 5;
    row(label, cur.empty() ? "Roblox's own" : base(cur), cw + 6 + rw);
    const std::string k = key, t = title, fl = flabel, pt = patterns;
    button({x + col_w - pad - rw - 6 - cw, y + 7, cw, bh}, "Choose...", px, false,
           [k, t, fl, pt] { choose(t.c_str(), fl.c_str(), pt.c_str(), k.c_str()); });
    button({x + col_w - pad - rw, y + 7, rw, bh}, "Reset", px, false, [k] {
      config_set(k.c_str(), "");
      apply_files();
    });
    next();
  };

  if (tab == kTabPerformance) {
    onoff("mod_potato", "Potato graphics", "Lowest everything: no grass, smallest textures, simplest lighting", false, apply_files);
    onoff("mod_gray_sky", "Plain grey sky", "No skybox to draw", false, apply_files);
    onoff("mod_gamemode", "Game mode", [] {
      const std::string st = gamemode_status();
      if (!config_bool("mod_gamemode", true)) return std::string("Off");
      if (st == "missing") return std::string("Install GameMode first: sudo pacman -S gamemode");
      if (st.rfind("error", 0) == 0) return "GameMode: " + st;
      return std::string("While you play: CPU governor and priority up (Feral GameMode)");
    }(), true);
    {
      static const int kBg[] = {0, 60, 30, 15, 5};
      const int bg = config_int("mod_bg_fps", 0);
      auto step_bg = [](int dir) {
        const int cur = config_int("mod_bg_fps", 0);
        int i = 0;
        for (int k = 0; k < 5; ++k)
          if (kBg[k] == cur) i = k;
        config_set("mod_bg_fps", std::to_string(kBg[std::clamp(i + dir, 0, 4)]));
      };
      stepper("Frame rate when away", "While you use another window: less heat and power", bg ? std::to_string(bg) + " fps" : "Full",
              [step_bg] { step_bg(1); }, [step_bg] { step_bg(-1); }, px * 6);
    }
    // Upscalers for the resolution cap (upscale.hpp) and NVIDIA's frame
    // generation (framegen.hpp). One upscaler at a time.
    {
      const bool vk = renderer_setting() == "vulkan" && !config_bool("vulkan_failed", false);
      const bool capped = config_int("res_cap", 0) > 0;
      const std::string up = config_get("upscaler", "off");
      auto need = [&](const std::string& ok_note) {
        if (!vk) return std::string("Vulkan only (General: Renderer)");
        if (!capped) return std::string("Set a Resolution cap (General): it upscales that");
        return ok_note;
      };
      auto up_row = [&](const char* id, const std::string& label, const std::string& note, const char* sharp_key, int sharp_default) {
        const bool on = up == id;
        row(label, need(note), bw);
        const std::string v = id;
        button({x + col_w - pad - bw, y + 7, bw, bh}, on ? "On" : "Off", px, on, [v, on] { config_set("upscaler", on ? "off" : v); });
        next();
        if (!on) return;
        const int sv = std::clamp(config_int(sharp_key, sharp_default), 0, 100);
        const std::string k = sharp_key;
        stepper(label + ": sharpness", "How much it sharpens the enlarged picture", std::to_string(sv) + " %",
                [k, sharp_default, step_int] { step_int(k.c_str(), sharp_default, 0, 100, 10, -1); },
                [k, sharp_default, step_int] { step_int(k.c_str(), sharp_default, 0, 100, 10, 1); }, px * 5);
      };
      onoff("block_analytics", "Block Roblox analytics", "Analytics, telemetry, tracing and crash reports: not sent (like a DNS blocklist)");
      up_row("fsr", "FSR 1 upscaling", "AMD FidelityFX: the capped picture enlarged sharply (any GPU)", "fsr_sharpness", 80);
      up_row("nis", "NVIDIA Image Scaling", "NIS: the capped picture enlarged and sharpened (any GPU)", "nis_sharpness", 50);
      const SmoothMotionSupport sm = smooth_motion_support();
      const bool on = sm.ok && config_bool("smooth_motion", false);
      row("NVIDIA Smooth Motion", !sm.ok ? sm.why
                                  : !vk ? std::string("Frame generation: turns the renderer to Vulkan")
                                        : "Frame generation: about twice the frames shown, a little more latency",
          bw);
      button({x + col_w - pad - bw, y + 7, bw, bh}, !sm.ok ? "N/A" : on ? "On" : "Off", px, on, [ok = sm.ok, on] {
        if (!ok) return;
        config_set("smooth_motion", on ? "0" : "1");
        if (!on) {  // it needs Vulkan
          config_set("renderer", "vulkan");
          config_set("vulkan_failed", "0");
        }
      });
      next();
    }
  } else if (tab == kTabPicture) {
    static const char* filters[] = {"None", "Vivid", "Black & white", "Warm", "Cool", "Night light", "High contrast", "Retro", "Sepia"};
    const bool vk = renderer_setting() == "vulkan" && !config_bool("vulkan_failed", false);
    const int f = std::clamp(config_int("mod_filter", 0), 0, 8);
    stepper("Colour filter", vk ? "OpenGL only (General: Renderer)" : "On tonic's side: the game itself is not changed", filters[f],
            [step_int] { step_int("mod_filter", 0, 0, 8, 1, -1); }, [step_int] { step_int("mod_filter", 0, 0, 8, 1, 1); }, px * 8);
    const int sh = config_int("mod_sharpen", 0);
    stepper("Sharpen", vk ? "OpenGL only (General: Renderer)" : "Crisper edges; good with a resolution cap",
            sh ? std::to_string(sh) + " %" : "Off", [step_int] { step_int("mod_sharpen", 0, 0, 100, 10, -1); },
            [step_int] { step_int("mod_sharpen", 0, 0, 100, 10, 1); }, px * 6);
    {
      const std::string last = window_last_screenshot();
      const int ow = px * 8;
      row("Screenshots", last.empty() ? "F12 in game saves one to Pictures/tonic" : "Last: " + base(last), ow);
      button({x + col_w - pad - ow, y + 7, ow, bh}, "Open folder", px, false, [] {
        const char* home = getenv("HOME");
        const std::string dir = std::string(home ? home : ".") + "/Pictures/tonic";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        spawn_detached({"xdg-open", dir});
      });
      next();
    }
  } else if (tab == kTabSound) {
    const int vol = config_int("mod_volume", 100);
    stepper("Volume", "Above 100 % makes Roblox louder", std::to_string(vol) + " %",
            [step_int] { step_int("mod_volume", 100, 0, 200, 10, -1); }, [step_int] { step_int("mod_volume", 100, 0, 200, 10, 1); },
            px * 6);
    onoff("mod_mute_unfocused", "Mute when away", "Silent while you use another window");
    file_row("mod_death_sound", "Death sound", "Choose a death sound", "Sound", "*.ogg *.mp3 *.wav");
    file_row("mod_jump_sound", "Jump sound", "Choose a jump sound", "Sound", "*.ogg *.mp3 *.wav");
    file_row("mod_land_sound", "Landing sound", "Choose a landing sound", "Sound", "*.ogg *.mp3 *.wav");
    file_row("mod_step_sound", "Footsteps", "Choose a footstep sound", "Sound", "*.ogg *.mp3 *.wav");
  } else if (tab == kTabLook) {
    static const char* styles[][2] = {{"roblox", "Roblox"}, {"dot", "Dot"}, {"cross", "Cross"}, {"custom", "File..."}};
    const int cw = px * 4 + 6, n = 4, sw = n * cw + (n - 1) * 4;
    const std::string cur = config_get("mod_cursor", "roblox");
    row("Cursor", cur == "custom" ? base(config_get("mod_cursor_file")) : "The mouse cursor in games", sw);
    for (int i = 0; i < n; ++i) {
      const std::string id = styles[i][0];
      button({x + col_w - pad - sw + i * (cw + 4), y + 7, cw, bh}, styles[i][1], small, cur == id, [id] {
        if (id == "custom") return choose("Choose a cursor image", "PNG image", "*.png", "mod_cursor_file");
        config_set("mod_cursor", id);
        apply_files();
      });
    }
    next();
    file_row("mod_font", "Font", "Choose a font", "Font", "*.ttf *.otf");
    // What tonic tells Roblox about "the phone" (device.hpp): Roblox lays out
    // its own UI from it. Takes effect when Roblox next starts.
    {
      const int sz = device_ui_size();
      stepper("Roblox UI size", sz == 100 ? "Menus, buttons and the top bar, as Roblox makes them"
                                          : "Bigger menus and buttons",
              std::to_string(sz) + " %", [] { config_set("ui_size", std::to_string(device_ui_size_step(-1))); },
              [] { config_set("ui_size", std::to_string(device_ui_size_step(1))); }, px * 5);
    }
    auto pick2 = [&](const char* key, const char* fallback, const std::string& label, const std::string& note, const char* a,
                     const char* al, const char* b, const char* bl) {
      const int cw = px * 5, sw = cw * 2 + 4;
      row(label, note, sw);
      const std::string cur = config_get(key, fallback), k = key, va = a, vb = b;
      button({x + col_w - pad - sw, y + 7, cw, bh}, al, small, cur == a, [k, va] { config_set(k.c_str(), va); });
      button({x + col_w - pad - cw, y + 7, cw, bh}, bl, small, cur == b, [k, vb] { config_set(k.c_str(), vb); });
      next();
    };
    {
      // Margin from the window's edges (device.hpp): -1 auto, else dp.
      static const int kMargins[] = {-1, 0, 8, 16, 24, 32, 48};
      const int cur = config_int("ui_margin", -1);
      int idx = 0;
      for (int i = 0; i < 7; ++i)
        if (kMargins[i] == cur) idx = i;
      const std::string v = cur < 0 ? "Auto (" + std::to_string(device_ui_margin()) + ")" : std::to_string(cur);
      stepper("Edge margin", "Keeps Roblox's buttons and menus off the window's edges", v,
              [idx] { config_set("ui_margin", std::to_string(kMargins[std::max(0, idx - 1)])); },
              [idx] { config_set("ui_margin", std::to_string(kMargins[std::min(6, idx + 1)])); }, px * 6);
    }
    pick2("ui_layout", "phone", "Roblox layout", "Tablet: Roblox's tablet menus and layouts", "phone", "Phone",
          "tablet", "Tablet");
    {
      // The Roblox app takes its theme from your account (account.hpp).
      const int cw = px * 5, sw = cw * 2 + 4;
      const std::string st = account_status();
      row("Roblox theme", !st.empty() ? st : "Saved on your Roblox account (roblox.com follows it too); counts at Play", sw);
      static std::string cur;  // what Roblox has (its app storage), looked up when the page opens
      static Clock::time_point looked{};
      if (Clock::now() - looked > std::chrono::seconds(2)) cur = device_roblox_theme(), looked = Clock::now();
      for (int i = 0; i < 2; ++i) {
        const std::string v = i ? "light" : "dark";
        button({x + col_w - pad - sw + i * (cw + 4), y + 7, cw, bh}, i ? "Light" : "Dark", small, cur == v, [v] {
          config_set("roblox_theme", v);
          config_set("roblox_theme_pending", "1");  // Roblox's cached theme too, at Play (device.cpp)
          cur = v;
          account_set_theme(v);
        });
      }
      next();
    }
  } else if (tab == kTabControls) {
    // Sensitivity as a multiplier (config: percent; 100 = as Roblox has it).
    auto sens = [&](const char* key, const std::string& label, const std::string& note) {
      const int v = std::clamp(config_int(key, 100), 10, 500);
      char buf[16];
      snprintf(buf, sizeof buf, "%.1fx", v / 100.0);
      const std::string k = key;
      stepper(label, note, buf, [k, step_int] { step_int(k.c_str(), 100, 10, 500, 10, -1); },
              [k, step_int] { step_int(k.c_str(), 100, 10, 500, 10, 1); }, px * 5);
    };
    sens("sens_mouse", "Mouse sensitivity", "How far the camera turns for each mouse movement");
    sens("sens_pad_move", "Gamepad: movement stick", "The left stick: how far a small push walks you");
    sens("sens_pad_look", "Gamepad: camera stick", "The right stick: how fast the camera turns");
    onoff("rumble", "Rumble", "Games that vibrate the phone rumble your controller (from the next start)", true);
    onoff("anti_afk", "Caffeinate", "Away 5 minutes: the mouse moves 1 pixel and back, so Roblox does not disconnect you");
    {
      const int st = std::clamp(config_int("rumble_strength", 100), 0, 200);
      stepper("Rumble strength", "100 % is what the game asks for", std::to_string(st) + " %",
              [step_int] { step_int("rumble_strength", 100, 0, 200, 10, -1); },
              [step_int] { step_int("rumble_strength", 100, 0, 200, 10, 1); }, px * 5);
    }
  } else if (tab == kTabInstances) {
    static std::vector<Instance> list;
    static Clock::time_point listed{};
    if (Clock::now() - listed > std::chrono::seconds(1) || g_relist) {
      list = profile_list();
      listed = Clock::now();
      g_relist = false;
    }
    {
      const int nw = px * 8;
      row("New instance", "Another tonic with its own login: sign in there", nw);
      button({x + col_w - pad - nw, y + 7, nw, bh}, "Start new", px, true, [] {
        const std::string id = profile_new();
        g_instance_msg = profile_launch(id) ? "Instance " + id + " is starting" : "Could not start it";
        g_relist = true;
      });
      next();
    }
    for (const Instance& in : list) {
      const std::string title = in.username.empty() ? in.label : in.label + "  -  " + in.username;
      const std::string state = in.self ? "This window" : in.running ? "Running" : in.signed_in ? "Signed in" : "Not signed in yet";
      const int lw = px * 5 + 10, dw = px * 5;
      const bool can_delete = !in.id.empty() && !in.running;
      row(title, state, lw + (can_delete ? 6 + dw : 0));
      const std::string id = in.id;
      if (can_delete) {
        const bool confirm = g_delete_id == id;
        button({x + col_w - pad - dw, y + 7, dw, bh}, confirm ? "Sure?" : "Delete", px, false, [id, confirm] {
          if (!confirm) {
            g_delete_id = id;
            return;
          }
          g_delete_id = "-";
          g_instance_msg = profile_delete(id) ? "Instance " + id + " deleted" : "Could not delete it";
          g_relist = true;
        }, kDanger);
      }
      const int lx = x + col_w - pad - (can_delete ? dw + 6 : 0) - lw;
      if (in.running) {
        put(in.self ? "Here" : "Open", small, lx + lw / 2, y + row_h / 2, kAccent, true, true);
      } else {
        button({lx, y + 7, lw, bh}, "Launch", px, false, [id] {
          g_instance_msg =
              profile_launch(id) ? (id.empty() ? std::string("Main") : "Instance " + id) + " is starting" : "Could not start it";
          g_delete_id = "-";
          g_relist = true;
        });
      }
      next();
      if (y > y0 + h - row_h) break;
    }
    if (!g_instance_msg.empty()) put(g_instance_msg, small, x + 2, y + 10, kDim);
  }
}

bool panel_click(int mx, int my) {
  g_mx = mx, g_my = my;
  for (const Hit& h : g_hits)
    if (hover(h.r)) {
      auto fn = h.fn;
      fn();
      return true;
    }
  return false;
}

}  // namespace tonic
