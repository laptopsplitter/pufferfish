#include "framegen.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "config.hpp"
#include "vkshim.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[framegen] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

bool g_active = false;
bool g_we_set = false;

std::string read_file(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The driver's layer: an implicit Vulkan layer manifest naming VK_LAYER_NV_present.
bool have_layer() {
  std::vector<std::string> dirs = {"/usr/share/vulkan/implicit_layer.d", "/etc/vulkan/implicit_layer.d",
                                   "/usr/local/share/vulkan/implicit_layer.d"};
  if (const char* x = getenv("XDG_DATA_HOME")) dirs.push_back(std::string(x) + "/vulkan/implicit_layer.d");
  if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.local/share/vulkan/implicit_layer.d");
  if (const char* x = getenv("XDG_DATA_DIRS")) {
    std::stringstream ss(x);
    for (std::string d; std::getline(ss, d, ':');)
      if (!d.empty()) dirs.push_back(d + "/vulkan/implicit_layer.d");
  }
  std::error_code ec;
  for (auto& d : dirs)
    for (auto& e : fs::directory_iterator(d, ec))
      if (e.path().extension() == ".json" && read_file(e.path()).find("VK_LAYER_NV_present") != std::string::npos) return true;
  return false;
}

// The NVIDIA GPU's name, from the driver (/proc/driver/nvidia/gpus/*/information).
std::string nvidia_model() {
  std::error_code ec;
  for (auto& e : fs::directory_iterator("/proc/driver/nvidia/gpus", ec)) {
    std::stringstream ss(read_file(e.path() / "information"));
    for (std::string line; std::getline(ss, line);)
      if (line.rfind("Model:", 0) == 0) {
        const size_t a = line.find_first_not_of(" \t", 6);
        return a == std::string::npos ? "" : line.substr(a);
      }
  }
  return {};
}

}  // namespace

SmoothMotionSupport smooth_motion_support() {
  static bool done = false;
  static SmoothMotionSupport s;
  if (done) return s;
  done = true;
  const std::string model = nvidia_model();
  if (model.empty()) {
    s.why = "Needs an NVIDIA GPU (RTX 40 series or newer) with NVIDIA's driver";
    return s;
  }
  // RTX 40 (Ada) and newer: GeForce RTX 40xx/50xx/..., RTX xxxx Ada, RTX PRO (Blackwell).
  // (GeForce only for the numbers: Quadro RTX 4000-8000 are older cards.)
  static const std::regex newer(R"re(GeForce\s+RTX\s+[4-9]0\d\d|\bAda\b|Blackwell|RTX\s+PRO)re", std::regex::icase);
  if (!std::regex_search(model, newer)) {
    s.why = "Needs an RTX 40 series or newer (this is " + model + ")";
    return s;
  }
  if (!have_layer()) {
    s.why = "Needs NVIDIA driver 575 or newer (no Smooth Motion layer found)";
    return s;
  }
  s.ok = true;
  s.why = model;
  return s;
}

void smooth_motion_apply() {
  const bool want = config_bool("smooth_motion", false);
  if (want && smooth_motion_support().ok && vulkan_enabled()) {
    setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", "1", 1);
    g_we_set = true;
    if (!g_active) LOG("NVIDIA Smooth Motion on (%s): about twice the frames shown; Roblox counts the real ones\n",
                       smooth_motion_support().why.c_str());
    g_active = true;
  } else {
    if (g_we_set) unsetenv("NVPRESENT_ENABLE_SMOOTH_MOTION");
    g_we_set = false;
    g_active = false;
    if (want && !smooth_motion_support().ok) LOG("Smooth Motion is on in Settings, but: %s\n", smooth_motion_support().why.c_str());
    else if (want) LOG("Smooth Motion needs the Vulkan renderer (Settings > General)\n");
  }
}

bool smooth_motion_active() { return g_active; }

}  // namespace tonic
