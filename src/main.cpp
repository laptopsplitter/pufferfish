// tonic: loads Roblox's Android x86_64 library and runs it in a desktop window.
// Development flags: --init, --jni-onload, --call SYM, --native CALL,
// --script FILE, --values FILE, --window, --assets DIR.

#include <pthread.h>
#include <signal.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#include "elfimg.hpp"
#include "fakejni.hpp"
#include "assets.hpp"
#include "input.hpp"
#include "webview.hpp"
#include "mainloop.hpp"
#include "game.hpp"
#include "audio.hpp"
#include "cookies.hpp"
#include "looper.hpp"
#include "menu.hpp"
#include "apk.hpp"
#include "apksig.hpp"

#ifndef TONIC_VERSION
#define TONIC_VERSION "dev"
#endif
#include "config.hpp"
#include "usersettings.hpp"
#include "clientsettings.hpp"
#include "mods.hpp"
#include "profile.hpp"
#include "links.hpp"
#include "device.hpp"
#include "haptics.hpp"
#include "vkshim.hpp"
#include "weblogin.hpp"
#include "script.hpp"
#include "window.hpp"
#include "shims.hpp"
#include "platform.hpp"
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#include "bsdabi.hpp"
#include "bsdsys.hpp"
#endif

namespace {

// Bounds of the current thread's stack, for crash reports.
thread_local uint64_t t_stack_lo = 0, t_stack_hi = 0;

void note_stack_bounds() {
  uint64_t lo = 0, hi = 0;
  if (tonic::thread_stack(lo, hi)) t_stack_lo = lo, t_stack_hi = hi;
}

// sigaltstack is per thread: every thread that may overflow its stack needs one,
// or the fault handler has nowhere to run and the process dies silently.
void install_altstack() {
  static thread_local unsigned char altstack[1 << 16];
  stack_t ss{};
  ss.ss_sp = altstack;
  ss.ss_size = sizeof altstack;
  sigaltstack(&ss, nullptr);
}

void out(const char* s) {
  if (write(2, s, strlen(s)) < 0) {}
}

// Read memory without faulting (platform.hpp).
bool safe_read(uint64_t addr, void* dst, size_t n) { return tonic::safe_read(addr, dst, n); }

void on_fault(int sig, siginfo_t* si, void* ctx) {
  uint64_t rip = 0, rsp = 0;
  tonic::signal_regs(ctx, rip, rsp);
  char buf[600], where[300];
  tonic::describe_addr(rip, where, sizeof where);
  int n;
  if (sig == SIGTRAP)
    n = snprintf(buf, sizeof buf, "\n[fault] signal %d (%s: a trap instruction, e.g. __builtin_trap after a fatal error), rip %s", sig,
                 strsignal(sig), where);
  else if (sig == SIGABRT || si->si_code <= 0)
    n = snprintf(buf, sizeof buf, "\n[fault] signal %d (%s), sent by pid %d, rip %s", sig, strsignal(sig),
                 (int)si->si_pid, where);
  else
    n = snprintf(buf, sizeof buf, "\n[fault] signal %d (%s), data address %p, rip %s", sig, strsignal(sig),
                 si->si_addr, where);
  const long idx = tonic::g_current_init;
  if (idx >= 0)
    n += snprintf(buf + n, sizeof buf - n, ", during constructor #%ld", idx);
  else if (idx == -2)
    n += snprintf(buf + n, sizeof buf - n, ", during DT_INIT");
  snprintf(buf + n, sizeof buf - n, "\n");
  out(buf);

  const uint64_t lo = t_stack_lo, hi = t_stack_hi;
  if (hi) {
    snprintf(buf, sizeof buf, "  rsp %#lx; this thread's stack is %#lx-%#lx (%lu KB), %ld KB in use\n", rsp, lo, hi,
             (hi - lo) >> 10, static_cast<long>(hi - rsp) >> 10);
    out(buf);
    const uint64_t fa = reinterpret_cast<uint64_t>(si->si_addr);
    if (sig == SIGSEGV && fa < lo + 0x10000 && fa + (1 << 20) > lo)
      out("  ==> STACK OVERFLOW: the fault is at the bottom of the stack (deep/endless recursion or a huge frame)\n");
  }

  // Walk the whole stack (safely) collecting values that point into image code.
  // Most are return addresses. We print the most recent ones, the oldest ones
  // (where the call chain started) and the most frequent ones (a recursion
  // cycle shows up as a few addresses repeated thousands of times).
  constexpr int kMax = 64;
  static uint64_t addr_v[kMax];
  static long addr_n[kMax];
  static uint64_t recent[12], oldest[12];
  int distinct = 0, nrecent = 0;
  long total = 0, nold = 0;
  const uint64_t end = hi ? hi : rsp + (256 << 10);
  static uint64_t page[512];
  for (uint64_t p = rsp & ~4095ULL; p < end; p += 4096) {
    if (!safe_read(p, page, sizeof page)) continue;
    for (int i = 0; i < 512; ++i) {
      const uint64_t slot = p + i * 8;
      if (slot < rsp) continue;
      const uint64_t v = page[i];
      if (!tonic::in_image_code(v)) continue;
      ++total;
      if (nrecent < 12) recent[nrecent++] = v;
      oldest[nold++ % 12] = v;
      int k = 0;
      while (k < distinct && addr_v[k] != v) ++k;
      if (k < distinct) ++addr_n[k];
      else if (distinct < kMax) addr_v[distinct] = v, addr_n[distinct++] = 1;
    }
  }
  snprintf(buf, sizeof buf, "  image code addresses on the stack: %ld total, %d distinct\n", total, distinct);
  out(buf);
  if (!total) {
    out("    (none found)\n");
    _exit(128 + sig);
  }
  out("  most recent first:\n");
  for (int i = 0; i < nrecent; ++i) {
    tonic::describe_addr(recent[i], where, sizeof where);
    snprintf(buf, sizeof buf, "    %s\n", where);
    out(buf);
  }
  if (total > 12) {
    out("  oldest (where this call chain started), oldest last:\n");
    const long cnt = nold < 12 ? nold : 12;
    for (long i = 0; i < cnt; ++i) {
      tonic::describe_addr(oldest[(nold - cnt + i) % 12], where, sizeof where);
      snprintf(buf, sizeof buf, "    %s\n", where);
      out(buf);
    }
    out("  most frequent:\n");
    for (int shown = 0; shown < 8; ++shown) {
      int best = -1;
      for (int k = 0; k < distinct; ++k)
        if (addr_n[k] > 0 && (best < 0 || addr_n[k] > addr_n[best])) best = k;
      if (best < 0 || addr_n[best] < 2) break;
      tonic::describe_addr(addr_v[best], where, sizeof where);
      snprintf(buf, sizeof buf, "    %6ld x %s\n", addr_n[best], where);
      out(buf);
      addr_n[best] = -addr_n[best];
    }
  }
  _exit(128 + sig);
}

void install_fault_handler() {
  install_altstack();
  note_stack_bounds();
  struct sigaction sa{};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(s, &sa, nullptr);
}

void print_wrapped(const std::vector<std::string>& names) {
  std::string line = " ";
  for (const auto& n : names) {
    if (line.size() + n.size() + 2 > 100) {
      puts(line.c_str());
      line = " ";
    }
    line += " " + n + ",";
  }
  if (line.size() > 1) puts(line.c_str());
}

// Host libraries whose symbols are fine to use from a bionic-built library.
bool plain_libc(const std::string& lib) {
  // tonic itself appears for stdin/stdout/stderr (copy relocations of
  // glibc's own variables) and the vDSO for time/gettimeofday: both are fine.
  for (const char* ok : {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
                         "libEGL.so.1", "libGLESv2.so.2", "libGLdispatch.so.0",
                         "ld-linux-x86-64.so.2", "linux-vdso.so.1", "tonic",
#if defined(__FreeBSD__) || defined(__OpenBSD__)
                         // (on FreeBSD, everything from libc goes through bsdabi.cpp first)
                         "libc.so.7", "libm.so.5", "libthr.so.3", "libepoll-shim.so.0", "ld-elf.so.1",
#endif
                        })
    if (lib == ok) return true;
  return false;
}

// Names we know behave differently on glibc than on bionic when called with
// bionic-built data. Not complete; it only flags what we already know about.
bool abi_suspect(const std::string& n) {
  static const char* exact[] = {"setlocale", "getauxval",
                                "dlopen", "dlsym", "dlclose", "dladdr", "getpwuid", "getpwnam", "sigsetjmp",
                                "siglongjmp", "sigprocmask", "sigaddset", "sigdelset", "sigismember", "mallinfo",
                                "syscall", "prctl"};
  for (const char* e : exact)
    if (n == e) return true;
  if (n == "__cxa_atexit" || n == "__cxa_finalize" || n == "__cxa_thread_atexit_impl") return false;
  return n.rfind("__cxa_", 0) == 0 || n.rfind("_Unwind_", 0) == 0 || n.rfind("_ZN", 0) == 0 ||
         n.rfind("_ZS", 0) == 0 || n.rfind("_ZT", 0) == 0 || n.rfind("__gxx", 0) == 0;
}

void print_host_report() {
  std::vector<tonic::HostSym> v = tonic::host_symbols();
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.lib != b.lib ? a.lib < b.lib : a.name < b.name; });
  printf("\nhost symbols by library:\n");
  std::string cur;
  std::vector<std::string> names;
  auto flush = [&] {
    if (cur.empty()) return;
    printf("  %s (%zu)%s\n", cur.c_str(), names.size(),
           plain_libc(cur) ? "" : "   <-- NOT libc: check these (C++ runtime/unwinder must not come from the host)");
    print_wrapped(names);
    names.clear();
  };
  for (const auto& h : v) {
    if (h.lib != cur) flush(), cur = h.lib;
    names.push_back(h.name);
  }
  flush();
  std::vector<std::string> sus;
  for (const auto& h : v)
    if (abi_suspect(h.name)) sus.push_back(h.name);
  printf("\nABI suspects (bionic and glibc disagree on struct layout or constants):\n");
  if (sus.empty()) puts("  (none)");
  else print_wrapped(sus);
}

}  // namespace

