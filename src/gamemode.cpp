#include "gamemode.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <string>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[gamemode] " fmt, ##__VA_ARGS__)

std::mutex g_mu;
std::condition_variable g_cv;
bool g_want = false, g_thread = false;
std::string g_status = "off";

// GameMode's requests go over D-Bus and can take a moment: its own thread.
// A session bus to reach gamemoded over. Without one, GameMode's library
// trips a libdbus assertion, which aborts the whole process.
bool have_session_bus() {
  if (const char* a = getenv("DBUS_SESSION_BUS_ADDRESS"); a && *a) return true;
  const char* rt = getenv("XDG_RUNTIME_DIR");
  struct stat st{};
  return rt && stat((std::string(rt) + "/bus").c_str(), &st) == 0;
}

void worker() {
  if (!have_session_bus()) {
    LOG("no D-Bus session bus: GameMode cannot be asked\n");
    std::lock_guard<std::mutex> l(g_mu);
    g_status = "error: no D-Bus session";
    return;
  }
  setenv("DBUS_FATAL_WARNINGS", "0", 1);  // a libdbus warning must never abort tonic
  void* lib = dlopen("libgamemode.so.0", RTLD_NOW | RTLD_LOCAL);
  auto start = lib ? reinterpret_cast<int (*)()>(dlsym(lib, "real_gamemode_request_start")) : nullptr;
  auto end = lib ? reinterpret_cast<int (*)()>(dlsym(lib, "real_gamemode_request_end")) : nullptr;
  auto error = lib ? reinterpret_cast<const char* (*)()>(dlsym(lib, "real_gamemode_error_string")) : nullptr;
  if (!start || !end) {
    LOG("GameMode is not installed (libgamemode.so.0): nothing to do\n");
    std::lock_guard<std::mutex> l(g_mu);
    g_status = "missing";
    return;
  }
  bool on = false;
  for (;;) {
    bool want;
    {
      std::unique_lock<std::mutex> l(g_mu);
      g_cv.wait(l, [&] { return g_want != on; });
      want = g_want;
    }
    const int r = want ? start() : end();
    std::lock_guard<std::mutex> l(g_mu);
    if (r == 0 || !want) {
      on = want;
      g_status = on ? "on" : "off";
      LOG("%s\n", on ? "on: CPU governor and priority up while you play" : "off");
    } else {
      on = want;  // do not retry in a loop
      g_status = std::string("error: ") + (error ? error() : "request failed");
      LOG("%s\n", g_status.c_str());
    }
  }
}

}  // namespace

void gamemode_want(bool want) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_thread) {
    if (!want) return;  // nothing asked yet: no need to load anything
    g_thread = true;
    std::thread(worker).detach();
  }
  if (g_want != want) {
    g_want = want;
    g_cv.notify_all();
  }
}

std::string gamemode_status() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

}  // namespace tonic
