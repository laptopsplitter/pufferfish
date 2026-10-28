#include "game.hpp"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <string>

#include "device.hpp"
#include "fakejni.hpp"
#include "haptics.hpp"
#include "mainloop.hpp"
#include "recent.hpp"
#include "script.hpp"
#include "textoverlay.hpp"
#include "usersettings.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[game] " fmt, ##__VA_ARGS__)

constexpr const char* kBus = "com/roblox/universalapp/messagebus/MessageBus";
constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";
constexpr const char* kParams = "com/roblox/engine/jni/autovalue/StartGameParams";

const ElfImage* g_img = nullptr;
std::mutex g_mu;
std::string g_username;
bool g_under13 = false;
int g_launches = 0;
bool g_in_game = false;  // main loop only
std::atomic<int> g_games_started{0};  // games launched (window.cpp: TONIC_FPS_AVG)

void* lookup(const char* cls, const char* name, const char* desc) {
  if (uint64_t fn = jni_registered_native(cls, name, desc)) return reinterpret_cast<void*>(fn);
  if (void* p = g_img->find_symbol(jni_short_name(cls, name).c_str())) return p;
  void* p = g_img->find_symbol(jni_long_name(cls, name, desc).c_str());
  if (!p) LOG("%s.%s%s not found\n", cls, name, desc);
  return p;
}

// Flat JSON reading: "key": "string" | number | true/false.
bool find_value(const std::string& j, const std::string& key, size_t& at) {
  const std::string k = "\"" + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return false;
  p = j.find(':', p + k.size());
  if (p == std::string::npos) return false;
  at = j.find_first_not_of(" \t\r\n", p + 1);
  return at != std::string::npos;
}
std::string get_string(const std::string& j, const std::string& key) {
  size_t p;
  if (!find_value(j, key, p) || j[p] != '"') return {};
  std::string out;
  for (++p; p < j.size() && j[p] != '"'; ++p) {
    if (j[p] == '\\' && p + 1 < j.size()) ++p;
    out += j[p];
  }
  return out;
}
long long get_number(const std::string& j, const std::string& key) {
  size_t p;
  if (!find_value(j, key, p)) return 0;
  if (j[p] == '"') ++p;  // numbers sometimes arrive as strings
  return strtoll(j.c_str() + p, nullptr, 10);
}

void launch(const std::string& json) {
  const long long place = get_number(json, "placeId");
  const long long user = get_number(json, "userId");
  const long long conversation = get_number(json, "conversationId");
  const std::string access = get_string(json, "accessCode"), link = get_string(json, "linkCode"),
                    instance = get_string(json, "gameInstanceId"),
                    reserved = get_string(json, "reservedServerAccessCode");
  // wh/i0.a: follow a user 1; private server (access or link code) 2; a given
  // server 3; reserved server 8; otherwise join the place 0.
  int type = 0;
  if (user > 0) type = 1;
  else if (!access.empty() || !link.empty()) type = 2;
  else if (!instance.empty()) type = 3;
  else if (!reserved.empty()) type = 8;
  if (place <= 0 && user <= 0) {
    LOG("Game.launch without a place or user; ignored\n");
    return;
  }
  const std::string label = "launch" + std::to_string(++g_launches);
  const std::string k = std::string(kParams) + "#" + label + ".";
  set_java_value_int(k + "placeId()J", place);
  set_java_value_int(k + "userId()J", user);
  set_java_value_int(k + "conversationId()J", conversation);
  set_java_value_int(k + "referredByPlayerId()J", get_number(json, "referredByPlayerId"));
  set_java_value_int(k + "joinRequestType()I", type);
  std::string username;
  bool under13;
  {
    std::lock_guard<std::mutex> l(g_mu);
    username = g_username;
    under13 = g_under13;
  }
  set_java_value_int(k + "isUnder13()Z", under13);
  set_java_value_string(k + "username()Ljava/lang/String;", username);
  for (const char* f : {"accessCode", "linkCode", "reservedServerAccessCode", "callId", "referralPage", "launchData",
                        "joinAttemptId", "joinAttemptOrigin", "isoContext", "gameJoinContext", "eventId",
                        "gameIdToExclude"})
    set_java_value_string(k + f + "()Ljava/lang/String;", get_string(json, f));
  set_java_value_string(k + "gameId()Ljava/lang/String;", instance);
  set_java_value_new(k + "surface()Landroid/view/Surface;");
  set_java_value_new(k + "platformParams()Lcom/roblox/engine/jni/model/PlatformParams;");
  LOG("launching place %lld (join type %d)\n", place, type);
  if (place > 0) recent_add(place);
  auto start = reinterpret_cast<int (*)(void*, void*, void*)>(
      lookup(kGL, "nativeAppBridgeV2StartGameWithParam", "(Lcom/roblox/engine/jni/autovalue/StartGameParams;)I"));
  if (!start) return;
  const int r = start(jni_env(), jni_class(kGL), jni_new_object(kParams, label));
  LOG("nativeAppBridgeV2StartGameWithParam returned %d\n", r);
  g_in_game = true;
  ++g_games_started;
  device_send_insets(*g_img);  // the game's UI keeps off the window's edges too
}

}  // namespace

int game_launches() { return g_games_started.load(); }

