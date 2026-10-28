#include <thread>
#include <chrono>
#include <cstring>
#include <string>
#include "mainloop.hpp"
#include "audio.hpp"
#include "config.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <vector>

#include "fakejni.hpp"
#include "script.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[audio] " fmt, ##__VA_ARGS__)

constexpr const char* kDevice = "org/fmod/AudioDevice";

std::mutex g_mu;
SDL_AudioDeviceID g_dev = 0;
std::atomic<bool> g_focused{true};

// Volume from the mod panel (config mod_volume, 0-200 %; mod_mute_unfocused),
// read twice a second.
float gain() {
  static std::atomic<long long> checked{0};
  static std::atomic<int> volume{100};
  static std::atomic<bool> mute_away{false};
  const long long now = SDL_GetTicks64();
  if (now - checked > 500) {
    checked = now;
    volume = std::clamp(config_int("mod_volume", 100), 0, 200);
    mute_away = config_bool("mod_mute_unfocused", false);
  }
  if (mute_away && !g_focused) return 0.f;
  return volume / 100.f;
}
Uint32 g_block_bytes = 0;

uint64_t device_init(void*, const std::vector<uint64_t>& a) {
  const char* off = getenv("TONIC_NO_AUDIO");
  if (off && *off == '1') return 0;
  if (a.size() < 4) return 0;
  const int channels = static_cast<int>(a[0]), rate = static_cast<int>(a[1]), block = static_cast<int>(a[2]),
            blocks = static_cast<int>(a[3]);
  std::lock_guard<std::mutex> l(g_mu);
  if (g_dev) SDL_CloseAudioDevice(g_dev), g_dev = 0;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    LOG("SDL audio: %s\n", SDL_GetError());
    return 0;
  }
  SDL_AudioSpec want{}, have{};
  want.freq = rate;
  want.format = AUDIO_S16SYS;  // AudioFormat.ENCODING_PCM_16BIT
  want.channels = static_cast<Uint8>(channels);
  want.samples = static_cast<Uint16>(block);
  g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts if the device differs
  if (!g_dev) {
    LOG("cannot open an audio device: %s\n", SDL_GetError());
    return 0;
  }
  g_block_bytes = static_cast<Uint32>(block) * channels * 2;
  SDL_PauseAudioDevice(g_dev, 0);
  LOG("playing %d ch, %d Hz, %d-frame blocks x %d through SDL (%s)\n", channels, rate, block, blocks,
      SDL_GetCurrentAudioDriver());
  return 1;
}

uint64_t device_write(void*, const std::vector<uint64_t>& a) {
  if (a.size() < 2) return 0;
  SDL_AudioDeviceID dev;
  Uint32 block;
  {
    std::lock_guard<std::mutex> l(g_mu);
    dev = g_dev;
    block = g_block_bytes;
  }
  if (!dev) return 0;
  std::string pcm = jni_text(reinterpret_cast<void*>(a[0]));
  const size_t n = std::min<size_t>(pcm.size(), static_cast<size_t>(a[1] & 0x7fffffff));
  // AudioTrack.write blocks while its buffer is full; FMOD's mixer relies on
  // that for pacing. Keep about four blocks queued.
  const Uint32 limit = std::max<Uint32>(block, static_cast<Uint32>(n)) * 4;
  while (SDL_GetQueuedAudioSize(dev) > limit) SDL_Delay(1);
  const float g = gain();
  if (g != 1.f) {  // 16-bit samples
    int16_t* smp = reinterpret_cast<int16_t*>(pcm.data());
    for (size_t i = 0; i < n / 2; ++i) smp[i] = static_cast<int16_t>(std::clamp(smp[i] * g, -32768.f, 32767.f));
  }
  SDL_QueueAudio(dev, pcm.data(), static_cast<Uint32>(n));
  return 0;
}

uint64_t device_close(void*, const std::vector<uint64_t>&) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_dev) SDL_CloseAudioDevice(g_dev), g_dev = 0;
  LOG("closed\n");
  return 0;
}

}  // namespace

void audio_set_focused(bool focused) { g_focused = focused; }

void audio_init() {
  register_java_method(std::string(kDevice) + ".init(IIII)Z", device_init);
  register_java_method(std::string(kDevice) + ".write([BI)V", device_write);
  register_java_method(std::string(kDevice) + ".close()V", device_close);
}

