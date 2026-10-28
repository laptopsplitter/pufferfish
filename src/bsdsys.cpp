// bsdsys.cpp - see bsdsys.hpp. FreeBSD only.
#ifdef __FreeBSD__

#include "bsdsys.hpp"
#include "bsdabi.hpp"
#include "platform.hpp"

#include <sys/param.h>
#include <sys/module.h>
#include <sys/syscall.h>
#include <machine/ucontext.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <limits.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace tonic {
namespace {

constexpr int kVersion = 1;       // bsdkmod/tonicsud.c TSUD_VERSION
constexpr int kTrapCode = 0x7473;  // TSUD_CODE
constexpr int kSig = SIGEMT;

int g_nr = -1;  // the module's control call
bool g_on = false;
std::mutex g_mu;
std::vector<std::pair<uint64_t, uint64_t>> g_early;  // ranges named before bsdsys_start
std::vector<std::pair<uint64_t, uint64_t>> g_named;  // what the module has (removal only asks when it overlaps)

std::atomic<bool> g_logged[512];
void note(long nr) {
  if (nr >= 0 && nr < 512 && !g_logged[nr].exchange(true))
    fprintf(stderr, "[bsdsys] Roblox calls Linux system call %ld directly: translated\n", nr);
}

// A Linux `syscall` from Roblox's code. FreeBSD's syscall entry keeps the 4th
// argument (r10) in the frame's rcx slot and does not save r10 itself, so the
// context has it in mc_rcx and junk in mc_r10. Linux's `syscall` leaves every
// register but rax, rcx and r11 as it was: r10 is put back, the result goes in
// rax, rcx gets the return address and r11 the flags.
void on_trap(int, siginfo_t* si, void* ucv) {
  auto* uc = static_cast<ucontext_t*>(ucv);
  mcontext_t& m = uc->uc_mcontext;
  if (si->si_code != kTrapCode) return;  // (someone's kill(SIGEMT): nothing to do)
  const long nr = static_cast<long>(m.mc_rax);
  note(nr);
  const long a[6] = {static_cast<long>(m.mc_rdi), static_cast<long>(m.mc_rsi), static_cast<long>(m.mc_rdx),
                     static_cast<long>(m.mc_rcx), static_cast<long>(m.mc_r8),  static_cast<long>(m.mc_r9)};
  m.mc_r10 = m.mc_rcx;
  const long r = bsd_raw_syscall(nr, a);
  m.mc_rax = r;
  m.mc_rcx = m.mc_rip;
  m.mc_r11 = m.mc_rflags;
}

bool add(uint64_t lo, uint64_t hi) {
  if (syscall(g_nr, 2, lo, hi) == 0) {
    g_named.emplace_back(lo, hi);
    return true;
  }
  fprintf(stderr, "[bsdsys] could not name code range %#llx-%#llx: %s\n", static_cast<unsigned long long>(lo),
          static_cast<unsigned long long>(hi), strerror(errno));
  return false;
}

BsdModule g_state = BsdModule::missing;
bool g_handler = false;

// Looks for the module and turns it on. Logs only when the outcome changes.
BsdModule try_on() {
  int nr = -1;
  BsdModule st = BsdModule::missing;
  const int id = modfind("sys/tonicsud");
  if (id >= 0) {
    module_stat ms{};
    ms.version = sizeof ms;
    if (modstat(id, &ms) == 0) {
      nr = ms.data.intval;
      st = syscall(nr, 0, 0, 0) == kVersion ? BsdModule::on : BsdModule::old;
    }
  }
  if (st == BsdModule::on && !g_handler) {
    struct sigaction sa{};
    sa.sa_sigaction = on_trap;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(kSig, &sa, nullptr) == 0) g_handler = true;
    else st = BsdModule::failed;
  }
  if (st == BsdModule::on && syscall(nr, 1, kSig, 0) != 0) st = BsdModule::failed;
  if (st != g_state) {
    switch (st) {
      case BsdModule::on:
        fprintf(stderr, "[bsdsys] tonicsud kernel module found: Roblox's direct Linux calls are translated\n");
        break;
      case BsdModule::missing:
        fprintf(stderr, "[bsdsys] the tonicsud kernel module is not loaded, so Roblox's direct Linux calls are not "
                        "translated (expect Error 304 after about a minute). Build it once with gmake kmod, then "
                        "as root: kldload %s\n", bsdsys_module_path().c_str());
        break;
      case BsdModule::old:
        fprintf(stderr, "[bsdsys] the loaded tonicsud is not the version this tonic needs (%d): rebuild it "
                        "(gmake kmod) and reload it as root (kldunload tonicsud; kldload %s)\n", kVersion,
                bsdsys_module_path().c_str());
        break;
      case BsdModule::failed:
        fprintf(stderr, "[bsdsys] tonicsud is loaded but could not be turned on: %s\n", strerror(errno));
        break;
      case BsdModule::off:
        break;
    }
  }
  g_state = st;
  if (st == BsdModule::on) {
    g_nr = nr;
    g_on = true;
    for (const auto& r : g_early) add(r.first, r.second);
    g_early.clear();
  }
  return st;
}