void game_surface_changed(int wmm, int hmm) {
  if (!g_img) return;  // the app has not started yet: StartApp gets the size
  constexpr const char* kPP = "com/roblox/engine/jni/model/PlatformParams";
  set_java_value_int(std::string(kPP) + ".viewportWidthMm:I", wmm);
  set_java_value_int(std::string(kPP) + ".viewportHeightMm:I", hmm);
  void* env = jni_env();
  // wh/a.surfaceChanged -> wh/e.H: the app's surface.
  if (auto app = reinterpret_cast<void (*)(void*, void*, void*, void*)>(
          lookup(kGL, "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams",
                 "(Landroid/view/Surface;Lcom/roblox/engine/jni/model/PlatformParams;)V")))
    app(env, jni_class(kGL), jni_new_object("android/view/Surface"), jni_new_object(kPP));
  // wh/h0.G: the game's surface, once a game runs (no VR activity).
  if (g_in_game)
    if (auto game = reinterpret_cast<void (*)(void*, void*, void*, void*, void*)>(
            lookup(kGL, "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams",
                   "(Landroid/view/Surface;Lcom/roblox/engine/jni/model/PlatformParams;Landroid/app/Activity;)V")))
      game(env, jni_class(kGL), jni_new_object("android/view/Surface"), jni_new_object(kPP), nullptr);
}

void game_shutdown() {
  if (!g_img) return;
  // As the Android app closing does: the game's surface goes (wh/h0.F:
  // PauseGame), the game is left (h0: LeaveGame), the app stops (wh/e.G:
  // PauseApp). Roblox saves its settings (GlobalBasicSettings_13.xml, with
  // the Maximum Frame Rate you picked) on the way; exiting at once lost them.
  const long long before = usersettings_mtime();
  auto* done = new std::atomic<bool>(false);
  main_post([done] {
    void* env = jni_env();
    void* cls = jni_class(kGL);
    if (g_in_game) {
      if (auto f = reinterpret_cast<void (*)(void*, void*)>(lookup(kGL, "nativeAppBridgeV2PauseGame", "()V"))) f(env, cls);
      if (auto f = reinterpret_cast<void (*)(void*, void*)>(lookup(kGL, "nativeAppBridgeV2LeaveGame", "()V"))) f(env, cls);
      g_in_game = false;
    }
    if (auto f = reinterpret_cast<void (*)(void*, void*)>(lookup(kGL, "nativeAppBridgeV2PauseApp", "()V"))) f(env, cls);
    *done = true;
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!*done && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // Settings are written by the library's own threads: give them a moment.
  while (std::chrono::steady_clock::now() < until && usersettings_mtime() == before)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  LOG("closed; settings %s\n", usersettings_mtime() != before ? "saved" : "not written (nothing changed, or Roblox did not save)");
}

void game_quick_join(long long place, long long user, const std::string& link_code) {
  std::string code;
  for (char c : link_code)
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') code += c;
  if (code.size() != link_code.size() || code.size() > 64) code.clear();  // (it goes into JSON as is)
  std::thread([place, user, code] {
    // As if Play were pressed on that game: once the app is up and logged in.
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    for (;;) {
      bool logged_in;
      {
        std::lock_guard<std::mutex> l(g_mu);
        logged_in = !g_username.empty();
      }
      if (g_img && logged_in && textoverlay_ready()) break;
      if (std::chrono::steady_clock::now() > until) {
        LOG("quick join of place %lld given up: not logged in or not ready\n", place);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));  // let the home screen settle
    LOG("quick join: %s%s%s\n", place > 0 ? ("place " + std::to_string(place)).c_str() : "", user > 0 ? " following a user" : "",
        code.empty() ? "" : " (private server)");
    std::string j = "{\"placeId\":" + std::to_string(place);
    if (user > 0) j += ",\"userId\":" + std::to_string(user);
    if (!code.empty()) j += ",\"linkCode\":\"" + code + "\"";
    j += ",\"joinAttemptOrigin\":\"tonicQuickJoin\"}";
    main_post([j] { launch(j); });
  }).detach();
}

void game_init() {
  add_jni_call_listener([](const std::string& label, const std::string& method, const std::vector<uint64_t>& raw) {
    if (label != "gameLaunch" || method != "run" || raw.empty()) return;
    const std::string json = jni_text(reinterpret_cast<void*>(raw[0]));
    main_post([json] { launch(json); });
  });
  set_jni_observer([](const std::string& method, const std::vector<uint64_t>& raw) {
    // onDataModelNotificationCallback("DID_LOG_IN", {"username": ..., "isUnder13": ...})
    if (method != "onDataModelNotificationCallback" || raw.size() < 2) return;
    if (jni_text(reinterpret_cast<void*>(raw[0])) != "DID_LOG_IN") return;
    const std::string j = jni_text(reinterpret_cast<void*>(raw[1]));
    std::lock_guard<std::mutex> l(g_mu);
    g_username = get_string(j, "username");
    g_under13 = j.find("\"isUnder13\":true") != std::string::npos;
    recent_set_owner(g_username);  // (signed in on Roblox's own screen)
  });
}

void game_enable(const ElfImage& img) {
  static bool done = false;
  if (done) return;
  done = true;
  g_img = &img;
  auto msg_id = reinterpret_cast<void* (*)(void*, void*, void*, void*)>(
      lookup(kBus, "getMessageId", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"));
  auto subscribe = reinterpret_cast<void* (*)(void*, void*, void*, void*, uint8_t)>(
      lookup(kBus, "doSubscribeRaw",
             "(Ljava/lang/String;Lcom/roblox/universalapp/messagebus/RawCallback;Z)"
             "Lcom/roblox/universalapp/messagebus/Connection;"));
  if (!msg_id || !subscribe) return;
  void* env = jni_env();
  void* id = msg_id(env, jni_class(kBus), jni_new_string("Game"), jni_new_string("launch"));
  subscribe(env, jni_new_object(kBus, "gameBus"), id,
            jni_new_object("com/roblox/universalapp/messagebus/RawCallback", "gameLaunch"), 0);
  LOG("listening for %s (Play)\n", jni_text(id).c_str());
  haptics_enable(img);
  device_send_insets(img);
}

}  // namespace tonic
