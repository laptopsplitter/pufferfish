#include "appicon.hpp"
#include "platform.hpp"

#include <SDL.h>
#include <png.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tonic {

namespace {

const char* const kLogo = "assets/icon.png";  // (square, transparent)
constexpr int kSize = 256;  // the icon's size; desktops scale it down as they need

// The logo scaled to fit a kSize square (bilinear), centred on transparency:
// the logo is taller than wide, and icons are square.
const std::vector<uint8_t>& icon_pixels() {
  static std::vector<uint8_t> out;
  static bool tried = false;
  if (tried) return out;
  tried = true;
  png_image img{};
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, share_file(kLogo).c_str())) return out;
  img.format = PNG_FORMAT_RGBA;
  std::vector<uint8_t> src(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, nullptr, src.data(), 0, nullptr)) {
    png_image_free(&img);
    return out;
  }
  const int sw = static_cast<int>(img.width), sh = static_cast<int>(img.height);
  if (sw <= 0 || sh <= 0) return out;
  const double scale = static_cast<double>(kSize) / std::max(sw, sh);
  const int dw = std::max(1, static_cast<int>(sw * scale)), dh = std::max(1, static_cast<int>(sh * scale));
  const int ox = (kSize - dw) / 2, oy = (kSize - dh) / 2;
  out.assign(static_cast<size_t>(kSize) * kSize * 4, 0);
  for (int y = 0; y < dh; ++y) {
    const double fy = std::clamp((y + 0.5) / scale - 0.5, 0.0, sh - 1.0);
    const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, sh - 1);
    const double ty = fy - y0;
    for (int x = 0; x < dw; ++x) {
      const double fx = std::clamp((x + 0.5) / scale - 0.5, 0.0, sw - 1.0);
      const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, sw - 1);
      const double tx = fx - x0;
      uint8_t* d = &out[(static_cast<size_t>(oy + y) * kSize + ox + x) * 4];
      const auto at = [&](int xx, int yy, int c) { return src[(static_cast<size_t>(yy) * sw + xx) * 4 + c]; };
      // Weighted by alpha, so transparent edges do not darken the colours.
      double a = 0, rgb[3] = {0, 0, 0};
      const double w[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
      const int px[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
      for (int k = 0; k < 4; ++k) {
        const double pa = at(px[k][0], px[k][1], 3) * w[k];
        a += pa;
        for (int c = 0; c < 3; ++c) rgb[c] += at(px[k][0], px[k][1], c) * pa;
      }
      if (a > 0)
        for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::clamp(rgb[c] / a + 0.5, 0.0, 255.0));
      d[3] = static_cast<uint8_t>(std::clamp(a + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

}  // namespace

void app_icon_set(SDL_Window* window) {
  if (!window) return;
  const std::vector<uint8_t>& px = icon_pixels();
  if (px.empty()) return;
  // (SDL copies the pixels; the surface only lends them)
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint8_t*>(px.data()), kSize, kSize, 32, kSize * 4,
                                                      SDL_PIXELFORMAT_ABGR8888);
  if (!s) return;
  SDL_SetWindowIcon(window, s);
  SDL_FreeSurface(s);
}

std::string app_icon_path() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::absolute(share_file(kLogo), ec);
  if (ec || !std::filesystem::exists(p, ec)) return "";
  return p.string();
}

}  // namespace tonic
