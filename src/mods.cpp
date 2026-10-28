#include "mods.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

#include "apk.hpp"
#include "config.hpp"
#include "vkshim.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[mods] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// --- file mods: copy in, keep the original, put it back -----------------------------

struct Ctx {
  std::string assets, originals;
};

bool ctx(Ctx& c) {
  RobloxInstall ri;
  if (!apk_current(ri)) return false;
  c.assets = ri.assets;
  c.originals = "data/mods/originals/" + (ri.version.empty() ? std::string("manual") : ri.version);
  return true;
}

// Puts DATA at assets/REL, keeping Roblox's own file the first time.
void place(const Ctx& c, const std::string& rel, const std::string& data) {
  std::error_code ec;
  const fs::path dst = fs::path(c.assets) / rel, keep = fs::path(c.originals) / rel;
  if (!fs::exists(keep, ec)) {
    fs::create_directories(keep.parent_path(), ec);
    if (fs::exists(dst, ec)) fs::copy_file(dst, keep, ec);
    else std::ofstream(keep.string() + ".absent");  // nothing to restore: delete on undo
  }
  fs::create_directories(dst.parent_path(), ec);
  std::ofstream(dst, std::ios::binary | std::ios::trunc) << data;
}

// Puts Roblox's own file back at assets/REL (if a mod replaced it).
void restore(const Ctx& c, const std::string& rel) {
  std::error_code ec;
  const fs::path dst = fs::path(c.assets) / rel, keep = fs::path(c.originals) / rel;
  if (fs::exists(keep, ec)) {
    fs::copy_file(keep, dst, fs::copy_options::overwrite_existing, ec);
    fs::remove(keep, ec);
  } else if (fs::exists(keep.string() + ".absent", ec)) {
    fs::remove(dst, ec);
    fs::remove(keep.string() + ".absent", ec);
  }
}

// --- cursors, drawn here (tonic's own art) -------------------------------------------

std::string png(int w, int h, const std::vector<uint8_t>& rgba) {
  png_image img{};
  img.version = PNG_IMAGE_VERSION;
  img.width = w, img.height = h, img.format = PNG_FORMAT_RGBA;
  png_alloc_size_t size = 0;
  png_image_write_to_memory(&img, nullptr, &size, 0, rgba.data(), 0, nullptr);
  std::string out(size, '\0');
  if (!png_image_write_to_memory(&img, out.data(), &size, 0, rgba.data(), 0, nullptr)) return {};
  out.resize(size);
  return out;
}

// A 64x64 cursor centred on the image (Roblox centres its cursor images).
std::string cursor(const std::string& style, bool far) {
  const int n = 64;
  std::vector<uint8_t> px(n * n * 4, 0);
  auto blend = [&](int x, int y, float a, uint8_t v) {
    if (x < 0 || y < 0 || x >= n || y >= n || a <= 0) return;
    uint8_t* p = &px[(y * n + x) * 4];
    const float na = std::min(1.f, a), oa = p[3] / 255.f, ra = na + oa * (1 - na);
    p[0] = p[1] = p[2] = static_cast<uint8_t>((v * na + p[0] * oa * (1 - na)) / std::max(ra, 1e-3f));
    p[3] = static_cast<uint8_t>(ra * 255);
  };
  const float c = (n - 1) / 2.f;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float dx = x - c, dy = y - c, d = std::sqrt(dx * dx + dy * dy);
      if (style == "dot") {
        const float r = far ? 3.5f : 4.5f;
        blend(x, y, std::clamp(r + 1.5f - d, 0.f, 1.f), 0);        // dark ring
        blend(x, y, std::clamp(r - d, 0.f, 1.f), 255);             // white dot
      } else {  // cross: four short bars around a gap, dark outlined
        const float len = far ? 9 : 11, gap = 3, half = 1.f;
        auto bar = [&](float ax, float ay) {  // distance-ish test for each arm
          return (std::fabs(ax) <= half && ay >= gap && ay <= len);
        };
        auto in = [&](float px_, float py_) {
          return bar(px_, py_) || bar(px_, -py_) || bar(py_, px_) || bar(py_, -px_);
        };
        bool outline = false;
        for (int oy = -1; oy <= 1 && !outline; ++oy)
          for (int ox = -1; ox <= 1; ++ox)
            if (in(dx + ox, dy + oy)) outline = true;
        if (outline) blend(x, y, 0.85f, 0);
        if (in(dx, dy)) blend(x, y, 1, 255);
      }
    }
  return png(n, n, px);
}

