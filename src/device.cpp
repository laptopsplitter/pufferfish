#include "device.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "config.hpp"
#include "elfimg.hpp"
#include "fakejni.hpp"
#include "framegen.hpp"
#include "script.hpp"
#include "weblogin.hpp"

namespace tonic {

#define LOG(fmt, ...) fprintf(stderr, "[device] " fmt, ##__VA_ARGS__)

namespace {
// (Not under 100: Roblox's rounded corners (UICorner) come out square there.)
const int kSizes[] = {100, 160, 170, 180, 190, 200, 260, 280, 300};
}

int device_ui_size() {
  const int v = config_int("ui_size", 100);
  int best = 100;
  for (int s : kSizes)
    if (std::abs(s - v) < std::abs(best - v) || (std::abs(s - v) == std::abs(best - v) && s < best)) best = s;
  return best;
}

int device_ui_size_step(int dir) {
  const int cur = device_ui_size(), n = sizeof kSizes / sizeof kSizes[0];
  for (int i = 0; i < n; ++i)
    if (kSizes[i] == cur) return kSizes[std::clamp(i + dir, 0, n - 1)];
  return 100;
}

std::string device_roblox_theme() {
  if (config_bool("roblox_theme_pending", false)) return config_get("roblox_theme", "dark");
  std::string t = app_storage_get("AuthenticatedTheme");
  for (char& c : t) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  if (t == "light" || t == "dark") return t;
  return config_get("roblox_theme", "dark") == "light" ? "light" : "dark";
}

namespace {
std::atomic<float> g_dpi{1.f};
}

float device_dpi_scale() { return g_dpi; }

int device_ui_margin() {
  const int m = config_int("ui_margin", -1);
  if (m < 0) return device_ui_size() > 100 ? 16 : 0;
  return std::clamp(m, 0, 64);
}

void device_send_insets(const ElfImage& img) {
  constexpr const char* kBus = "com/roblox/universalapp/messagebus/MessageBus";
  const char* desc = "(Ljava/lang/String;Ljava/lang/String;)V";
  void* fn = reinterpret_cast<void*>(jni_registered_native(kBus, "publishRaw", desc));
  if (!fn) fn = img.find_symbol(jni_short_name(kBus, "publishRaw").c_str());
  if (!fn) fn = img.find_symbol(jni_long_name(kBus, "publishRaw", desc).c_str());
  auto get_id = reinterpret_cast<void* (*)(void*, void*, void*, void*)>(
      img.find_symbol(jni_short_name(kBus, "getMessageId").c_str()));
  if (!get_id)
    if (uint64_t r = jni_registered_native(kBus, "getMessageId", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"))
      get_id = reinterpret_cast<void* (*)(void*, void*, void*, void*)>(r);
  if (!fn || !get_id) return;
  const int m = device_ui_margin();
  const std::string json = "{\"left\":" + std::to_string(m) + ",\"top\":" + std::to_string(m) + ",\"right\":" +
                           std::to_string(m) + ",\"bottom\":" + std::to_string(m) + "}";
  void* env = jni_env();
  void* id = get_id(env, jni_class(kBus), jni_new_string("Screen"), jni_new_string("safeAreaInsets"));
  reinterpret_cast<void (*)(void*, void*, void*, void*)>(fn)(env, jni_new_object(kBus, "screenBus"), id, jni_new_string(json));
  static int logged = 0;
  if (logged++ < 2) LOG("margin from the window's edges: %d dp\n", m);
}

void device_apply_settings() {
  const int size = device_ui_size();
  g_dpi = size / 100.f;
  const bool tablet = config_get("ui_layout", "phone") == "tablet";
  const std::string theme = config_get("roblox_theme", "dark") == "light" ? "light" : "dark";

  set_java_value_double("com/roblox/engine/jni/model/PlatformParams.dpiScale:F", size / 100.0);
  set_java_value_int("com/roblox/engine/jni/autovalue/InitParams.isTablet()Z", tablet);
  std::string ua = java_value_string("com/roblox/engine/jni/autovalue/InitParams.userAgent()Ljava/lang/String;");
  if (const size_t p = ua.find(tablet ? " Phone " : " Tablet "); p != std::string::npos)
    ua.replace(p, tablet ? 7 : 8, tablet ? " Tablet " : " Phone ");
  if (!ua.empty()) set_java_value_string("com/roblox/engine/jni/autovalue/InitParams.userAgent()Ljava/lang/String;", ua);
  set_java_value_string("com/roblox/engine/jni/user/NativeUserJavaInterface.getTheme()Ljava/lang/String;", theme);
  set_java_value_string("com/roblox/engine/jni/autovalue/StartAppParams.selectedTheme()Ljava/lang/String;", theme);
  // A theme picked in tonic since Roblox last ran: Roblox starts with the
  // theme it cached (AuthenticatedTheme; a per-device choice, DeviceLevelTheme,
  // wins over it), and would only notice the account's new one a start later.
  if (config_bool("roblox_theme_pending", false)) {
    app_storage_set("AuthenticatedTheme", theme == "light" ? "Light" : "Dark");
    app_storage_erase("DeviceLevelTheme");
    config_set("roblox_theme_pending", "0");
    LOG("Roblox starts %s (its cached theme updated)\n", theme.c_str());
  }
  LOG("Roblox's UI: size %d %%, %s layout, %s theme\n", size, tablet ? "tablet" : "phone", theme.c_str());
  smooth_motion_apply();  // NVIDIA frame generation, before Roblox's Vulkan starts
}

}  // namespace tonic