void display_rates_report(const ElfImage& img) {
  constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";
  SDL_Window* win = SDL_GL_GetCurrentWindow();
  int disp = 0;
  if (!win) {
    // The probe's window is the only one; find its display through the focus.
    SDL_Window* w = SDL_GetKeyboardFocus();
    if (w) disp = SDL_GetWindowDisplayIndex(w);
  } else {
    disp = SDL_GetWindowDisplayIndex(win);
  }
  if (disp < 0) disp = 0;
  SDL_DisplayMode cur{};
  float current = 60;
  if (SDL_GetCurrentDisplayMode(disp, &cur) == 0 && cur.refresh_rate > 0) current = static_cast<float>(cur.refresh_rate);
  // Display.getSupportedRefreshRates: the rates of the modes at this resolution.
  std::vector<float> rates;
  for (int i = 0, n = SDL_GetNumDisplayModes(disp); i < n; ++i) {
    SDL_DisplayMode m{};
    if (SDL_GetDisplayMode(disp, i, &m) != 0 || m.refresh_rate <= 0) continue;
    if (cur.w && (m.w != cur.w || m.h != cur.h)) continue;
    const float r = static_cast<float>(m.refresh_rate);
    if (std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
  }
  if (std::find(rates.begin(), rates.end(), current) == rates.end()) rates.push_back(current);
  std::sort(rates.begin(), rates.end());
  std::string shown;
  for (float r : rates) shown += (shown.empty() ? "" : ", ") + std::to_string(static_cast<int>(std::lround(r)));
  fprintf(stderr, "[display] refresh %.0f Hz; supported: %s\n", current, shown.c_str());
  void* env = jni_env();
  if (auto f = reinterpret_cast<void (*)(void*, void*, void*)>(find_native(img, kGL, "nativePassSupportedRefreshRates", "([F)V")))
    f(env, jni_class(kGL), jni_new_float_array(rates));
  if (auto f = reinterpret_cast<void (*)(void*, void*, float)>(
          find_native(img, kGL, "nativePassCurrentDisplayRefreshRate", "(F)V")))
    f(env, jni_class(kGL), current);
}

namespace {

// The battery as Android's BatteryManager reports it: level 0-100 and status
// (2 charging, 3 discharging, 4 not charging, 5 full). A desktop has none:
// report it as a full battery on mains power, so the engine's power saving
// (it lowers the frame rate on battery) stays off. A laptop reports its own.
void read_battery(int& level, int& status) {
  level = 100;
  status = 5;
  for (int i = 0; i < 4; ++i) {
    const std::string dir = "/sys/class/power_supply/BAT" + std::to_string(i) + "/";
    FILE* f = fopen((dir + "capacity").c_str(), "r");
    if (!f) continue;
    if (fscanf(f, "%d", &level) != 1) level = 100;
    fclose(f);
    char s[32] = {0};
    if ((f = fopen((dir + "status").c_str(), "r"))) {
      if (!fgets(s, sizeof s, f)) s[0] = 0;
      fclose(f);
    }
    status = !strncmp(s, "Charging", 8) ? 2 : !strncmp(s, "Discharging", 11) ? 3 : !strncmp(s, "Not charging", 12) ? 4 : 5;
    return;
  }
}

}  // namespace

void power_state_report(const ElfImage& img) {
  constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";
  // PowerManager.getCurrentThermalStatus() (wh/y0): 0, THERMAL_STATUS_NONE.
  if (auto f = reinterpret_cast<void (*)(void*, void*, int)>(find_native(img, kGL, "reportThermalStateChanged", "(I)V")))
    f(jni_env(), jni_class(kGL), 0);
  // ACTION_BATTERY_CHANGED (wh/h.n): reported at start and whenever it changes.
  static auto battery = reinterpret_cast<void (*)(void*, void*, int, int)>(
      find_native(img, kGL, "reportBatteryStateChanged", "(II)V"));
  if (!battery) return;
  static int last_level = -1, last_status = -1;
  int level, status;
  read_battery(level, status);
  battery(jni_env(), jni_class(kGL), level, status);
  last_level = level, last_status = status;
  fprintf(stderr, "[power] thermal status none; battery %d%%, status %d\n", level, status);
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      int l, s;
      read_battery(l, s);
      if (l == last_level && s == last_status) continue;
      last_level = l, last_status = s;
      main_post([l, s] { battery(jni_env(), jni_class("com/roblox/engine/jni/NativeGLInterface"), l, s); });
    }
  }).detach();
}

}  // namespace tonic
