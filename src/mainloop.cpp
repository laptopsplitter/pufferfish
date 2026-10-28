#include "mainloop.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <algorithm>

#include "fakejni.hpp"
#include "looper.hpp"
#include "script.hpp"

namespace tonic {

namespace {

// Never destroyed: the loop waits on it until the process exits, and
// destroying a condition variable that has a waiter blocks at exit.
struct Jobs {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> q;
};
Jobs& jobs() {
  static Jobs& j = *new Jobs;
  return j;
}

constexpr const char* kGL = "com/roblox/engine/jni/NativeGLInterface";
constexpr const char* kName = "nativeCallMessagesFromMainThread";

}  // namespace

void main_post(std::function<void()> fn) {
  Jobs& j = jobs();
  std::lock_guard<std::mutex> l(j.mu);
  j.q.push_back(std::move(fn));
  j.cv.notify_one();
  looper_wake_main();
}

void main_loop(const ElfImage& img) {
  void (*pump)(void*, void*) = nullptr;
  const char* off = getenv("TONIC_NO_MAIN_PUMP");
  if (!(off && *off == '1')) {
    uint64_t fn = jni_registered_native(kGL, kName, "()V");
    if (!fn) fn = reinterpret_cast<uint64_t>(img.find_symbol(jni_short_name(kGL, kName).c_str()));
    pump = reinterpret_cast<void (*)(void*, void*)>(fn);
  }
  // How often the main thread wakes when nothing wakes it: Android's runs
  // at the display's rate (vsync). TONIC_MAIN_HZ overrides; default 240.
  const char* hz_env = getenv("TONIC_MAIN_HZ");
  const int hz = hz_env && atoi(hz_env) > 0 ? atoi(hz_env) : 240;
  const int period_ms = std::max(1, 1000 / hz);
  fprintf(stderr, "[main] this thread is now the app's main loop (its looper, posted jobs%s, every %d ms)\n",
          pump ? ", nativeCallMessagesFromMainThread" : "", period_ms);
  void* env = jni_env();
  void* cls = jni_class(kGL);
  Jobs& j = jobs();
  for (;;) {
    // Waits on the looper's descriptors (the library's own wake-ups) and on
    // posted jobs (main_post wakes the looper), at most one period.
    looper_poll_main(period_ms);
    std::deque<std::function<void()>> now;
    {
      std::lock_guard<std::mutex> l(j.mu);
      now.swap(j.q);
    }
    for (auto& f : now) f();
    if (pump) pump(env, cls);
    fflush(stderr);
  }
}

}  // namespace tonic