int main(int argc, char** argv, char** envp) {
  if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
    printf("tonic %s\n", TONIC_VERSION);
    return 0;
  }
  // ./tonic --apk-check FILE...: is each APK Roblox's own, unmodified? Prints
  // the signers' certificate fingerprints (to compare with apksig.cpp's list).
  if (argc >= 3 && !strcmp(argv[1], "--apk-check")) {
    int bad = 0;
    for (int i = 2; i < argc; ++i) {
      const tonic::ApkSignature sig = tonic::apk_signature(argv[i]);
      std::string why;
      const bool roblox = tonic::apk_signed_by_roblox(argv[i], why);
      printf("%s\n  signature: %s\n", argv[i], sig.valid ? "valid" : sig.error.c_str());
      for (const std::string& c : sig.certs) printf("  certificate SHA-256: %s\n", c.c_str());
      printf("  Roblox's own: %s\n", roblox ? "yes" : why.c_str());
      bad += !roblox;
    }
    return bad ? 1 : 0;
  }
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], "--window")) {
      tonic::memory_scope_enter(argc, argv);  // may start again in a scope
#if defined(__FreeBSD__) || defined(__OpenBSD__)
      // Roblox's own Linux system calls, through the tonicsud kernel module
      // (bsdsys.hpp). Before any thread exists.
      tonic::bsdsys_start();
#endif
      break;
    }
  std::string lib, call_sym;
  bool run_init = false, abort_on_stub = false, host_report = false, jni_onload = false;
  unsigned long stack_mb = 8;
  std::vector<tonic::NativeCall> calls;
  bool want_window = false, wait_for_close = true, touch = false;
  int win_w = 1280, win_h = 720;
  bool assets_given = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--init")) run_init = true;
    else if (!strcmp(argv[i], "--jni-onload")) jni_onload = run_init = true;
    else if (!strcmp(argv[i], "--abort-on-stub")) abort_on_stub = true;
    else if (!strcmp(argv[i], "--host-report")) host_report = true;
    else if (!strcmp(argv[i], "--stack-mb") && i + 1 < argc) stack_mb = strtoul(argv[++i], nullptr, 0);
    else if (!strcmp(argv[i], "--call") && i + 1 < argc) call_sym = argv[++i];
    else if (!strcmp(argv[i], "--window")) want_window = true;
    else if (!strcmp(argv[i], "--no-wait")) wait_for_close = false;
    else if (!strcmp(argv[i], "--touch")) touch = true;
    else if (!strcmp(argv[i], "--assets") && i + 1 < argc) tonic::set_asset_root(argv[++i]), assets_given = true;
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
      if (sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2 || win_w <= 0 || win_h <= 0) {
        fprintf(stderr, "--size wants WxH, e.g. 1280x720\n");
        return 2;
      }
    }
    else if (!strcmp(argv[i], "--native") && i + 1 < argc) {
      tonic::NativeCall c;
      std::string e;
      if (!tonic::parse_native_call(argv[++i], "--native", c, e)) {
        fprintf(stderr, "--native: %s\n", e.c_str());
        return 2;
      }
      calls.push_back(std::move(c));
      jni_onload = run_init = true;
    } else if (!strcmp(argv[i], "--values") && i + 1 < argc) {
      std::string e;
      if (!tonic::load_java_values(argv[++i], e)) {
        fprintf(stderr, "--values: %s\n", e.c_str());
        return 2;
      }
    } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
      std::string e;
      if (!tonic::load_script(argv[++i], calls, e)) {
        fprintf(stderr, "--script: %s\n", e.c_str());
        return 2;
      }
      jni_onload = run_init = true;
    }
    else if (argv[i][0] != '-') lib = argv[i];
    else {
      fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (lib.empty()) {
    fprintf(stderr, "usage: %s LIB.so [--init] [--jni-onload] [--call SYMBOL] [--abort-on-stub] [--host-report] [--stack-mb N]\n"
                    "       [--native 'CALL'] [--script FILE] [--values FILE] [--window [--size WxH] [--no-wait] [--touch]]\n"
                    "       [--assets DIR]\n", argv[0]);
    return 2;
  }

  // "auto" (run.sh's default): the Roblox installed from your APK
  // (apk.hpp). With none yet, the launcher asks for one.
  bool setup_only = false;
  {
    char cwd[4096];
    const std::string here = getcwd(cwd, sizeof cwd) ? cwd : ".";
    tonic::set_script_var("DATA", here + "/data");
    // This instance's own folders and login (profile.hpp); one at a time each.
    if (want_window && !tonic::profile_setup(here)) {
      // A Roblox link (run.sh --link) while tonic already runs: that tonic joins.
      tonic::JoinLink link;
      if (tonic::profile_id().empty() && tonic::link_take_startup(link)) {
        if (tonic::link_send(link)) {
          fprintf(stderr, "handed the link to the tonic that is running\n");
          return 0;
        }
      }
      return 1;
    }
    if (lib == "auto") {
      tonic::RobloxInstall ri;
      // Before any of Roblox's code is loaded: still Roblox's own, unchanged
      // (apk.hpp)? If not, it is removed and a fresh copy downloaded.
      if (tonic::apk_current(ri)) {
        std::string why;
        bool old = false;
        if (!tonic::apk_check_install(ri, why, &old)) {
          fprintf(stderr, "[apk] Roblox %s: %s; downloading it again\n", ri.version.c_str(), why.c_str());
          tonic::apk_forget();
          tonic::menu_set_setup_reason(old ? "Tonic has been updated, fetching new APK now"
                                           : "Roblox's files had changed, fetching a fresh copy now");
          ri = {};
        }
      }
      if (!ri.lib.empty()) {
        lib = ri.lib;
        if (!assets_given) tonic::set_asset_root(ri.assets);
        tonic::set_script_var("VERSION", ri.version);
        printf("Roblox %s: %s\n", ri.version.c_str(), lib.c_str());
        if (want_window) tonic::clientsettings_start();  // (fetched while the launcher shows)
        if (want_window) tonic::links_handler_refresh();
      } else if (want_window) {
        setup_only = true;
      } else {
        fprintf(stderr, "no Roblox installed: open tonic from your app menu (or run: tonic) and it downloads it\n");
        return 2;
      }
    } else if (tonic::RobloxInstall ri; tonic::apk_current(ri)) {
      tonic::set_script_var("VERSION", ri.version);  // a library given by path: the installed version's number
    }
    const std::string assets = tonic::asset_root();
    const std::string assets_abs = assets[0] == '/' ? assets : here + "/" + assets;
    tonic::set_script_var("ASSETS", assets_abs);
    // Where the APK's content is, wherever this folder is.
    tonic::set_java_value_string("com/roblox/engine/jni/model/PlatformParams.assetFolderPath:Ljava/lang/String;",
                                 assets_abs + "/content");
  }
  if (setup_only) {
    // No Roblox yet: only the launcher, which installs the APK and starts
    // tonic again with it.
    tonic::menu_set_setup();
    std::string e;
    if (!tonic::window_open(win_w, win_h, e)) {
      fprintf(stderr, "window: %s\n", e.c_str());
      return 1;
    }
    while (tonic::window_pump(16)) {}
    _exit(0);
  }

  install_fault_handler();
  tonic::init_shims(abort_on_stub);
  tonic::register_window_shims();
  tonic::register_vulkan_shims();
  tonic::register_asset_shims();
  tonic::register_looper_shims();
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  tonic::init_bsd_abi();  // last: it wraps some of the shims registered above
#endif
  tonic::input_init(touch);
  tonic::webview_init();
  tonic::game_init();
  tonic::haptics_init();  // game vibration -> controller rumble
  tonic::audio_init();
  tonic::cookies_init();
  tonic::weblogin_apply_identity();  // the website sign-in's user, if any
  tonic::device_apply_settings();    // UI size, layout and theme (Settings > Look)
  tonic::memory_limit_apply();
  tonic::cookies_storage_init();
  tonic::register_script_builtin(
      "cookies", [](const tonic::ElfImage& im, const std::vector<std::string>&) { tonic::cookies_enable(im); });
  tonic::register_script_builtin(
      "webview", [](const tonic::ElfImage& im, const std::vector<std::string>&) { tonic::webview_enable(im); });
  if (want_window) {
    std::string e;
    if (!tonic::window_open(win_w, win_h, e)) {
      fprintf(stderr, "window: %s\n", e.c_str());
      return 1;
    }
  }

  tonic::ElfImage img;
  std::string err;
  if (!img.load(lib, err)) {
    fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  tonic::register_image(lib, img.base(), img.mapped_size(), img.phdrs().data(), img.phdrs().size());
  printf("loaded %s\n  base %#lx, %.1f MB mapped, %zu constructors\n", lib.c_str(), img.base(),
         img.mapped_size() / 1e6, img.init_count());
  {
    std::string nd;
    for (const auto& d : img.needed()) nd += (nd.empty() ? "" : ", ") + d;
    printf("  needs: %s\n", nd.empty() ? "(nothing)" : nd.c_str());
  }

  if (!img.relocate(tonic::resolve_symbol, err)) {
    fprintf(stderr, "relocation failed: %s\n", err.c_str());
    return 1;
  }
  if (!img.protect_relro(err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  const auto& rs = img.stats();
  const auto& ss = tonic::shim_stats();
  printf("relocations: RELATIVE=%zu GLOB_DAT=%zu JUMP_SLOT=%zu 64=%zu\n", rs.relative, rs.glob_dat, rs.jump_slot,
         rs.abs64);
  printf("symbols: from glibc=%zu, our shims=%zu, weak->null=%zu, NOT IMPLEMENTED (logging stubs)=%zu\n", ss.host,
         ss.shim, ss.weak_null, ss.stubbed);

  auto names = tonic::stubbed_symbols();
  std::sort(names.begin(), names.end());
  if (!names.empty()) {
    printf("\nnot implemented yet:\n");
    print_wrapped(names);
  }
  if (host_report) print_host_report();
  if (void* jni = img.find_symbol("JNI_OnLoad")) printf("\nJNI_OnLoad at image+%#lx\n", (uint64_t)jni - img.base());

  fflush(stdout);
  if (run_init) {
    printf("\nrunning constructors...\n");
    fflush(stdout);
    img.run_init(argc, argv, envp, true);
    printf("constructors finished\n");
  }
  if (jni_onload) {
    auto fn = reinterpret_cast<int (*)(void*, void*)>(img.find_symbol("JNI_OnLoad"));
    if (!fn) {
      fprintf(stderr, "JNI_OnLoad not found\n");
      return 1;
    }
    struct Job {
      int (*fn)(void*, void*);
      void* vm;
      int result;
      const tonic::ElfImage* img;
      const std::vector<tonic::NativeCall>* calls;
      int failed;
      bool stay;  // window mode, waiting for close: become the main loop
    } job{fn, tonic::fake_java_vm(), 0, &img, &calls, 0, want_window && wait_for_close};
    printf("\ncalling JNI_OnLoad(fake JavaVM, NULL) on a thread with a %lu MB stack\n", stack_mb);
    fflush(stdout);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, stack_mb << 20);
    pthread_t th;
    const int e = pthread_create(
        &th, &at,
        [](void* p) -> void* {
          install_altstack();
          note_stack_bounds();
          tonic::jni_attach_current_thread();
          tonic::looper_prepare_main_thread();  // Android's main thread always has one
          // Roblox starts when Play is pressed on the menu (at once without one);
          // the frame rate cap chosen there applies now.
          tonic::menu_wait_start();
          tonic::device_apply_settings();  // as set in the launcher just now (Look page)
          tonic::clientsettings_wait();  // Roblox's settings, fetched since tonic started
          tonic::usersettings_apply();
          if (tonic::window_is_open()) tonic::vulkan_decide();  // Vulkan, or OpenGL (before the flags: they say which)
          tonic::mods_apply_flags();  // after usersettings: both edit the preload overrides
          tonic::mods_apply_files();
          auto* j = static_cast<Job*>(p);
          j->result = j->fn(j->vm, nullptr);
          // Java calls natives on the thread that loaded the library (the
          // app's main thread), so the calls run here, after JNI_OnLoad.
          tonic::install_rules(*j->img, *j->calls);
          if (!j->calls->empty()) {
            if (j->result < 0) {
              printf("\nJNI_OnLoad failed; not calling natives\n");
              j->failed = static_cast<int>(j->calls->size());
            } else {
              printf("\nJNI_OnLoad returned %#x; calling %zu native(s)\n", j->result, j->calls->size());
              j->failed = tonic::run_native_calls(*j->img, *j->calls);
            }
          }
          if (tonic::window_is_open() && j->result >= 0) {
            tonic::input_enable(*j->img);
            tonic::webview_enable(*j->img);
            tonic::game_enable(*j->img);
            tonic::display_rates_report(*j->img);
            tonic::power_state_report(*j->img);
          }
          if (tonic::window_is_open() && j->result >= 0 && j->stay) {
            // Like Android's UI thread, this thread stays: it becomes the
            // app's main loop (mainloop.hpp). Report now, as it never ends.
            fflush(stderr);
            if (!j->calls->empty()) printf("\nnative calls: %zu run, %d failed\n", j->calls->size(), j->failed);
            tonic::print_jni_summary();
            tonic::print_asset_summary();
            printf("\ncalls finished; the window stays open until you close it\n");
            fflush(stdout);
            tonic::main_loop(*j->img);
          }
          return nullptr;
        },
        &job);
    pthread_attr_destroy(&at);
    if (e != 0) {
      fprintf(stderr, "pthread_create failed: %s\n", strerror(e));
      return 1;
    }
    if (tonic::window_is_open()) {
      // The main thread owns the window: it handles window events while the
      // calls run, and keeps doing so afterwards (the library's own threads
      // render) until the window is closed.
      bool joined = false;
      for (;;) {
        const bool open = tonic::window_pump(16);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
        joined = pthread_peekjoin_np(th, nullptr) == 0 && pthread_join(th, nullptr) == 0;
#elif defined(__OpenBSD__)
        joined = false;
#else
        joined = pthread_tryjoin_np(th, nullptr) == 0;
#endif
        if (joined) break;
        // Closing with Roblox running: the closing screen, with a moment to
        // cancel. Closing again (or no Roblox yet) closes at once.
        if (!open && !tonic::menu_closing_begin()) break;
        if (tonic::menu_closing_state() == 3) break;
      }
      if (!joined) {
        printf("\nwindow closed\n");
        fflush(stdout);
        if (tonic::menu_roblox_started()) {
          // Roblox leaves the game and saves its settings (up to a few
          // seconds); the closing screen keeps drawing meanwhile.
          tonic::menu_closing_begin();  // (when not already showing)
          tonic::menu_closing_go();
          std::atomic<bool> done{false};
          std::thread shut([&done] {
            tonic::game_shutdown();
            done = true;
          });
          while (!done) tonic::window_pump(16);
          shut.join();
        }
        tonic::vulkan_exit();
        fflush(stderr);
        _exit(0);
      }
    } else {
      pthread_join(th, nullptr);
    }
    const int v = job.result;
    fflush(stderr);
    printf("JNI_OnLoad returned %#x%s\n", v, v == 0x10006 ? " (JNI 1.6)" : v < 0 ? " (FAILED)" : "");
    tonic::print_jni_summary();
    tonic::print_asset_summary();
    if (!calls.empty()) printf("\nnative calls: %zu run, %d failed\n", calls.size(), job.failed);
    if (tonic::window_is_open()) {
      if (wait_for_close) {
        printf("\ncalls finished; window stays open until you close it\n");
        fflush(stdout);
        while (tonic::window_pump(100)) {}
      }
      fflush(stdout);
      fflush(stderr);
      _exit(job.failed ? 1 : 0);  // the library's threads are still running
    }
    if (job.failed) return 1;
  }
  if (!call_sym.empty()) {
    auto fn = reinterpret_cast<int (*)(void*, void*)>(img.find_symbol(call_sym.c_str()));
    if (!fn) {
      fprintf(stderr, "symbol %s not found\n", call_sym.c_str());
      return 1;
    }
    printf("\ncalling %s(NULL, NULL)\n", call_sym.c_str());
    fflush(stdout);
    printf("returned %#x\n", fn(nullptr, nullptr));
  }
  return 0;
}