std::string dir_of(const std::string& p) {
  const size_t s = p.rfind('/');
  return s == std::string::npos ? "." : s == 0 ? "/" : p.substr(0, s);
}

std::string shell_quote(const std::string& s) {
  if (s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-+") == std::string::npos)
    return s;
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

}  // namespace

void bsdsys_start() {
  const char* e = getenv("TONIC_SYSCALLS");
  if (e && !strcmp(e, "off")) {
    fprintf(stderr, "[bsdsys] TONIC_SYSCALLS=off: Roblox's direct Linux calls are not translated "
                    "(expect Error 304 after about a minute)\n");
    std::lock_guard<std::mutex> l(g_mu);
    g_state = BsdModule::off;
    return;
  }
  std::lock_guard<std::mutex> l(g_mu);
  g_state = BsdModule::off;  // (so the first outcome is logged)
  try_on();
}

BsdModule bsdsys_state() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_state;
}

BsdModule bsdsys_retry() {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_state == BsdModule::on || g_state == BsdModule::off) return g_state;
  return try_on();
}

constexpr const char* kInstalled = "/boot/modules/tonicsud.ko";  // (install.sh puts it there)

std::string bsdsys_module_path() {
  if (access(kInstalled, R_OK) == 0) return kInstalled;
  std::string p = share_file("bsdkmod/tonicsud.ko");
  if (p[0] != '/') {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) p = std::string(cwd) + "/" + p;
  }
  return p;
}

std::string bsdsys_load_command() {
  const std::string ko = bsdsys_module_path();
  if (ko == kInstalled) return std::string(bsdsys_state() == BsdModule::old ? "kldunload tonicsud; " : "") + "kldload tonicsud";
  const std::string q = shell_quote(ko);
  const bool built = access(ko.c_str(), R_OK) == 0;
  std::string load = built ? "kldload " + q
                           // Not built yet: built where root's files do not end up in yours.
                           : "mkdir -p /tmp/tonicsud && env MAKEOBJDIR=/tmp/tonicsud make -C " +
                                 shell_quote(dir_of(ko)) + " && kldload /tmp/tonicsud/tonicsud.ko";
  if (bsdsys_state() == BsdModule::old) load = "kldunload tonicsud; " + load;
  return load;
}

bool bsdsys_sources_missing() {
  return access(bsdsys_module_path().c_str(), R_OK) != 0 && access("/usr/src/sys/kern/subr_syscall.c", R_OK) != 0;
}

void bsdsys_add_range(uint64_t lo, uint64_t hi) {
  if (lo >= hi) return;
  std::lock_guard<std::mutex> l(g_mu);
  if (g_on) add(lo, hi);
  else if (g_early.size() < 256) g_early.emplace_back(lo, hi);
}

void bsdsys_remove_range(uint64_t lo, uint64_t hi) {
  if (lo >= hi) return;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_on) {  // (not passed on yet: forget what it covers whole)
    for (auto it = g_early.begin(); it != g_early.end();)
      it = lo <= it->first && hi >= it->second ? g_early.erase(it) : it + 1;
    return;
  }
  bool overlaps = false;
  for (auto it = g_named.begin(); it != g_named.end();) {
    if (hi <= it->first || lo >= it->second) {
      ++it;
      continue;
    }
    overlaps = true;
    if (lo <= it->first && hi >= it->second) it = g_named.erase(it);
    else ++it;  // (cut at an edge or in the middle: keep it; asking again later is harmless)
  }
  if (overlaps) syscall(g_nr, 4, lo, hi);
}

}  // namespace tonic

#endif  // __FreeBSD__
