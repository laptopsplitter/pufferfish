// netping.cpp - see netping.hpp.
#include "netping.hpp"
#include "spawn.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tonic {
namespace {

using Clock = std::chrono::steady_clock;

struct Peer {
  sockaddr_storage a{};
  socklen_t len = 0;
  unsigned count = 0;
};
constexpr int kPeers = 8;

std::mutex g_mu;
Peer g_window[kPeers];          // what the game sent to in the current few seconds
Clock::time_point g_window_at;
Peer g_server;                  // the game server: the most sent to in the last window
Clock::time_point g_server_seen;
std::atomic<bool> g_want{false}, g_started{false};
std::atomic<float> g_ms{-1};

bool same(const sockaddr_storage& x, socklen_t xl, const sockaddr* y, socklen_t yl) {
  return xl == yl && memcmp(&x, y, xl) == 0;
}

// Not the game server: this machine, your network, multicast, DNS and the web.
bool worth_noting(const sockaddr* a, socklen_t len) {
  if (a->sa_family == AF_INET && len >= sizeof(sockaddr_in)) {
    const auto* s = reinterpret_cast<const sockaddr_in*>(a);
    const uint32_t ip = ntohl(s->sin_addr.s_addr);
    const unsigned port = ntohs(s->sin_port);
    if (port == 53 || port == 80 || port == 443 || port == 853 || port == 5353 || port == 1900 || port == 0) return false;
    const unsigned a0 = ip >> 24, a1 = (ip >> 16) & 0xff;
    if (a0 == 0 || a0 == 10 || a0 == 127 || a0 >= 224) return false;
    if (a0 == 172 && a1 >= 16 && a1 < 32) return false;
    if (a0 == 192 && a1 == 168) return false;
    if (a0 == 169 && a1 == 254) return false;
    if (a0 == 100 && a1 >= 64 && a1 < 128) return false;
    return true;
  }
  if (a->sa_family == AF_INET6 && len >= sizeof(sockaddr_in6)) {
    const auto* s = reinterpret_cast<const sockaddr_in6*>(a);
    const unsigned port = ntohs(s->sin6_port);
    if (port == 53 || port == 80 || port == 443 || port == 853 || port == 5353 || port == 1900 || port == 0) return false;
    const uint8_t* b = s->sin6_addr.s6_addr;
    if (IN6_IS_ADDR_LOOPBACK(&s->sin6_addr) || IN6_IS_ADDR_UNSPECIFIED(&s->sin6_addr)) return false;
    if (b[0] == 0xff || (b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)) return false;
    return true;
  }
  return false;
}

std::string ip_of(const Peer& p, bool& v6) {
  char buf[INET6_ADDRSTRLEN] = {};
  v6 = p.a.ss_family == AF_INET6;
  if (v6) inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&p.a)->sin6_addr, buf, sizeof buf);
  else inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&p.a)->sin_addr, buf, sizeof buf);
  return buf;
}

// "time=12.3 ms" in ping's reply line (Linux and FreeBSD alike).
float parse_time(const std::string& out) {
  const size_t t = out.find("time=");
  if (t == std::string::npos) return -1;
  return static_cast<float>(atof(out.c_str() + t + 5));
}

// The round trip of a TCP connection attempt to the server on PORT: its answer,
// a connection or a refusal, takes one trip there and back. -1: no answer.
float tcp_rtt(const Peer& p, int port) {
  sockaddr_storage a = p.a;
  if (a.ss_family == AF_INET6) reinterpret_cast<sockaddr_in6*>(&a)->sin6_port = htons(static_cast<uint16_t>(port));
  else reinterpret_cast<sockaddr_in*>(&a)->sin_port = htons(static_cast<uint16_t>(port));
  const int fd = socket(a.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  const auto t0 = Clock::now();
  float ms = -1;
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), p.len) == 0) {
    // (connected at once: something local, not the server)
  } else if (errno == EINPROGRESS) {
    pollfd pf{fd, POLLOUT, 0};
    if (poll(&pf, 1, 1500) == 1) {
      const float t = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
      // A connection or a refusal is the server's answer. Under a millisecond is
      // not: no server out on the internet is that close, so something on this
      // machine or network answered for it (a firewall or a proxy).
      if ((err == 0 || err == ECONNREFUSED) && t >= 1.f) ms = t;
    }
  }
  close(fd);
  return ms;
}