const char* kCursorFiles[] = {"content/textures/Cursors/KeyboardMouse/ArrowCursor.png",
                              "content/textures/Cursors/KeyboardMouse/ArrowFarCursor.png",
                              "android/textures/Cursors/KeyboardMouse/ArrowCursor.png",
                              "android/textures/Cursors/KeyboardMouse/ArrowFarCursor.png"};
// Sound mods: config key -> the sound Roblox plays (both places it looks).
struct SoundMod {
  const char* key;
  const char* file;  // under sounds/
  const char* name;
};
const SoundMod kSounds[] = {{"mod_death_sound", "ouch.ogg", "death sound"},
                            {"mod_jump_sound", "action_jump.mp3", "jump sound"},
                            {"mod_land_sound", "action_jump_land.mp3", "landing sound"},
                            {"mod_step_sound", "action_footsteps_plastic.mp3", "footsteps"}};

// The font mod: every font family points at content/fonts/CustomFont.ttf.
void font(const Ctx& c, const std::string& file) {
  std::error_code ec;
  const fs::path fam = fs::path(c.assets) / "content/fonts/families";
  std::vector<std::string> jsons;
  for (auto& e : fs::directory_iterator(fam, ec))
    if (e.path().extension() == ".json") jsons.push_back("content/fonts/families/" + e.path().filename().string());
  if (file.empty()) {
    for (auto& j : jsons) restore(c, j);
    restore(c, "content/fonts/CustomFont.ttf");
    return;
  }
  const std::string data = slurp(file);
  if (data.empty()) return;
  place(c, "content/fonts/CustomFont.ttf", data);
  static const std::regex asset(R"re(rbxasset://fonts/[^"]+\.(ttf|otf))re");
  for (auto& j : jsons) {
    const fs::path keep = fs::path(c.originals) / j;
    const std::string original = fs::exists(keep, ec) ? slurp(keep.string()) : slurp((fs::path(c.assets) / j).string());
    place(c, j, std::regex_replace(original, asset, "rbxasset://fonts/CustomFont.ttf"));
  }
}

// --- flags --------------------------------------------------------------------------

// Rendering flags, both F and DF forms (Roblox uses one; the other is ignored).
std::map<std::string, std::string> wanted_flags() {
  std::map<std::string, std::string> f;
  auto both = [&](const char* kind, const char* name, const char* v) {
    f[std::string(kind) + name] = v;
    f[std::string("DF") + std::string(kind).substr(1) + name] = v;
  };
  if (config_bool("mod_potato", false)) {
    both("FInt", "DebugFRMQualityLevelOverride", "1");  // graphics quality 1 of 21
    both("FInt", "DebugForceMSAASamples", "0");
    both("FFlag", "TextureQualityOverrideEnabled", "True");
    both("FInt", "TextureQualityOverride", "0");
    both("FInt", "DebugTextureManagerSkipMips", "4");   // smaller textures: less VRAM and bandwidth
    both("FInt", "TerrainArraySliceSize", "4");
    both("FInt", "FRMMinGrassDistance", "0");
    both("FInt", "FRMMaxGrassDistance", "0");
    both("FInt", "CSGLevelOfDetailSwitchingDistance", "0");
    both("FInt", "CSGLevelOfDetailSwitchingDistanceL12", "0");
    both("FInt", "CSGLevelOfDetailSwitchingDistanceL23", "0");
    both("FInt", "CSGLevelOfDetailSwitchingDistanceL34", "0");
    both("FInt", "RenderLocalLightUpdatesMax", "1");
    both("FInt", "RenderLocalLightUpdatesMin", "1");
  }
  if (config_bool("mod_gray_sky", false)) both("FFlag", "DebugSkyGray", "True");
  // Renderer (vkshim.hpp): Vulkan preferred when tonic offers it this run;
  // otherwise Roblox is told to keep to OpenGL.
  if (vulkan_enabled()) {
    both("FFlag", "DebugGraphicsPreferVulkan", "True");
    both("FFlag", "DebugGraphicsDisableVulkan", "False");
    both("FInt", "GraphicsVulkanMinAndroidVersion", "0");
  } else {
    both("FFlag", "DebugGraphicsDisableVulkan", "True");
    both("FFlag", "DebugGraphicsPreferVulkan", "False");
  }
  return f;
}

}  // namespace

