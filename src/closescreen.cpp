#include "closescreen.hpp"
#include "platform.hpp"

#include <png.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include "textoverlay.hpp"

namespace tonic {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kBg = 0x101216, kTrack = 0x2A2E36, kText = 0xF2F4F8, kDim = 0x8A909C, kAccent = 0xA6B05C, kRow = 0x20242C;

std::atomic<int> g_state{0};
std::mutex g_mu;
std::vector<uint8_t> g_img;
int g_w = 0, g_h = 0;
long g_gen = 0;
Clock::time_point g_drawn{}, g_since = Clock::now();

std::vector<uint8_t> g_logo;
int g_logo_w = 0, g_logo_h = 0;
bool g_logo_tried = false;

void load_logo() {
  g_logo_tried = true;
  png_image img{};
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, share_file("assets/logo.png").c_str())) return;
  img.format = PNG_FORMAT_RGBA;
  g_logo.resize(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, nullptr, g_logo.data(), 0, nullptr)) {
    g_logo.clear();
    return;
  }
  g_logo_w = static_cast<int>(img.width), g_logo_h = static_cast<int>(img.height);
}

struct Canvas {
  std::vector<uint8_t>& px;
  int w, h;
  void blend(int x, int y, uint32_t c, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    const float r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    p[0] = static_cast<uint8_t>(p[0] + (r - p[0]) * a);
    p[1] = static_cast<uint8_t>(p[1] + (g - p[1]) * a);
    p[2] = static_cast<uint8_t>(p[2] + (b - p[2]) * a);
  }
  void rect(int x, int y, int rw, int rh, uint32_t c) {
    for (int j = std::max(0, y); j < std::min(h, y + rh); ++j)
      for (int i = std::max(0, x); i < std::min(w, x + rw); ++i) blend(i, j, c, 1);
  }
  // Text centred on CX (or from its left edge), vertically centred on CY.
  void text(const std::string& s, int px_size, bool bold, int cx, int cy, uint32_t c, bool centre = true) {
    std::vector<uint8_t> t;
    int tw, th, adv;
    if (!ui_text_bitmap(s.c_str(), px_size, bold, t, tw, th, adv)) return;
    const int x0 = centre ? cx - adv / 2 : cx, y0 = cy - th / 2;
    for (int j = 0; j < th; ++j)
      for (int i = 0; i < tw; ++i) blend(x0 + i, y0 + j, c, t[(static_cast<size_t>(j) * tw + i) * 4 + 3] / 255.f);
  }
  void image(const std::vector<uint8_t>& src, int sw, int sh, int x0, int y0, int dw, int dh) {
    for (int j = 0; j < dh; ++j)
      for (int i = 0; i < dw; ++i) {  // bilinear, so the logo stays smooth
        const float sx = (i + 0.5f) * sw / dw - 0.5f, sy = (j + 0.5f) * sh / dh - 0.5f;
        const int ix = std::clamp(static_cast<int>(sx), 0, sw - 1), iy = std::clamp(static_cast<int>(sy), 0, sh - 1);
        const int ix1 = std::min(ix + 1, sw - 1), iy1 = std::min(iy + 1, sh - 1);
        const float fx = std::clamp(sx - ix, 0.f, 1.f), fy = std::clamp(sy - iy, 0.f, 1.f);
        float acc[4] = {0, 0, 0, 0};
        const int xs[2] = {ix, ix1}, ys[2] = {iy, iy1};
        const float wx[2] = {1 - fx, fx}, wy[2] = {1 - fy, fy};
        for (int a = 0; a < 2; ++a)
          for (int b = 0; b < 2; ++b) {
            const uint8_t* p = &src[(static_cast<size_t>(ys[a]) * sw + xs[b]) * 4];
            const float k = wy[a] * wx[b], al = p[3] / 255.f;
            for (int ch = 0; ch < 3; ++ch) acc[ch] += p[ch] * al * k;
            acc[3] += al * k;
          }
        if (acc[3] <= 0) continue;
        const uint32_t c = (static_cast<uint32_t>(acc[0] / acc[3]) << 16) | (static_cast<uint32_t>(acc[1] / acc[3]) << 8) |
                           static_cast<uint32_t>(acc[2] / acc[3]);
        blend(x0 + i, y0 + j, c, acc[3]);
      }
  }
};