void pinger() {
  std::string last_ip;
  int fails = 0, tcp_fails = 0;
  bool icmp = true;  // ping(8) first; a TCP connection attempt when the server does not answer it
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!g_want) continue;
    Peer p;
    bool fresh;
    {
      std::lock_guard<std::mutex> l(g_mu);
      p = g_server;
      fresh = p.len && Clock::now() - g_server_seen < std::chrono::seconds(10);
    }
    if (!fresh) {
      g_ms = -1;
      continue;
    }
    bool v6;
    const std::string ip = ip_of(p, v6);
    if (ip != last_ip) {
      last_ip = ip;
      fails = tcp_fails = 0;
      icmp = true;
      fprintf(stderr, "[ping] game server %s: its round trip is shown in the stats overlay\n", ip.c_str());
    }
    if (icmp) {
      std::vector<std::string> cmd = {"ping", "-c", "1"};
      if (v6) cmd.push_back("-6");
#if defined(__FreeBSD__) || defined(__OpenBSD__)
      cmd.insert(cmd.end(), {"-t", "2"});  // (FreeBSD: -t is the whole wait, in seconds)
#else
      cmd.insert(cmd.end(), {"-W", "2"});  // (Linux: -W is the wait for the reply, in seconds)
#endif
      cmd.push_back(ip);
      int st = -1;
      const auto t0 = Clock::now();
      const std::string out = spawn_read(cmd, 4, &st);
      const float ms = parse_time(out);
      if (ms >= 0) {
        g_ms = ms;
        fails = 0;
        continue;
      }
      const bool missing = st == 127 || (out.empty() && st < 0 && Clock::now() - t0 < std::chrono::milliseconds(500));
      if (!missing && ++fails < 2) continue;
      icmp = false;
      fprintf(stderr, "[ping] %s %s; timing a TCP connection attempt to it instead\n", ip.c_str(),
              missing ? "cannot be pinged here (no ping command)" : "does not answer ping");
    }
    const unsigned game_port = ntohs(p.a.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6*>(&p.a)->sin6_port
                                                               : reinterpret_cast<const sockaddr_in*>(&p.a)->sin_port);
    float ms = tcp_rtt(p, static_cast<int>(game_port));
    if (ms < 0) ms = tcp_rtt(p, 443);
    if (ms >= 0) {
      g_ms = ms;
      tcp_fails = 0;
    } else if (++tcp_fails == 2) {
      g_ms = -2;
      fprintf(stderr, "[ping] %s answers neither ping nor a TCP connection attempt: shown as no reply\n", ip.c_str());
    } else if (tcp_fails > 2) {
      g_ms = -2;
    }
  }
}

}  // namespace

void netping_note(const sockaddr* a, socklen_t len) {
  if (!a || len < sizeof(sa_family_t) * 2 || !worth_noting(a, len)) return;
  if (len > sizeof(sockaddr_storage)) len = sizeof(sockaddr_storage);
  const auto now = Clock::now();
  {
    std::lock_guard<std::mutex> l(g_mu);
    int slot = -1, least = 0;
    for (int i = 0; i < kPeers; ++i) {
      if (g_window[i].len && same(g_window[i].a, g_window[i].len, a, len)) {
        slot = i;
        break;
      }
      if (g_window[i].count < g_window[least].count) least = i;
    }
    if (slot < 0) {  // a new address: in an empty or the least used slot
      slot = least;
      g_window[slot] = Peer{};
      memcpy(&g_window[slot].a, a, len);
      g_window[slot].len = len;
    }
    ++g_window[slot].count;
    if (same(g_server.a, g_server.len, a, len)) g_server_seen = now;
    if (now - g_window_at > std::chrono::seconds(3)) {  // the busiest of these seconds is the game server
      int best = 0;
      for (int i = 1; i < kPeers; ++i)
        if (g_window[i].count > g_window[best].count) best = i;
      if (g_window[best].count >= 10) {  // (a game sends many times a second; a lookup or two does not count)
        if (!same(g_server.a, g_server.len, reinterpret_cast<const sockaddr*>(&g_window[best].a), g_window[best].len))
          g_server = g_window[best];
        g_server_seen = now;
      }
      for (Peer& p : g_window) p = Peer{};
      g_window_at = now;
    }
  }
  if (!g_started.exchange(true)) std::thread(pinger).detach();
}

void netping_want(bool on) { g_want = on; }

float netping_ms() { return g_ms; }

}  // namespace tonic
