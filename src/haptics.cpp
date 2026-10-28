#include "haptics.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "fakejni.hpp"
#include "mainloop.hpp"
#include "script.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[rumble] " fmt, ##__VA_ARGS__)
using Clock = std::chrono::steady_clock;

constexpr const char* kBus = "com/roblox/universalapp/messagebus/MessageBus";
constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";
constexpr const char* kProtocol = "HapticProtocol";
// One motor, as a phone with one vibrator reports it.
constexpr const char* kMotors = "{\"defaultMotor\":{\"x\":0,\"y\":0,\"z\":0}}";

const ElfImage* g_img = nullptr;
std::mutex g_mu;
float g_steady = 0;              // UpdateSingletonVibration
std::vector<float> g_wave;       // SetMotorState: intensities, one per step
float g_wave_ms = 0;
Clock::time_point g_wave_t0;
std::atomic<bool> g_running{false};

void* lookup(const char* cls, const char* name, const char* desc) {
  if (uint64_t fn = jni_registered_native(cls, name, desc)) return reinterpret_cast<void*>(fn);
  if (void* p = g_img->find_symbol(jni_short_name(cls, name).c_str())) return p;
  return g_img->find_symbol(jni_long_name(cls, name, desc).c_str());
}

double num(const std::string& j, const std::string& key, double fallback) {
  const size_t k = j.find("\"" + key + "\"");
  if (k == std::string::npos) return fallback;
  size_t p = j.find(':', k);
  if (p == std::string::npos) return fallback;
  p = j.find_first_not_of(" \t\"", p + 1);
  return p == std::string::npos ? fallback : strtod(j.c_str() + p, nullptr);
}

// Every controller (SDL's own lock: safe from any thread).
void rumble_all(float level, int ms) {
  const float strength = std::clamp(config_int("rumble_strength", 100), 0, 200) / 100.f;
  const float v = std::clamp(level * strength, 0.f, 1.f);
  const Uint16 low = static_cast<Uint16>(v * 0xFFFF), high = static_cast<Uint16>(v * 0.7f * 0xFFFF);
  SDL_LockJoysticks();
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (!SDL_IsGameController(i)) continue;
    if (SDL_GameController* gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i)))
      SDL_GameControllerRumble(gc, low, high, static_cast<Uint32>(ms));
  }
  SDL_UnlockJoysticks();
}

// The level now: the steady vibration, or the motor pattern while it plays.
float level_now() {
  std::lock_guard<std::mutex> l(g_mu);
  float v = g_steady;
  if (!g_wave.empty() && g_wave_ms > 0) {
    const double t = std::chrono::duration<double, std::milli>(Clock::now() - g_wave_t0).count();
    const size_t i = static_cast<size_t>(t / g_wave_ms);
    if (i < g_wave.size()) v = std::max(v, g_wave[i]);
    else g_wave.clear();
  }
  return v;
}

// Keeps the controllers' rumble at level_now() (refreshed: SDL's rumble lasts a set time).
void worker() {
  float last = 0;
  for (;;) {
    const float v = config_bool("rumble", true) ? level_now() : 0.f;
    if (v > 0.01f && last <= 0.01f) {
      static int logged = 0;
      if (logged++ < 3) LOG("the game vibrates (%.0f %%): rumbling %d controller(s)\n", v * 100, SDL_NumJoysticks());
    }
    if (v > 0.01f) rumble_all(v, 120);
    else if (last > 0.01f) rumble_all(0, 0);
    last = v;
    std::this_thread::sleep_for(std::chrono::milliseconds(v > 0.01f ? 16 : 50));
  }
}

void respond(const std::string& method, const std::string& json) {
  main_post([method, json] {
    auto pub = reinterpret_cast<void (*)(void*, void*, void*, void*, void*, int, void*)>(
        lookup(kBus, "publishProtocolMethodResponseRaw", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;)V"));
    if (pub)
      pub(jni_env(), jni_new_object(kBus, "hapticBus"), jni_new_string(kProtocol), jni_new_string(method), jni_new_string(json), 0,
          jni_new_string("{}"));
  });
}

void on_request(const std::string& method, const std::string& json) {
  if (method == "SupportsHaptics") return respond(method, "{\"supportsHaptics\":true}");
  if (method == "GetMotors") return respond(method, kMotors);
  if (method == "UpdateSingletonVibration") {
    std::lock_guard<std::mutex> l(g_mu);
    g_steady = static_cast<float>(std::clamp(num(json, "intensity", 0), 0.0, 1.0));
    return;
  }
  if (method == "SetMotorState") {
    // {"defaultMotor":{"millisecondsPerSample":N,"intensities":[...]}}
    std::vector<float> wave;
    const size_t a = json.find("\"intensities\"");
    if (a != std::string::npos) {
      size_t p = json.find('[', a);
      const size_t e = json.find(']', p);
      while (p != std::string::npos && e != std::string::npos && p < e && wave.size() < 10000) {
        ++p;
        char* end = nullptr;
        const double v = strtod(json.c_str() + p, &end);
        if (end == json.c_str() + p) break;
        wave.push_back(static_cast<float>(std::clamp(v, 0.0, 1.0)));
        p = json.find(',', end - json.c_str());
        if (p > e) break;
      }
    }
    std::lock_guard<std::mutex> l(g_mu);
    g_wave = std::move(wave);
    g_wave_ms = static_cast<float>(std::clamp(num(json, "millisecondsPerSample", 0), 0.0, 10000.0));
    g_wave_t0 = Clock::now();
  }
}

const char* const kMethods[] = {"SupportsHaptics", "UpdateSingletonVibration", "GetMotors", "SetMotorState"};

}  // namespace

void haptics_init() {
  add_jni_call_listener([](const std::string& label, const std::string& method, const std::vector<uint64_t>& raw) {
    if (label.rfind("haptic:", 0) != 0 || method != "run" || raw.empty()) return;
    on_request(label.substr(7), jni_text(reinterpret_cast<void*>(raw[0])));
  });
}

void haptics_enable(const ElfImage& img) {
  if (g_img) return;
  g_img = &img;
  if (!config_bool("rumble", true)) {
    LOG("off (Settings > Controls)\n");
    return;
  }
  auto sub = reinterpret_cast<void* (*)(void*, void*, void*, void*, void*, uint8_t)>(
      lookup(kBus, "doSubscribeProtocolMethodRequestRaw",
             "(Ljava/lang/String;Ljava/lang/String;Lcom/roblox/universalapp/messagebus/RawCallback;Z)"
             "Lcom/roblox/universalapp/messagebus/Connection;"));
  auto state = reinterpret_cast<void (*)(void*, void*, uint8_t, void*)>(
      lookup(kGL, "nativeUpdateMobileHapticsState", "(ZLjava/lang/String;)V"));
  if (!sub) {
    LOG("Roblox's message bus has no protocol subscriptions here: no rumble\n");
    return;
  }
  void* env = jni_env();
  for (const char* m : kMethods)
    sub(env, jni_new_object(kBus, "hapticBus"), jni_new_string(kProtocol), jni_new_string(m),
        jni_new_object("com/roblox/universalapp/messagebus/RawCallback", std::string("haptic:") + m), 0);
  if (state) state(env, jni_class(kGL), 1, jni_new_string(kMotors));  // "this phone can vibrate"
  if (!g_running.exchange(true)) std::thread(worker).detach();
  LOG("game vibration goes to your controllers%s\n", state ? "" : " (haptics state not reported)");
}

}  // namespace tonic
