#include "looper.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>

#include "shims.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[looper] " fmt, ##__VA_ARGS__)

using Callback = int (*)(int fd, int events, void* data);

// ALOOPER_EVENT_* and ALOOPER_POLL_*
constexpr int kIn = 1, kOut = 2, kErr = 4, kHup = 8;
constexpr int kPollWake = -1, kPollCallback = -2, kPollTimeout = -3, kPollError = -4;

struct Request {
  int ident, events;
  Callback cb;
  void* data;
};
struct Looper {
  int ep = -1, wake = -1;
  std::atomic<int> refs{1};
  std::mutex mu;
  std::map<int, Request> fds;
};

thread_local Looper* t_looper = nullptr;
Looper* g_main = nullptr;

Looper* create() {
  auto* l = new Looper;
  l->ep = epoll_create1(EPOLL_CLOEXEC);
  l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = l->wake;
  epoll_ctl(l->ep, EPOLL_CTL_ADD, l->wake, &ev);
  return l;
}

uint32_t to_epoll(int events) {
  uint32_t e = 0;
  if (events & kIn) e |= EPOLLIN;
  if (events & kOut) e |= EPOLLOUT;
  return e;
}
int from_epoll(uint32_t e) {
  return (e & EPOLLIN ? kIn : 0) | (e & EPOLLOUT ? kOut : 0) | (e & EPOLLERR ? kErr : 0) | (e & EPOLLHUP ? kHup : 0);
}

// One wait. Callbacks run here; an fd registered with an ident (no callback)
// is returned to the caller, as Android does.
int poll_once(Looper* l, int timeout_ms, int* out_fd, int* out_events, void** out_data) {
  epoll_event evs[16];
  const int n = epoll_wait(l->ep, evs, 16, timeout_ms);
  if (n < 0) return errno == EINTR ? kPollWake : kPollError;
  if (n == 0) return kPollTimeout;
  int result = kPollWake;
  for (int i = 0; i < n; ++i) {
    const int fd = evs[i].data.fd;
    if (fd == l->wake) {
      uint64_t v;
      while (read(l->wake, &v, sizeof v) > 0) {}
      continue;
    }
    Request r;
    {
      std::lock_guard<std::mutex> g(l->mu);
      auto it = l->fds.find(fd);
      if (it == l->fds.end()) continue;
      r = it->second;
    }
    const int events = from_epoll(evs[i].events);
    if (r.cb) {
      if (r.cb(fd, events, r.data) == 0) {  // 0: unregister
        std::lock_guard<std::mutex> g(l->mu);
        auto it = l->fds.find(fd);
        if (it != l->fds.end() && it->second.cb == r.cb) {
          l->fds.erase(it);
          epoll_ctl(l->ep, EPOLL_CTL_DEL, fd, nullptr);
        }
      }
      result = kPollCallback;
    } else {
      if (out_fd) *out_fd = fd;
      if (out_events) *out_events = events;
      if (out_data) *out_data = r.data;
      return r.ident;
    }
  }
  return result;
}

// --- the NDK functions --------------------------------------------------------

void* sh_ALooper_forThread() { return t_looper; }
void* sh_ALooper_prepare(int) {
  if (!t_looper) t_looper = create();
  return t_looper;
}
void sh_ALooper_acquire(void* p) {
  if (p) ++static_cast<Looper*>(p)->refs;
}
void sh_ALooper_release(void* p) {
  if (p) --static_cast<Looper*>(p)->refs;  // kept: the library may still hold fds on it
}
int sh_ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events, void** out_data) {
  if (!t_looper) return kPollError;
  return poll_once(t_looper, timeout_ms, out_fd, out_events, out_data);
}
int sh_ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events, void** out_data) {
  if (!t_looper) return kPollError;
  for (;;) {
    const int r = poll_once(t_looper, timeout_ms, out_fd, out_events, out_data);
    if (r != kPollCallback) return r;
  }
}
void sh_ALooper_wake(void* p) {
  if (!p) return;
  const uint64_t one = 1;
  if (write(static_cast<Looper*>(p)->wake, &one, sizeof one) < 0) {}
}
int sh_ALooper_addFd(void* p, int fd, int ident, int events, Callback cb, void* data) {
  auto* l = static_cast<Looper*>(p);
  if (!l || fd < 0) return -1;
  epoll_event ev{};
  ev.events = to_epoll(events);
  ev.data.fd = fd;
  std::lock_guard<std::mutex> g(l->mu);
  const bool had = l->fds.count(fd);
  l->fds[fd] = Request{cb ? kPollCallback : ident, events, cb, data};
  if (epoll_ctl(l->ep, had ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev) != 0) {
    l->fds.erase(fd);
    return -1;
  }
  static std::atomic<int> logged{0};
  if (logged++ < 8) LOG("fd %d added to the %s looper (%s)\n", fd, l == g_main ? "main" : "a thread's",
                        cb ? "callback" : "ident");
  return 1;
}
int sh_ALooper_removeFd(void* p, int fd) {
  auto* l = static_cast<Looper*>(p);
  if (!l) return -1;
  std::lock_guard<std::mutex> g(l->mu);
  if (!l->fds.erase(fd)) return 0;
  epoll_ctl(l->ep, EPOLL_CTL_DEL, fd, nullptr);
  return 1;
}

}  // namespace

void register_looper_shims() {
  register_shim("ALooper_forThread", reinterpret_cast<void*>(&sh_ALooper_forThread));
  register_shim("ALooper_prepare", reinterpret_cast<void*>(&sh_ALooper_prepare));
  register_shim("ALooper_acquire", reinterpret_cast<void*>(&sh_ALooper_acquire));
  register_shim("ALooper_release", reinterpret_cast<void*>(&sh_ALooper_release));
  register_shim("ALooper_pollOnce", reinterpret_cast<void*>(&sh_ALooper_pollOnce));
  register_shim("ALooper_pollAll", reinterpret_cast<void*>(&sh_ALooper_pollAll));
  register_shim("ALooper_wake", reinterpret_cast<void*>(&sh_ALooper_wake));
  register_shim("ALooper_addFd", reinterpret_cast<void*>(&sh_ALooper_addFd));
  register_shim("ALooper_removeFd", reinterpret_cast<void*>(&sh_ALooper_removeFd));
}

void looper_prepare_main_thread() {
  sh_ALooper_prepare(0);
  g_main = t_looper;
}

void looper_poll_main(int timeout_ms) {
  if (!g_main) return;
  // Run every ready callback, then return (the caller has other work too).
  int r = poll_once(g_main, timeout_ms, nullptr, nullptr, nullptr);
  while (r == kPollCallback) r = poll_once(g_main, 0, nullptr, nullptr, nullptr);
}

void looper_wake_main() {
  if (g_main) sh_ALooper_wake(g_main);
}

}  // namespace tonic
