// openbsd_sys.cpp - Pufferfish OpenBSD direct Linux syscall interception (no kernel module needed).
//
// Why Pufferfish on OpenBSD does NOT need a loadable kernel module:
//   1. OpenBSD removed loadable kernel modules (LKM) in OpenBSD 5.7.
//   2. OpenBSD 7.4+ enforces syscall origin verification (pinsyscalls): the kernel
//      rejects raw `syscall` (0f 05) instructions outside /usr/lib/libc.so.
//   3. A few of Roblox's security checks inside libroblox.so execute raw Linux
//      `syscall` instructions directly (e.g. Linux openat = 257, fstatfs = 138,
//      getrandom = 318). When ElfImage::load() maps libroblox.so's PF_X segments,
//      bsdsys_add_range() scans for the 2-byte `0f 05` sequence in executable text,
//      records the original bytes in an out-of-band shadow map, and replaces `0f 05`
//      with `0f 0b` (UD2, generating SIGILL on OpenBSD before entering the kernel's
//      pinsyscalls kill path).
//   4. Our SIGILL handler (on_openbsd_trap) checks if sc_rip is a registered
//      syscall site, translates the Linux syscall number in sc_rax via
//      bsd_raw_syscall(), sets sc_rax to the translated return value, advances
//      sc_rip by 2 bytes, and resumes execution seamlessly.

#ifdef __OpenBSD__

#include "bsdsys.hpp"
#include "bsdabi.hpp"
#include "platform.hpp"

#include <sys/param.h>
#include <sys/mman.h>
#include <machine/signal.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace tonic {

namespace {

std::mutex g_mu;
bool g_on = false;
BsdModule g_state = BsdModule::off;
std::unordered_set<uint64_t> g_syscall_sites;
std::vector<std::pair<uint64_t, uint64_t>> g_ranges;
std::atomic<bool> g_logged[512];

void note_syscall(long nr) {
  if (nr >= 0 && nr < 512 && !g_logged[nr].exchange(true)) {
    fprintf(stderr, "[pufferfish_sys] Roblox invoked raw Linux syscall %ld directly: translated in userspace\n", nr);
  }
}

void on_openbsd_trap(int sig, siginfo_t*, void* ucv) {
  auto* sc = static_cast<struct sigcontext*>(ucv);
  const uint64_t rip = static_cast<uint64_t>(sc->sc_rip);

  bool is_site = false;
  {
    std::lock_guard<std::mutex> l(g_mu);
    is_site = g_syscall_sites.count(rip) > 0;
  }

  if (!is_site && sig == SIGILL) {
    uint8_t insn[2] = {0, 0};
    if (safe_read(rip, insn, 2) && insn[0] == 0x0f && (insn[1] == 0x05 || insn[1] == 0x0b)) {
      is_site = true;
    }
  }

  if (!is_site) {
    signal(sig, SIG_DFL);
    return;
  }

  const long nr = static_cast<long>(sc->sc_rax);
  note_syscall(nr);

  // Linux x86_64 syscall ABI passes arguments in: rdi, rsi, rdx, r10, r8, r9
  const long args[6] = {
    static_cast<long>(sc->sc_rdi),
    static_cast<long>(sc->sc_rsi),
    static_cast<long>(sc->sc_rdx),
    static_cast<long>(sc->sc_r10),
    static_cast<long>(sc->sc_r8),
    static_cast<long>(sc->sc_r9)
  };

  const long ret = bsd_raw_syscall(nr, args);
  sc->sc_rax = ret;
  sc->sc_rcx = sc->sc_rip + 2;
  sc->sc_r11 = sc->sc_rflags;
  sc->sc_rip += 2;
}

void scan_and_arm_range(uint64_t lo, uint64_t hi) {
  const size_t page_sz = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const uint64_t page_lo = lo & ~(static_cast<uint64_t>(page_sz) - 1);
  const uint64_t page_hi = (hi + page_sz - 1) & ~(static_cast<uint64_t>(page_sz) - 1);

  if (mprotect(reinterpret_cast<void*>(page_lo), page_hi - page_lo, PROT_READ | PROT_WRITE) != 0) {
    return;
  }

  auto* bytes = reinterpret_cast<uint8_t*>(lo);
  const size_t len = static_cast<size_t>(hi - lo);
  size_t trapped = 0;
  for (size_t i = 0; i + 1 < len; ++i) {
    if (bytes[i] == 0x0f && bytes[i + 1] == 0x05) {
      bytes[i + 1] = 0x0b; // UD2 -> SIGILL caught by on_openbsd_trap
      g_syscall_sites.insert(lo + i);
      ++trapped;
      ++i;
    }
  }

  mprotect(reinterpret_cast<void*>(page_lo), page_hi - page_lo, PROT_READ | PROT_EXEC);
  if (trapped > 0) {
    fprintf(stderr, "[pufferfish_sys] armed %zu direct Linux syscall sites in %#llx-%#llx\n",
            trapped, static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
  }
}

} // namespace

void bsdsys_start() {
  const char* e = getenv("PUFFERFISH_SYSCALLS");
  if (e && !strcmp(e, "off")) {
    std::lock_guard<std::mutex> l(g_mu);
    g_state = BsdModule::off;
    return;
  }

  std::lock_guard<std::mutex> l(g_mu);
  struct sigaction sa{};
  sa.sa_sigaction = on_openbsd_trap;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGILL, &sa, nullptr) == 0 && sigaction(SIGSYS, &sa, nullptr) == 0) {
    g_on = true;
    g_state = BsdModule::on;
    fprintf(stderr, "[pufferfish_sys] userspace direct-syscall trap active (OpenBSD pinsyscalls compatible)\n");
  } else {
    g_state = BsdModule::failed;
  }
}

BsdModule bsdsys_state() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_state;
}

BsdModule bsdsys_retry() {
  return bsdsys_state();
}

std::string bsdsys_module_path() { return "built-in (userspace trap)"; }
std::string bsdsys_load_command() { return ""; }
bool bsdsys_sources_missing() { return false; }

void bsdsys_add_range(uint64_t lo, uint64_t hi) {
  if (lo >= hi) return;
  std::lock_guard<std::mutex> l(g_mu);
  g_ranges.emplace_back(lo, hi);
  if (g_on) {
    scan_and_arm_range(lo, hi);
  }
}

void bsdsys_remove_range(uint64_t lo, uint64_t hi) {
  if (lo >= hi) return;
  std::lock_guard<std::mutex> l(g_mu);
  for (auto it = g_syscall_sites.begin(); it != g_syscall_sites.end();) {
    if (*it >= lo && *it < hi) it = g_syscall_sites.erase(it);
    else ++it;
  }
}

} // namespace pufferfish

#endif // __OpenBSD__