// Where Cancel goes, as on the launcher: bottom right.
void cancel_rect(int W, int H, int& x, int& y, int& w, int& h) {
  const int bpx = std::max(13, H * 22 / 1000), m = std::max(12, H / 40);
  w = bpx * 7, h = bpx * 2 + 4, x = W - w - m, y = H - h - m;
}

void draw(int W, int H) {
  g_img.assign(static_cast<size_t>(W) * H * 4, 255);
  Canvas c{g_img, W, H};
  c.rect(0, 0, W, H, kBg);
  if (!g_logo_tried) load_logo();
  const int title_px = std::max(28, H * 9 / 100), sub_px = std::max(14, H * 25 / 1000);
  const int logo_h = g_logo.empty() ? 0 : std::min(H * 34 / 100, g_logo_h);
  const int logo_w = g_logo.empty() ? 0 : g_logo_w * logo_h / std::max(1, g_logo_h);
  const int gap = H * 3 / 100, bar_h = std::max(3, H / 200);
  const int block = logo_h + (logo_h ? gap : 0) + title_px + H * 3 / 100 + sub_px + H * 4 / 100 + bar_h + H * 25 / 1000 + sub_px;
  const int top = std::max(0, (H - block) / 2), cx = W / 2;
  if (logo_h > 0) c.image(g_logo, g_logo_w, g_logo_h, cx - logo_w / 2, top, logo_w, logo_h);
  int y = top + logo_h + (logo_h ? gap : 0);
  c.text("tonic", title_px, true, cx, y + title_px / 2, kText);
  y += title_px + H * 3 / 100;
  c.text("tonic is going for a shutdown", sub_px, false, cx, y + sub_px / 2, kText);
  y += sub_px + H * 4 / 100;
  // The loading bar: a segment sliding across the track, eased at the ends.
  const int bar_w = std::min(W * 40 / 100, 320), bx = cx - bar_w / 2;
  c.rect(bx, y, bar_w, bar_h, kTrack);
  const float t = std::fmod(std::chrono::duration<float>(Clock::now() - g_since).count(), 1.6f) / 1.6f;
  const float e = t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2.f) / 2;
  const int seg = bar_w * 30 / 100, sx = bx - seg + static_cast<int>((bar_w + seg) * e);
  const int s0 = std::max(bx, sx), s1 = std::min(bx + bar_w, sx + seg);
  if (s1 > s0) c.rect(s0, y, s1 - s0, bar_h, kAccent);
  y += bar_h + H * 25 / 1000;
  const int state = g_state;
  c.text(state == 1 ? "Closing in a moment" : "Roblox is saving your settings", sub_px, false, cx, y + sub_px / 2, kDim);
  if (state == 1) {
    int bx2, by, bw, bh;
    cancel_rect(W, H, bx2, by, bw, bh);
    c.rect(bx2, by, bw, bh, kRow);
    c.text("Cancel", std::max(13, H * 22 / 1000), false, bx2 + bw / 2, by + bh / 2, kText);
  }
}

}  // namespace

void closescreen_set(int state) {
  if (state && !g_state) g_since = Clock::now();
  g_state = state;
  std::lock_guard<std::mutex> l(g_mu);
  g_drawn = {};  // redraw now
}

int closescreen_state() { return g_state; }

bool closescreen_image(int w, int h, const std::vector<uint8_t>*& rgba, long& gen) {
  if (!g_state || w <= 0 || h <= 0) return false;
  std::lock_guard<std::mutex> l(g_mu);
  const auto now = Clock::now();
  if (w != g_w || h != g_h || now - g_drawn > std::chrono::milliseconds(33)) {
    g_w = w, g_h = h;
    g_drawn = now;
    draw(w, h);
    ++g_gen;
  }
  rgba = &g_img;
  gen = g_gen;
  return true;
}

bool closescreen_cancel_hit(int x, int y, int w, int h) {
  if (g_state != 1) return false;
  int bx, by, bw, bh;
  cancel_rect(w, h, bx, by, bw, bh);
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

}  // namespace tonic