void mods_apply_files() {
  Ctx c;
  if (!ctx(c)) return;
  const std::string style = config_get("mod_cursor", "roblox");
  if (style == "dot" || style == "cross") {
    for (const char* f : kCursorFiles) place(c, f, cursor(style, std::string(f).find("Far") != std::string::npos));
  } else if (style == "custom" && !config_get("mod_cursor_file").empty()) {
    const std::string data = slurp(config_get("mod_cursor_file"));
    for (const char* f : kCursorFiles) place(c, f, data);
  } else {
    for (const char* f : kCursorFiles) restore(c, f);
  }
  for (const SoundMod& m : kSounds) {
    const std::string mine = config_get(m.key), data = mine.empty() ? "" : slurp(mine);
    for (const char* dir : {"content/sounds/", "android/sounds/"}) {
      if (!data.empty()) place(c, std::string(dir) + m.file, data);
      else restore(c, std::string(dir) + m.file);
    }
  }
  font(c, config_get("mod_font"));
  LOG("files: %s\n", mods_summary().c_str());
}

namespace {

const char* kFlagNames =
    "DebugFRMQualityLevelOverride|DebugForceMSAASamples|TextureQualityOverrideEnabled|TextureQualityOverride|"
    "DebugTextureManagerSkipMips|TerrainArraySliceSize|FRMMinGrassDistance|FRMMaxGrassDistance|"
    "CSGLevelOfDetailSwitchingDistance(L12|L23|L34)?|RenderLocalLightUpdatesMax|RenderLocalLightUpdatesMin|DebugSkyGray|"
    "DebugGraphicsPreferVulkan|DebugGraphicsDisableVulkan|GraphicsVulkanMinAndroidVersion";

// Puts FLAGS into the JSON object that follows ANCHOR in PATH (the first
// object when ANCHOR is empty), after taking out every flag a mod sets. A
// value Roblox's own settings (SERVER) had for one of them goes back in when
// the mod is off.
bool edit_flags(const std::string& path, const std::string& anchor, const std::map<std::string, std::string>& flags,
                const std::string& server) {
  std::string json = slurp(path);
  if (json.empty()) return false;
  const std::regex ours(std::string(R"re(\s*"((DF|F)(Int|Flag)()re") + kFlagNames + R"re())"\s*:\s*"[^"]*"\s*,?)re");
  json = std::regex_replace(json, ours, "");
  json = std::regex_replace(json, std::regex(R"(,(\s*)\})"), "$1}");
  std::map<std::string, std::string> put = flags;
  if (!server.empty()) {  // Roblox's own values for the flags no mod sets now
    const std::regex any(std::string(R"re("((DF|F)(Int|Flag)()re") + kFlagNames + R"re())"\s*:\s*"([^"]*)")re");
    for (std::sregex_iterator it(server.begin(), server.end(), any), e; it != e; ++it)
      if (!put.count((*it)[1])) put[(*it)[1]] = (*it)[6];
  }
  std::string add;
  for (auto& [k, v] : put) add += (add.empty() ? "" : ", ") + std::string("\"") + k + "\": \"" + v + "\"";
  if (!add.empty()) {
    size_t open = anchor.empty() ? json.find('{') : json.find(anchor);
    if (!anchor.empty() && open != std::string::npos) open = json.find('{', open);
    if (open == std::string::npos) return false;
    const bool empty = json.find_first_not_of(" \t\r\n", open + 1) == json.find('}', open);
    json.insert(open + 1, add + (empty ? "" : ", "));
  }
  std::ofstream(path, std::ios::trunc) << json;
  return true;
}

}  // namespace

void mods_apply_flags() {
  const auto f = wanted_flags();
  // Both ways tonic hands Roblox its settings: the preload overrides and the
  // client settings (where Roblox's own server values are).
  const bool a = edit_flags("data/settings/preload-overrides.json", "", f, "");
  const bool b = edit_flags("data/settings/client-settings.json", "\"applicationSettings\"", f,
                            slurp("data/settings/AndroidApp.json"));
  if (!a && !b) {
    if (!f.empty()) LOG("data/settings missing: graphics mods not applied\n");
    return;
  }
  if (!f.empty()) {
    std::string names;
    for (auto& [k, v] : f)
      if (k[0] == 'F') names += (names.empty() ? "" : " ") + k + "=" + v;
    LOG("rendering flags handed to Roblox at start (%s%s): %s\n", a ? "preload" : "", b ? (a ? " + client settings" : "client settings") : "",
        names.c_str());
  }
}

std::string mods_summary() {
  std::string s;
  auto add = [&](const std::string& x) { s += (s.empty() ? "" : ", ") + x; };
  if (config_bool("mod_potato", false)) add("potato graphics");
  if (config_bool("mod_gray_sky", false)) add("grey sky");
  const std::string cur = config_get("mod_cursor", "roblox");
  if (cur != "roblox") add("cursor " + cur);
  for (const SoundMod& m : kSounds)
    if (!config_get(m.key).empty()) add(m.name);
  if (!config_get("mod_font").empty()) add("font");
  return s.empty() ? "none" : s;
}

}  // namespace tonic
