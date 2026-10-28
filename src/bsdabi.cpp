// bsdabi.cpp - FreeBSD only: Linux numbers and layouts in, FreeBSD's calls,
// answers converted back. No equivalent: fails as an older Linux would, logged
// once as [bsdabi]. errno is converted when the library reads it (sh_errno).
#ifdef __FreeBSD__

#include "bsdabi.hpp"
#include "bsdsys.hpp"
#include "netping.hpp"
#include "shims.hpp"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/auxv.h>
#include <sys/cpuset.h>
#include <sys/eventfd.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/procctl.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/umtx.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/utsname.h>
#include <machine/trap.h>
#include <arpa/inet.h>
#include <cpuid.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <langinfo.h>
#include <locale.h>
#include <malloc_np.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pthread_np.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC  // (FreeBSD before 14; 14 defines it so)
#endif

namespace tonic {
namespace {

// Says once per message that something needed a translation worth knowing
// about (or had none).
__attribute__((format(printf, 1, 2))) void note_once(const char* fmt, ...) {
  char m[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m, sizeof m, fmt, ap);
  va_end(ap);
  static std::mutex mu;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> l(mu);
  if (seen.insert(m).second) fprintf(stderr, "[bsdabi] %s\n", m);
}

// TONIC_STATS=1: how often the costly translations run, every 5 seconds.
bool g_stats = false;
std::atomic<uint64_t> g_n_clock{0}, g_n_dontneed{0}, g_n_vmmap{0}, g_n_yield{0}, g_n_nap{0}, g_n_futex{0};
// Network: polls (and how many ended by timing out, and the time spent in
// those), connections, bytes through the socket calls.
std::atomic<uint64_t> g_n_poll{0}, g_n_poll_timeout{0}, g_poll_timeout_ms{0}, g_n_connect{0}, g_net_in{0}, g_net_out{0};
inline void count(std::atomic<uint64_t>& c) {
  if (__builtin_expect(g_stats, 0)) c.fetch_add(1, std::memory_order_relaxed);
}
// Timed waits: how many timed out, how many of those came back more than 5 ms
// late, and the worst lateness (us), per kind of wait.
struct WaitStat {
  const char* name;
  std::atomic<uint64_t> timeouts{0}, late{0}, worst_us{0};
};
WaitStat g_w_futex{"futex"}, g_w_cond{"cond"}, g_w_sleep{"clock_nanosleep"}, g_w_mutex{"mutex"};
uint64_t now_ns(clockid_t c) {
  timespec t{};
  clock_gettime(c, &t);
  return static_cast<uint64_t>(t.tv_sec) * 1000000000ULL + static_cast<uint64_t>(t.tv_nsec);
}
uint64_t ts_ns(const timespec* t) { return static_cast<uint64_t>(t->tv_sec) * 1000000000ULL + static_cast<uint64_t>(t->tv_nsec); }
// A wait on clock C that should have ended at DEADLINE_NS timed out: how late?
void note_timeout(WaitStat& w, clockid_t c, uint64_t deadline_ns) {
  if (!__builtin_expect(g_stats, 0)) return;
  w.timeouts.fetch_add(1, std::memory_order_relaxed);
  const uint64_t now = now_ns(c);
  if (now <= deadline_ns) return;
  const uint64_t late_us = (now - deadline_ns) / 1000;
  if (late_us > 5000) w.late.fetch_add(1, std::memory_order_relaxed);
  uint64_t prev = w.worst_us.load(std::memory_order_relaxed);
  while (late_us > prev && !w.worst_us.compare_exchange_weak(prev, late_us, std::memory_order_relaxed)) {
  }
}

template <typename T>
void reg(const char* name, T* fn) {
  register_shim(name, reinterpret_cast<void*>(fn));
}

template <typename T>
T lookup_prev(const char* name, T host) {
  if (void* p = shim_lookup(name)) return reinterpret_cast<T>(p);
  return host;
}

// For functions that do not exist on FreeBSD at all.
long sh_enosys() {
  errno = ENOSYS;
  return -1;
}

// ===========================================================================
// errno
// ===========================================================================

struct NumPair {
  int lx, bsd;
};
// Linux number, FreeBSD name (so FreeBSD's own value is used).
constexpr NumPair kErrno[] = {
    {1, EPERM},         {2, ENOENT},           {3, ESRCH},           {4, EINTR},          {5, EIO},
    {6, ENXIO},         {7, E2BIG},            {8, ENOEXEC},         {9, EBADF},          {10, ECHILD},
    {11, EAGAIN},       {12, ENOMEM},          {13, EACCES},         {14, EFAULT},        {15, ENOTBLK},
    {16, EBUSY},        {17, EEXIST},          {18, EXDEV},          {19, ENODEV},        {20, ENOTDIR},
    {21, EISDIR},       {22, EINVAL},          {23, ENFILE},         {24, EMFILE},        {25, ENOTTY},
    {26, ETXTBSY},      {27, EFBIG},           {28, ENOSPC},         {29, ESPIPE},        {30, EROFS},
    {31, EMLINK},       {32, EPIPE},           {33, EDOM},           {34, ERANGE},        {35, EDEADLK},
    {36, ENAMETOOLONG}, {37, ENOLCK},          {38, ENOSYS},         {39, ENOTEMPTY},     {40, ELOOP},
    {42, ENOMSG},       {43, EIDRM},           {61, ENOATTR},        {66, EREMOTE},       {67, ENOLINK},
    {71, EPROTO},       {72, EMULTIHOP},       {74, EBADMSG},        {75, EOVERFLOW},     {84, EILSEQ},
    {87, EUSERS},       {88, ENOTSOCK},        {89, EDESTADDRREQ},   {90, EMSGSIZE},      {91, EPROTOTYPE},
    {92, ENOPROTOOPT},  {93, EPROTONOSUPPORT}, {94, ESOCKTNOSUPPORT}, {95, EOPNOTSUPP},   {96, EPFNOSUPPORT},
    {97, EAFNOSUPPORT}, {98, EADDRINUSE},      {99, EADDRNOTAVAIL},  {100, ENETDOWN},     {101, ENETUNREACH},
    {102, ENETRESET},   {103, ECONNABORTED},   {104, ECONNRESET},    {105, ENOBUFS},      {106, EISCONN},
    {107, ENOTCONN},    {108, ESHUTDOWN},      {109, ETOOMANYREFS},  {110, ETIMEDOUT},    {111, ECONNREFUSED},
    {112, EHOSTDOWN},   {113, EHOSTUNREACH},   {114, EALREADY},      {115, EINPROGRESS},  {116, ESTALE},
    {122, EDQUOT},      {125, ECANCELED},      {130, EOWNERDEAD},    {131, ENOTRECOVERABLE},
};
// FreeBSD-only errors, as the nearest Linux one.
constexpr NumPair kErrnoBsdOnly[] = {
    {22, EFTYPE},  {22, EDOOFUS},   {1, ENOTCAPABLE}, {1, ECAPMODE},     {11, EPROCLIM},
    {5, EINTEGRITY}, {5, EBADRPC},  {5, ERPCMISMATCH}, {5, EPROGUNAVAIL}, {5, EPROGMISMATCH},
    {5, EPROCUNAVAIL}, {13, EAUTH}, {13, ENEEDAUTH},
};
// Linux-only errors, as the nearest FreeBSD one (for strerror and friends).
constexpr NumPair kErrnoLxOnly[] = {
    {62, ETIMEDOUT}, {63, ENOBUFS}, {77, EBADF}, {85, EINTR},  {86, EPIPE},
    {123, ENXIO},    {64, ENETDOWN}, {70, EIO},  {121, EIO},   {60, EINVAL},
};

constexpr int kLxErrMax = 134;
int g_e2lx[ELAST + 1];
int g_lx2e[kLxErrMax];

void init_errno_maps() {
  for (int& e : g_e2lx) e = 5;  // EIO
  for (int& e : g_lx2e) e = EINVAL;
  g_e2lx[0] = 0;
  g_lx2e[0] = 0;
  for (const auto& p : kErrnoBsdOnly) g_e2lx[p.bsd] = p.lx;
  for (const auto& p : kErrnoLxOnly) g_lx2e[p.lx] = p.bsd;
  for (const auto& p : kErrno) g_e2lx[p.bsd] = p.lx, g_lx2e[p.lx] = p.bsd;
}

inline int lx_errno(int e) { return e > 0 && e <= ELAST ? g_e2lx[e] : e; }
inline int bsd_errno(int e) { return e > 0 && e < kLxErrMax ? g_lx2e[e] : (e == 0 ? 0 : EINVAL); }
// For functions that return an error number instead of setting errno.
inline int rc(int r) { return r > 0 ? lx_errno(r) : r; }

// The library's errno. bionic's errno is (*__errno()). FreeBSD functions set
// FreeBSD's errno; the next time the library looks, the new value is converted
// and FreeBSD's is cleared, so a later failure is seen as new. What the library
// writes through the pointer stays until a FreeBSD call fails again.
thread_local int t_errno = 0;
int* sh_errno() {
  const int h = errno;
  if (h) {
    t_errno = lx_errno(h);
    errno = 0;
  }
  return &t_errno;
}
int lib_errno_as_bsd() { return bsd_errno(*sh_errno()); }

template <typename F, F f>
struct Rc;
template <typename R, typename... A, R (*f)(A...)>
struct Rc<R (*)(A...), f> {
  static R call(A... a) { return static_cast<R>(rc(static_cast<int>(f(a...)))); }
};
#define REG_RC(name) reg(#name, &Rc<decltype(&::name), &::name>::call)

char* sh_strerror(int e) { return strerror(bsd_errno(e)); }
char* sh_strerror_l(int e, locale_t l) { return strerror_l(bsd_errno(e), l); }
int sh_strerror_r(int e, char* b, size_t n) { return rc(strerror_r(bsd_errno(e), b, n)); }
char* sh_gnu_strerror_r(int e, char* b, size_t n) {
  if (n) strerror_r(bsd_errno(e), b, n);
  return b;
}
void sh_perror(const char* s) {
  const int e = lib_errno_as_bsd();
  errno = e;
  perror(s);
  errno = 0;
}
int* sh_get_h_errno() { return __h_errno(); }

// ===========================================================================
// Bit and number maps
// ===========================================================================

struct Bit {
  unsigned lx, bsd;  // bsd 0: accepted and dropped (nothing to do on FreeBSD)
};

template <size_t N>
unsigned bits_to_bsd(unsigned v, const Bit (&t)[N], const char* what) {
  unsigned out = 0, known = 0;
  for (const Bit& b : t)
    if (v & b.lx) out |= b.bsd, known |= b.lx;
  if (v & ~known) note_once("%s: Linux bits %#x have no FreeBSD equivalent (ignored)", what, v & ~known);
  return out;
}
template <size_t N>
unsigned bits_to_lx(unsigned v, const Bit (&t)[N]) {
  unsigned out = 0;
  for (const Bit& b : t)
    if (b.bsd && (v & b.bsd) == b.bsd) out |= b.lx;
  return out;
}

// --- open(2) flags -----------------------------------------------------------
constexpr Bit kOpen[] = {
    {0x40, O_CREAT},     {0x80, O_EXCL},      {0x100, O_NOCTTY},     {0x200, O_TRUNC},
    {0x400, O_APPEND},   {0x800, O_NONBLOCK}, {0x2000, O_ASYNC},     {0x4000, O_DIRECT},
    {0x8000, 0},         // O_LARGEFILE: always on here
    {0x10000, O_DIRECTORY}, {0x20000, O_NOFOLLOW}, {0x40000, 0},     // O_NOATIME
    {0x80000, O_CLOEXEC}, {0x200000, O_PATH},
};
constexpr unsigned kLxOSync = 0x101000, kLxODsync = 0x1000, kLxOTmpfile = 0x410000;

bool open_to_bsd(int f, int* out) {
  if ((f & kLxOTmpfile) == kLxOTmpfile) {
    note_once("open: O_TMPFILE is not on FreeBSD");
    errno = EOPNOTSUPP;
    return false;
  }
  int b = f & O_ACCMODE;
  if ((f & kLxOSync) == kLxOSync) b |= O_SYNC;
  else if (f & kLxODsync) b |= O_DSYNC;
  b |= static_cast<int>(bits_to_bsd(static_cast<unsigned>(f) & ~(O_ACCMODE | kLxOSync), kOpen, "open flags"));
  *out = b;
  return true;
}
int open_to_lx(int b) {
  int f = b & O_ACCMODE;
  f |= static_cast<int>(bits_to_lx(static_cast<unsigned>(b), kOpen));
  if (b & O_SYNC) f |= kLxOSync;
  else if (b & O_DSYNC) f |= kLxODsync;
  return f;
}
bool needs_mode(int lxflags) { return (lxflags & 0x40) || (lxflags & kLxOTmpfile) == kLxOTmpfile; }

// --- *at(2) flags --------------------------------------------------------------
enum class AtKind { plain, access, unlink };
int at_to_bsd(int f, AtKind k) {
  int b = 0;
  if (f & 0x100) b |= AT_SYMLINK_NOFOLLOW;
  if (f & 0x200) b |= k == AtKind::access ? AT_EACCESS : k == AtKind::unlink ? AT_REMOVEDIR : 0;
  if (f & 0x400) b |= AT_SYMLINK_FOLLOW;
  if (f & 0x1000) b |= AT_EMPTY_PATH;
  if (f & ~0x1f00) note_once("*at flags %#x: not known", f & ~0x1f00);
  return b;  // 0x800 (AT_NO_AUTOMOUNT): nothing to do
}

// --- clocks ----------------------------------------------------------------------
bool clock_to_bsd(clockid_t c, clockid_t* out) {
  // Negative ids are CPU-time clocks FreeBSD itself handed out
  // (pthread_getcpuclockid): they go back unchanged.
  if (c < 0) return *out = c, true;
  switch (c) {
    case 0: *out = CLOCK_REALTIME; return true;
    case 1: *out = CLOCK_MONOTONIC; return true;
    case 2: *out = CLOCK_PROCESS_CPUTIME_ID; return true;
    case 3: *out = CLOCK_THREAD_CPUTIME_ID; return true;
    case 4: *out = CLOCK_MONOTONIC_PRECISE; return true;   // MONOTONIC_RAW
    case 5: *out = CLOCK_REALTIME_FAST; return true;       // REALTIME_COARSE
    case 6: *out = CLOCK_MONOTONIC_FAST; return true;      // MONOTONIC_COARSE
    case 7: *out = CLOCK_BOOTTIME; return true;
    case 8: *out = CLOCK_REALTIME; return true;            // REALTIME_ALARM
    case 9: *out = CLOCK_BOOTTIME; return true;            // BOOTTIME_ALARM
    case 11:                                               // TAI
      note_once("CLOCK_TAI: FreeBSD has none here, using CLOCK_REALTIME");
      *out = CLOCK_REALTIME;
      return true;
  }
  note_once("clock id %d: not known", static_cast<int>(c));
  return false;
}
clockid_t clock_to_lx(clockid_t b) {
  switch (b) {
    case CLOCK_REALTIME: return 0;
    case CLOCK_MONOTONIC: return 1;
    case CLOCK_PROCESS_CPUTIME_ID: return 2;
    case CLOCK_THREAD_CPUTIME_ID: return 3;
    case CLOCK_MONOTONIC_PRECISE: return 4;
    case CLOCK_REALTIME_FAST: return 5;
    case CLOCK_MONOTONIC_FAST: return 6;
    case CLOCK_UPTIME: return 7;
  }
  return b;
}

// Fast CLOCK_MONOTONIC where FreeBSD has no user-space clock (VMs): real time
// every 2 ms, the cycle counter in between, never backwards, back to the real
// clock on >50 us drift. TONIC_CLOCK=plain turns it off.
std::atomic<bool> g_fast_clock{false};
double g_cyc_per_ns = 0;     // (measured at start-up)
uint64_t g_resync_cyc = 0;   // 2 ms in cycles
std::atomic<uint64_t> g_n_clock_real{0};
std::atomic<int> g_clock_drift_bad{0};
struct ClockAnchor {
  uint64_t tsc = 0;
  int64_t ns = 0;
};
thread_local ClockAnchor t_anchor[2];  // 0: MONOTONIC (and RAW), 1: BOOTTIME (FreeBSD: same as UPTIME)
std::atomic<int64_t> g_clock_max[2];
int64_t real_ns(clockid_t b) {
  timespec t{};
  clock_gettime(b, &t);
  return static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}
bool fast_clock(int k, clockid_t b, timespec* ts) {
  if (!g_fast_clock.load(std::memory_order_relaxed)) return false;
  ClockAnchor& a = t_anchor[k];
  const uint64_t now = __builtin_ia32_rdtsc();
  int64_t ns;
  // (now < a.tsc, a thread on a core whose counter is behind, wraps round
  // to a huge difference: a real read too)
  if (a.tsc == 0 || now - a.tsc > g_resync_cyc) {
    count(g_n_clock_real);
    ns = real_ns(b);
    const uint64_t after = __builtin_ia32_rdtsc();
    // (checked only when the read itself was quick: a thread paused in
    // between would look like drift)
    if (a.tsc != 0 && now - a.tsc <= 4 * g_resync_cyc && after - now < g_resync_cyc / 100) {
      const int64_t counted = a.ns + static_cast<int64_t>((now - a.tsc) / g_cyc_per_ns);
      const int64_t off = counted > ns ? counted - ns : ns - counted;
      if (off > 50000 && g_clock_drift_bad.fetch_add(1, std::memory_order_relaxed) == 20) {
        g_fast_clock = false;
        fprintf(stderr, "[bsdabi] the cycle counter is not steady here: the fast clock is off again\n");
      }
    }
    a.tsc = after;
    a.ns = ns;
  } else {
    ns = a.ns + static_cast<int64_t>((now - a.tsc) / g_cyc_per_ns);
  }
  int64_t prev = g_clock_max[k].load(std::memory_order_relaxed);
  while (ns > prev && !g_clock_max[k].compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
  }
  if (ns < prev) ns = prev;
  ts->tv_sec = ns / 1000000000LL;
  ts->tv_nsec = ns % 1000000000LL;
  return true;
}
// Called once the cycle counter's speed is known (measure_yield_gap).
void init_fast_clock() {
  if (const char* e = getenv("TONIC_CLOCK"); e && !strcmp(e, "plain")) return;
  if (g_cyc_per_ns <= 0) return;
  // What a real read costs here.
  const uint64_t t0 = __builtin_ia32_rdtsc();
  for (int i = 0; i < 2000; ++i) real_ns(CLOCK_MONOTONIC);
  const double cost_ns = (__builtin_ia32_rdtsc() - t0) / g_cyc_per_ns / 2000;
  if (cost_ns < 200) return;  // FreeBSD's own clock is fast here already
  g_resync_cyc = static_cast<uint64_t>(g_cyc_per_ns * 2000000.0);
  g_fast_clock = true;
  fprintf(stderr, "[bsdabi] clock reads cost %.0f ns here: using the fast clock (TONIC_CLOCK=plain: off)\n", cost_ns);
}

int sh_clock_gettime(clockid_t c, timespec* ts) {
  count(g_n_clock);
  if (c == 1 || c == 4) {
    if (fast_clock(0, CLOCK_MONOTONIC, ts)) return 0;
  } else if (c == 7) {
    if (fast_clock(1, CLOCK_BOOTTIME, ts)) return 0;
  }
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return errno = EINVAL, -1;
  return clock_gettime(b, ts);
}
int sh_clock_getres(clockid_t c, timespec* ts) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return errno = EINVAL, -1;
  return clock_getres(b, ts);
}
int sh_clock_settime(clockid_t c, const timespec* ts) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return errno = EINVAL, -1;
  return clock_settime(b, ts);
}
int sh_clock_nanosleep(clockid_t c, int flags, const timespec* req, timespec* rem) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return 22;
  const uint64_t deadline = !g_stats ? 0 : (flags & 1) ? ts_ns(req) : now_ns(b) + ts_ns(req);
  const int r = clock_nanosleep(b, flags & 1 ? TIMER_ABSTIME : 0, req, rem);
  if (r == 0) note_timeout(g_w_sleep, b, deadline);
  return rc(r);
}

timespec ts_add(timespec a, long sec, long nsec) {
  a.tv_sec += sec;
  a.tv_nsec += nsec;
  while (a.tv_nsec >= 1000000000L) a.tv_nsec -= 1000000000L, ++a.tv_sec;
  while (a.tv_nsec < 0) a.tv_nsec += 1000000000L, --a.tv_sec;
  return a;
}
// An absolute time on clock C, as an absolute CLOCK_REALTIME time (what
// FreeBSD's timed waits use).
timespec abs_to_realtime(clockid_t bsd_clock, const timespec* abs) {
  timespec now_c{}, now_r{};
  clock_gettime(bsd_clock, &now_c);
  clock_gettime(CLOCK_REALTIME, &now_r);
  if (bsd_clock == CLOCK_REALTIME) return *abs;
  return ts_add(now_r, abs->tv_sec - now_c.tv_sec, abs->tv_nsec - now_c.tv_nsec);
}
timespec rel_to_realtime(const timespec* rel) {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return ts_add(now, rel->tv_sec, rel->tv_nsec);
}

// ===========================================================================
// Files
// ===========================================================================

// /proc/meminfo and /proc/self/statm: FreeBSD's own memory figures, in the
// format Linux uses (what FreeBSD's linprocfs gives too). Only these two;
// everything else under /proc stays "no such file".
template <typename T>
T sysctl_value(const char* name) {
  T v{};
  size_t n = sizeof v;
  if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) return T{};
  return v;
}
// /proc/self/NAME or /proc/<our pid>/NAME (also /proc/thread-self/NAME).
bool is_self(const char* p, const char* name) {
  if (!p) return false;
  char mine[96];
  snprintf(mine, sizeof mine, "/proc/self/%s", name);
  if (!strcmp(p, mine)) return true;
  snprintf(mine, sizeof mine, "/proc/thread-self/%s", name);
  if (!strcmp(p, mine)) return true;
  snprintf(mine, sizeof mine, "/proc/%d/%s", static_cast<int>(getpid()), name);
  return !strcmp(p, mine);
}
bool is_self_statm(const char* p) { return is_self(p, "statm"); }
// Anything under /proc the library looks at that tonic does not serve: one
// log line each, so a check that fails on FreeBSD shows up in the log.
void proc_note(const char* p) {
  if (p && !strncmp(p, "/proc", 5) && (p[5] == '/' || p[5] == 0)) note_once("/proc: the library looked at %s", p);
}
// /proc/self/maps in Linux's format, from the kernel's own map of this process
// (KERN_PROC_VMMAP, what FreeBSD's linprocfs shows as maps too).
bool self_maps(std::string& text) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid()};
  std::vector<char> buf;
  size_t len = 0;
  for (int tries = 0;; ++tries) {
    if (tries == 4 || sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0) return false;
    len += len / 2 + 4096;
    buf.resize(len);
    if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) == 0) break;
    if (errno != ENOMEM) return false;
  }
  for (size_t at = 0; at + sizeof(int) <= len;) {
    const auto* kv = reinterpret_cast<const kinfo_vmentry*>(buf.data() + at);
    if (kv->kve_structsize <= 0) break;
    at += static_cast<size_t>(kv->kve_structsize);
    const bool file = kv->kve_type == KVME_TYPE_VNODE;
    const bool shared = (kv->kve_flags & (KVME_FLAG_SYSVSHM | KVME_FLAG_POSIXSHM)) ||
                        (file && !(kv->kve_flags & KVME_FLAG_COW));  // (COW: private file mapping)
    const uint64_t fsid = file ? static_cast<uint64_t>(kv->kve_vn_fsid) : 0;
    char line[160];
    int n = snprintf(line, sizeof line, "%08llx-%08llx %c%c%c%c %08llx %02x:%02x %llu",
                     static_cast<unsigned long long>(kv->kve_start), static_cast<unsigned long long>(kv->kve_end),
                     kv->kve_protection & KVME_PROT_READ ? 'r' : '-', kv->kve_protection & KVME_PROT_WRITE ? 'w' : '-',
                     kv->kve_protection & KVME_PROT_EXEC ? 'x' : '-', shared ? 's' : 'p',
                     static_cast<unsigned long long>(file ? kv->kve_offset : 0),
                     static_cast<unsigned>((fsid >> 8) & 0xfff), static_cast<unsigned>(fsid & 0xff),
                     static_cast<unsigned long long>(file ? kv->kve_vn_fileid : 0));
    if (n < 0) continue;
    text.append(line, static_cast<size_t>(std::min<int>(n, sizeof line - 1)));
    const char* name = file && kv->kve_path[0] ? kv->kve_path : (kv->kve_flags & KVME_FLAG_GROWS_DOWN) ? "[stack]" : "";
    if (*name) text.append(static_cast<size_t>(std::max(1, 73 - n)), ' ').append(name);
    text += '\n';
  }
  return true;
}
bool proc_more_path(const char* p);
bool proc_more(const char* p, std::string& text);
const char* proc_fd_redirect(const char* p, char* buf, size_t n);
void fdescfs_check();
ssize_t proc_fd_link(const char* p, char* b, size_t n, bool* handled);
// A read-only fd with the file's text, or -1 (errno ENOENT if not one of ours).
int proc_file(const char* p) {
  if (!p || strncmp(p, "/proc/", 6) != 0) return errno = ENOENT, -1;
  std::string text;
  const uint64_t pg = static_cast<uint64_t>(getpagesize());
  if (!strcmp(p, "/proc/meminfo")) {
    const uint64_t total = sysctl_value<unsigned long>("hw.physmem") / 1024;
    const uint64_t free_kb = sysctl_value<unsigned>("vm.stats.vm.v_free_count") * pg / 1024;
    const uint64_t inactive_kb = sysctl_value<unsigned>("vm.stats.vm.v_inactive_count") * pg / 1024;
    char b[512];
    snprintf(b, sizeof b,
             "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\n"
             "Buffers:        %8llu kB\nCached:         %8llu kB\nSwapCached:     %8llu kB\n",
             static_cast<unsigned long long>(total), static_cast<unsigned long long>(free_kb),
             static_cast<unsigned long long>(free_kb + inactive_kb), 0ULL,
             static_cast<unsigned long long>(inactive_kb), 0ULL);
    text = b;
  } else if (is_self_statm(p)) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    kinfo_proc kp{};
    size_t n = sizeof kp;
    if (sysctl(mib, 4, &kp, &n, nullptr, 0) != 0) return -1;
    char b[160];
    snprintf(b, sizeof b, "%llu %llu 0 %llu 0 %llu 0\n", static_cast<unsigned long long>(kp.ki_size / pg),
             static_cast<unsigned long long>(kp.ki_rssize), static_cast<unsigned long long>(kp.ki_tsize),
             static_cast<unsigned long long>(kp.ki_dsize + kp.ki_ssize));
    text = b;
  } else if (is_self(p, "maps")) {
    if (!self_maps(text)) return errno = EACCES, -1;
  } else if (proc_more_path(p)) {
    if (!proc_more(p, text)) return -1;
  } else {
    proc_note(p);
    return errno = ENOENT, -1;
  }
  const int fd = memfd_create("tonic-proc", MFD_CLOEXEC);
  if (fd < 0) return -1;
  if (write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
    close(fd);
    return errno = EIO, -1;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}
bool proc_path(const char* p) {
  return p && (!strcmp(p, "/proc/meminfo") || is_self_statm(p) || is_self(p, "maps") || proc_more_path(p));
}
int track_proc_fd(int fd);
FILE* sh_fopen(const char* p, const char* mode) {
  if (proc_path(p)) {
    const int fd = track_proc_fd(proc_file(p));
    if (fd < 0) return nullptr;
    FILE* f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
  }
  proc_note(p);
  return fopen(p, mode);
}

// Descriptors the library holds on /proc (the directory, or a file tonic
// serves), so fstatfs on them answers like statfs on the path (sh_fstatfs).
// Kept with the file's device and inode: close() is not ours, so a number
// reused for another file no longer matches and is dropped.
std::mutex g_procfd_mu;
std::map<int, std::pair<dev_t, ino_t>> g_procfd;
int track_proc_fd(int fd) {
  struct stat st;
  if (fd >= 0 && fstat(fd, &st) == 0) {
    std::lock_guard<std::mutex> l(g_procfd_mu);
    g_procfd[fd] = {st.st_dev, st.st_ino};
  }
  return fd;
}
bool is_proc_fd(int fd) {
  std::lock_guard<std::mutex> l(g_procfd_mu);
  const auto it = g_procfd.find(fd);
  if (it == g_procfd.end()) return false;
  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_dev == it->second.first && st.st_ino == it->second.second) return true;
  g_procfd.erase(it);
  return false;
}
bool proc_dir_path(const char* p) { return p && (!strcmp(p, "/proc") || !strcmp(p, "/proc/")); }

int sh_open(const char* p, int f, ...) {
  mode_t m = 0;
  if (needs_mode(f)) {
    va_list ap;
    va_start(ap, f);
    m = static_cast<mode_t>(va_arg(ap, int));
    va_end(ap);
  }
  if (proc_path(p)) return track_proc_fd(proc_file(p));
  int b;
  if (!open_to_bsd(f, &b)) return -1;
  char fdbuf[64];
  if (const char* r = proc_fd_redirect(p, fdbuf, sizeof fdbuf)) return fdescfs_check(), open(r, b, m);
  if (!proc_dir_path(p)) proc_note(p);
  const int fd = open(p, b, m);
  return proc_dir_path(p) ? track_proc_fd(fd) : fd;
}
int sh_openat(int d, const char* p, int f, ...) {
  mode_t m = 0;
  if (needs_mode(f)) {
    va_list ap;
    va_start(ap, f);
    m = static_cast<mode_t>(va_arg(ap, int));
    va_end(ap);
  }
  if (proc_path(p)) return track_proc_fd(proc_file(p));  // (absolute: the directory does not matter)
  int b;
  if (!open_to_bsd(f, &b)) return -1;
  char fdbuf[64];
  if (const char* r = proc_fd_redirect(p, fdbuf, sizeof fdbuf)) return fdescfs_check(), open(r, b, m);
  if (!proc_dir_path(p)) proc_note(p);
  const int fd = openat(d, p, b, m);
  return proc_dir_path(p) ? track_proc_fd(fd) : fd;
}
int sh_open_2(const char* p, int f) { return sh_open(p, f, 0); }
int sh_openat_2(int d, const char* p, int f) { return sh_openat(d, p, f, 0); }

int sh_pipe2(int fds[2], int f) {
  int b;
  if (!open_to_bsd(f, &b)) return -1;
  return pipe2(fds, b & (O_CLOEXEC | O_NONBLOCK));  // O_DIRECT (packet mode): FreeBSD pipes have none
}
int sh_dup3(int a, int b, int f) { return dup3(a, b, f & 0x80000 ? O_CLOEXEC : 0); }
int sh_mkostemp(char* t, int f) {
  int b;
  if (!open_to_bsd(f, &b)) return -1;
  return mkostemp(t, b & ~O_ACCMODE);
}
int sh_mkostemps(char* t, int sl, int f) {
  int b;
  if (!open_to_bsd(f, &b)) return -1;
  return mkostemps(t, sl, b & ~O_ACCMODE);
}

// struct flock: Linux puts l_type and l_whence first, FreeBSD last, and the
// lock type numbers differ.
struct LxFlock {
  short l_type, l_whence;
  int64_t l_start, l_len;
  int l_pid;
};
static_assert(sizeof(LxFlock) == 32, "Linux x86_64 struct flock is 32 bytes");

int sh_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  const long arg = va_arg(ap, long);
  va_end(ap);
  switch (cmd) {
    case 0: return fcntl(fd, F_DUPFD, static_cast<int>(arg));
    case 1: return fcntl(fd, F_GETFD);
    case 2: return fcntl(fd, F_SETFD, static_cast<int>(arg));  // FD_CLOEXEC is 1 on both
    case 3: {
      const int r = fcntl(fd, F_GETFL);
      return r < 0 ? r : open_to_lx(r);
    }
    case 4: {
      int b;
      if (!open_to_bsd(static_cast<int>(arg) & ~O_ACCMODE, &b)) return -1;
      return fcntl(fd, F_SETFL, b);
    }
    case 5: case 6: case 7: {  // F_GETLK, F_SETLK, F_SETLKW
      auto* l = reinterpret_cast<LxFlock*>(arg);
      if (!l) return errno = EFAULT, -1;
      struct flock b{};
      b.l_type = l->l_type == 0 ? F_RDLCK : l->l_type == 1 ? F_WRLCK : F_UNLCK;
      b.l_whence = l->l_whence;
      b.l_start = l->l_start;
      b.l_len = l->l_len;
      b.l_pid = l->l_pid;
      const int r = fcntl(fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &b);
      if (r == 0 && cmd == 5) {
        l->l_type = b.l_type == F_RDLCK ? 0 : b.l_type == F_WRLCK ? 1 : 2;
        l->l_whence = b.l_whence;
        l->l_start = b.l_start;
        l->l_len = b.l_len;
        l->l_pid = b.l_pid;
      }
      return r;
    }
    case 8: return fcntl(fd, F_SETOWN, static_cast<int>(arg));
    case 9: return fcntl(fd, F_GETOWN);
    case 1030: return fcntl(fd, F_DUPFD_CLOEXEC, static_cast<int>(arg));
    case 1031: return static_cast<int>(arg);  // F_SETPIPE_SZ: FreeBSD pipes size themselves
    case 1032: return 65536;                  // F_GETPIPE_SZ
    case 1033: return fcntl(fd, F_ADD_SEALS, static_cast<int>(arg));  // seal bits match
    case 1034: return fcntl(fd, F_GET_SEALS);
  }
  note_once("fcntl command %d: not on FreeBSD", cmd);
  errno = EINVAL;
  return -1;
}

int sh_eventfd(unsigned v, int f) {
  static constexpr Bit k[] = {{1, EFD_SEMAPHORE}, {0x800, EFD_NONBLOCK}, {0x80000, EFD_CLOEXEC}};
  return eventfd(v, static_cast<int>(bits_to_bsd(static_cast<unsigned>(f), k, "eventfd flags")));
}
int sh_timerfd_create(int c, int f) {
  static constexpr Bit k[] = {{0x800, TFD_NONBLOCK}, {0x80000, TFD_CLOEXEC}};
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return errno = EINVAL, -1;
  return timerfd_create(b, static_cast<int>(bits_to_bsd(static_cast<unsigned>(f), k, "timerfd flags")));
}
int sh_timerfd_settime(int fd, int f, const itimerspec* n, itimerspec* o) {
  static constexpr Bit k[] = {{1, TFD_TIMER_ABSTIME}, {2, TFD_TIMER_CANCEL_ON_SET}};
  return timerfd_settime(fd, static_cast<int>(bits_to_bsd(static_cast<unsigned>(f), k, "timerfd_settime flags")), n, o);
}

// epoll comes from epoll-shim, which uses Linux's event bits and
// O_CLOEXEC (FreeBSD's) for EPOLL_CLOEXEC. Looked up at run time so this file
// does not need its headers.
int sh_epoll_create1(int f) {
  static auto fn = reinterpret_cast<int (*)(int)>(dlsym(RTLD_DEFAULT, "epoll_create1"));
  if (!fn) return errno = ENOSYS, -1;
  return fn(f & 0x80000 ? O_CLOEXEC : 0);
}

// --- stat --------------------------------------------------------------------------
struct LxStat {
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, pad0;
  uint64_t st_rdev;
  int64_t st_size, st_blksize, st_blocks;
  timespec st_atim, st_mtim, st_ctim;
  int64_t unused[3];
};
static_assert(sizeof(LxStat) == 144, "Linux x86_64 struct stat is 144 bytes");

void stat_to_lx(const struct stat& s, LxStat* o) {
  memset(o, 0, sizeof *o);
  o->st_dev = s.st_dev;
  o->st_ino = s.st_ino;
  o->st_nlink = s.st_nlink;
  o->st_mode = s.st_mode;  // S_IF* and permission bits are the same octal values
  o->st_uid = s.st_uid;
  o->st_gid = s.st_gid;
  o->st_rdev = s.st_rdev;
  o->st_size = s.st_size;
  o->st_blksize = s.st_blksize;
  o->st_blocks = s.st_blocks;
  o->st_atim = s.st_atim;
  o->st_mtim = s.st_mtim;
  o->st_ctim = s.st_ctim;
}
// The /proc files tonic serves, as stat sees them on Linux: read-only regular
// files of size 0, owned by us. (Otherwise open works but stat says "no such file".)
bool proc_stat(const char* p, LxStat* o) {
  char fdbuf[64];
  if (const char* r = proc_fd_redirect(p, fdbuf, sizeof fdbuf)) {
    struct stat s;
    if (stat(r, &s) != 0) return false;  // (errno from FreeBSD; the caller's stat then fails too)
    stat_to_lx(s, o);
    return true;
  }
  if (!proc_path(p)) {
    proc_note(p);
    return false;
  }
  memset(o, 0, sizeof *o);
  o->st_dev = 0x16;  // (a proc mount's device on Linux, 0:22)
  o->st_ino = 0x10000 + strlen(p);
  o->st_nlink = 1;
  o->st_mode = S_IFREG | 0444;
  o->st_uid = getuid();
  o->st_gid = getgid();
  o->st_blksize = 1024;
  clock_gettime(CLOCK_REALTIME, &o->st_atim);
  o->st_mtim = o->st_ctim = o->st_atim;
  return true;
}
int sh_stat(const char* p, LxStat* o) {
  if (proc_stat(p, o)) return 0;
  struct stat s;
  const int r = stat(p, &s);
  if (r == 0) stat_to_lx(s, o);
  return r;
}
int sh_lstat(const char* p, LxStat* o) {
  if (proc_stat(p, o)) return 0;
  struct stat s;
  const int r = lstat(p, &s);
  if (r == 0) stat_to_lx(s, o);
  return r;
}
int sh_fstat(int fd, LxStat* o) {
  struct stat s;
  const int r = fstat(fd, &s);
  if (r == 0) stat_to_lx(s, o);
  return r;
}
int sh_fstatat(int d, const char* p, LxStat* o, int f) {
  if (proc_stat(p, o)) return 0;  // (absolute: the directory does not matter)
  struct stat s;
  const int r = fstatat(d, p, &s, at_to_bsd(f, AtKind::plain));
  if (r == 0) stat_to_lx(s, o);
  return r;
}

// --- statfs / statvfs -----------------------------------------------------------------
struct LxStatfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static_assert(sizeof(LxStatfs) == 120, "Linux x86_64 struct statfs is 120 bytes");

// Linux reports the file system as a magic number; FreeBSD by name.
uint64_t fs_magic(const char* name) {
  static const struct { const char* n; uint64_t m; } k[] = {
      {"ufs", 0x00011954},  {"zfs", 0x2fc12fc1}, {"tmpfs", 0x01021994}, {"nfs", 0x6969},
      {"msdosfs", 0x4d44},  {"devfs", 0x1373},   {"procfs", 0x9fa0},    {"fusefs", 0x65735546},
      {"ext2fs", 0xef53},   {"cd9660", 0x9660},  {"nullfs", 0x794c7630},
      {"linprocfs", 0x9fa0},  // FreeBSD's Linux-style /proc is still Linux's proc (as Mocktail's Linuxulator patch)
  };
  for (const auto& e : k)
    if (!strcmp(name, e.n)) return e.m;
  return 0;
}
void statfs_to_lx(const struct statfs& s, LxStatfs* o) {
  memset(o, 0, sizeof *o);
  o->f_type = fs_magic(s.f_fstypename);
  o->f_bsize = s.f_bsize;
  o->f_blocks = s.f_blocks;
  o->f_bfree = s.f_bfree;
  o->f_bavail = s.f_bavail < 0 ? 0 : static_cast<uint64_t>(s.f_bavail);
  o->f_files = s.f_files;
  o->f_ffree = s.f_ffree < 0 ? 0 : static_cast<uint64_t>(s.f_ffree);
  o->f_fsid[0] = s.f_fsid.val[0];
  o->f_fsid[1] = s.f_fsid.val[1];
  o->f_namelen = s.f_namemax;
  o->f_frsize = s.f_bsize;
  uint64_t fl = 0x20;  // ST_VALID
  if (s.f_flags & MNT_RDONLY) fl |= 1;
  if (s.f_flags & MNT_NOSUID) fl |= 2;
  if (s.f_flags & MNT_NOEXEC) fl |= 8;
  if (s.f_flags & MNT_SYNCHRONOUS) fl |= 16;
  if (s.f_flags & MNT_NOATIME) fl |= 1024;
  o->f_flags = fl;
}
// /proc is the library's procfs: on Linux it is always mounted there, and tonic
// serves the files it reads (proc_file above). On FreeBSD /proc is often an
// empty directory on the root file system (procfs is not mounted by default),
// so statfs said "ufs"/"zfs". Roblox reads that as a broken install and
// disconnects with Error 304 after about a minute (the same check Mocktail's
// FreeBSD notes describe). So /proc itself and the files tonic serves under it
// report the proc file system, as Linux does.
bool proc_fs_path(const char* p) { return proc_dir_path(p) || proc_path(p); }
void proc_statfs(LxStatfs* o) {
  memset(o, 0, sizeof *o);
  o->f_type = 0x9fa0;  // PROC_SUPER_MAGIC
  o->f_bsize = o->f_frsize = static_cast<uint64_t>(getpagesize());
  o->f_namelen = 255;
  o->f_flags = 0x20 | 2 | 4 | 8 | 1024;  // ST_VALID|NOSUID|NODEV|NOEXEC|NOATIME, as Linux mounts it
}
int sh_statfs(const char* p, LxStatfs* o) {
  struct statfs s;
  const int r = statfs(p, &s);
  if (r == 0) statfs_to_lx(s, o);
  if (proc_fs_path(p) && (r != 0 || o->f_type != 0x9fa0)) {
    note_once("statfs /proc: no proc file system mounted there; reporting tonic's own /proc");
    proc_statfs(o);
    return 0;
  }
  return r;
}
int sh_fstatfs(int fd, LxStatfs* o) {
  struct statfs s;
  const int r = fstatfs(fd, &s);
  if (r == 0) statfs_to_lx(s, o);
  if (is_proc_fd(fd) && (r != 0 || o->f_type != 0x9fa0)) {
    note_once("fstatfs on /proc: no proc file system mounted there; reporting tonic's own /proc");
    proc_statfs(o);
    return 0;
  }
  return r;
}

struct LxStatvfs {
  unsigned long f_bsize, f_frsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
  unsigned long f_fsid, f_flag, f_namemax;
  uint32_t reserved[6];
};
static_assert(sizeof(LxStatvfs) == 112, "Linux x86_64 struct statvfs is 112 bytes");
void statvfs_to_lx(const struct statvfs& s, LxStatvfs* o) {
  memset(o, 0, sizeof *o);
  o->f_bsize = s.f_bsize;
  o->f_frsize = s.f_frsize;
  o->f_blocks = s.f_blocks;
  o->f_bfree = s.f_bfree;
  o->f_bavail = s.f_bavail;
  o->f_files = s.f_files;
  o->f_ffree = s.f_ffree;
  o->f_favail = s.f_favail;
  o->f_fsid = s.f_fsid;
  o->f_flag = s.f_flag & (ST_RDONLY | ST_NOSUID);  // 1 and 2 on both
  o->f_namemax = s.f_namemax;
}
int sh_statvfs(const char* p, LxStatvfs* o) {
  struct statvfs s;
  const int r = statvfs(p, &s);
  if (r == 0) statvfs_to_lx(s, o);
  return r;
}
int sh_fstatvfs(int fd, LxStatvfs* o) {
  struct statvfs s;
  const int r = fstatvfs(fd, &s);
  if (r == 0) statvfs_to_lx(s, o);
  return r;
}

// --- directories ----------------------------------------------------------------------
struct LxDirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};
static_assert(offsetof(LxDirent, d_name) == 19, "Linux struct dirent has d_name at 19");

void dirent_to_lx(const dirent* e, LxDirent* o) {
  o->d_ino = e->d_fileno;
  o->d_off = e->d_off;
  o->d_type = e->d_type;  // DT_* values match
  const size_t n = std::min<size_t>(e->d_namlen, sizeof o->d_name - 1);
  memcpy(o->d_name, e->d_name, n);
  o->d_name[n] = '\0';
  o->d_reclen = static_cast<uint16_t>((offsetof(LxDirent, d_name) + n + 1 + 7) & ~size_t{7});
}

std::mutex g_dir_mu;
std::unordered_map<DIR*, LxDirent*> g_dirs;  // readdir's per-stream entry

LxDirent* sh_readdir(DIR* d) {
  const dirent* e = readdir(d);
  if (!e) return nullptr;
  LxDirent* o;
  {
    std::lock_guard<std::mutex> l(g_dir_mu);
    LxDirent*& slot = g_dirs[d];
    if (!slot) slot = new LxDirent;
    o = slot;
  }
  dirent_to_lx(e, o);
  return o;
}
int sh_readdir_r(DIR* d, LxDirent* entry, LxDirent** result) {
  const int saved = errno;
  errno = 0;
  const dirent* e = readdir(d);
  if (!e) {
    *result = nullptr;
    const int r = errno;
    errno = saved;
    return rc(r);
  }
  errno = saved;
  dirent_to_lx(e, entry);
  *result = entry;
  return 0;
}
int sh_closedir(DIR* d) {
  {
    std::lock_guard<std::mutex> l(g_dir_mu);
    auto it = g_dirs.find(d);
    if (it != g_dirs.end()) {
      delete it->second;
      g_dirs.erase(it);
    }
  }
  return closedir(d);
}
int sh_alphasort(const LxDirent** a, const LxDirent** b) { return strcoll((*a)->d_name, (*b)->d_name); }
int sh_scandir(const char* dir, LxDirent*** list, int (*filter)(const LxDirent*),
               int (*compar)(const LxDirent**, const LxDirent**)) {
  DIR* d = opendir(dir);
  if (!d) return -1;
  std::vector<LxDirent*> v;
  while (const dirent* e = readdir(d)) {
    auto* o = static_cast<LxDirent*>(malloc(sizeof(LxDirent)));
    if (!o) break;
    dirent_to_lx(e, o);
    if (filter && !filter(o)) {
      free(o);
      continue;
    }
    v.push_back(o);
  }
  closedir(d);
  if (compar)
    std::sort(v.begin(), v.end(), [&](LxDirent* a, LxDirent* b) {
      return compar(const_cast<const LxDirent**>(&a), const_cast<const LxDirent**>(&b)) < 0;
    });
  auto* out = static_cast<LxDirent**>(malloc(sizeof(LxDirent*) * (v.size() ? v.size() : 1)));
  if (!out) {
    for (auto* p : v) free(p);
    return errno = ENOMEM, -1;
  }
  std::copy(v.begin(), v.end(), out);
  *list = out;
  return static_cast<int>(v.size());
}

// --- other *at calls and times ----------------------------------------------------------
int sh_faccessat(int d, const char* p, int m, int f) {
  if (proc_path(p)) return (m & 2) ? (errno = EACCES, -1) : 0;  // (W_OK; served files are read-only)
  proc_note(p);
  return faccessat(d, p, m, at_to_bsd(f, AtKind::access));
}
int sh_access(const char* p, int m) { return sh_faccessat(AT_FDCWD, p, m, 0); }
// /proc/self/exe: tonic's own path (KERN_PROC_PATHNAME, what linprocfs gives).
ssize_t self_exe(char* b, size_t n) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  char path[PATH_MAX];
  size_t len = sizeof path;
  if (sysctl(mib, 4, path, &len, nullptr, 0) != 0) return -1;
  const size_t l = strnlen(path, sizeof path);
  const size_t c = std::min(l, n);
  memcpy(b, path, c);  // (readlink does not terminate)
  return static_cast<ssize_t>(c);
}
ssize_t sh_readlink(const char* p, char* b, size_t n) {
  if (is_self(p, "exe")) return self_exe(b, n);
  bool fdl = false;
  const ssize_t fr = proc_fd_link(p, b, n, &fdl);
  if (fdl) return fr;
  proc_note(p);
  return readlink(p, b, n);
}
ssize_t sh_readlinkat(int d, const char* p, char* b, size_t n) {
  if (is_self(p, "exe")) return self_exe(b, n);
  bool fdl = false;
  const ssize_t fr = proc_fd_link(p, b, n, &fdl);
  if (fdl) return fr;
  proc_note(p);
  return readlinkat(d, p, b, n);
}
DIR* sh_opendir(const char* p) {
  char fdbuf[64];
  if (const char* r = proc_fd_redirect(p, fdbuf, sizeof fdbuf)) return fdescfs_check(), opendir(r);
  proc_note(p);
  return opendir(p);
}
int sh_unlinkat(int d, const char* p, int f) { return unlinkat(d, p, at_to_bsd(f, AtKind::unlink)); }
int sh_fchmodat(int d, const char* p, mode_t m, int f) { return fchmodat(d, p, m, at_to_bsd(f, AtKind::plain)); }
int sh_fchownat(int d, const char* p, uid_t u, gid_t g, int f) {
  return fchownat(d, p, u, g, at_to_bsd(f, AtKind::plain));
}
int sh_linkat(int a, const char* p, int b, const char* q, int f) { return linkat(a, p, b, q, at_to_bsd(f, AtKind::plain)); }
int sh_renameat2(int a, const char* p, int b, const char* q, unsigned f) {
  if (f) return note_once("renameat2 flags %#x: not on FreeBSD", f), errno = EINVAL, -1;
  return renameat(a, p, b, q);
}
void times_to_bsd(const timespec* in, timespec* out) {
  for (int i = 0; i < 2; ++i) {
    out[i] = in[i];
    if (in[i].tv_nsec == (1L << 30) - 1) out[i].tv_nsec = UTIME_NOW;
    else if (in[i].tv_nsec == (1L << 30) - 2) out[i].tv_nsec = UTIME_OMIT;
  }
}
int sh_utimensat(int d, const char* p, const timespec* t, int f) {
  timespec b[2];
  if (t) times_to_bsd(t, b);
  return utimensat(d, p, t ? b : nullptr, at_to_bsd(f, AtKind::plain));
}
int sh_futimens(int fd, const timespec* t) {
  timespec b[2];
  if (t) times_to_bsd(t, b);
  return futimens(fd, t ? b : nullptr);
}

ssize_t sh_sendfile(int out, int in, off_t* off, size_t count) {
  std::vector<char> buf(65536);
  size_t done = 0;
  while (done < count) {
    const size_t want = std::min(buf.size(), count - done);
    const ssize_t r = off ? pread(in, buf.data(), want, *off + static_cast<off_t>(done)) : read(in, buf.data(), want);
    if (r < 0) {
      if (done) break;
      return -1;
    }
    if (r == 0) break;
    ssize_t w = 0;
    while (w < r) {
      const ssize_t k = write(out, buf.data() + w, static_cast<size_t>(r - w));
      if (k <= 0) break;
      w += k;
    }
    done += static_cast<size_t>(w);
    if (w < r) {
      if (!off) lseek(in, w - r, SEEK_CUR);  // give back what was read but not written
      if (!done) return -1;
      break;
    }
  }
  if (off) *off += static_cast<off_t>(done);
  return static_cast<ssize_t>(done);
}
// posix_fallocate where the file system cannot preallocate (ZFS returns
// EINVAL): make the file at least OFF+LEN long, as glibc does when the kernel
// has no fallocate. The space is not reserved, but reading and mmap work.
// Returns 0 or a FreeBSD errno.
int fallocate_or_extend(int fd, off_t off, off_t len) {
  if (off < 0 || len <= 0) return EINVAL;
  int r = posix_fallocate(fd, off, len);
  if (r != EINVAL && r != EOPNOTSUPP && r != ENODEV) return r;
  struct stat st;
  if (fstat(fd, &st) != 0) return errno;
  if (!S_ISREG(st.st_mode)) return r;
  note_once("posix_fallocate: this file system cannot preallocate; extending the file instead");
  if (st.st_size < off + len && ftruncate(fd, off + len) != 0) return errno;
  return 0;
}
int sh_posix_fallocate(int fd, off_t off, off_t len) { return rc(fallocate_or_extend(fd, off, len)); }
int sh_fallocate(int fd, int mode, off_t off, off_t len) {
  if (mode) {
    note_once("fallocate mode %#x: not on FreeBSD", mode);
    return errno = EOPNOTSUPP, -1;
  }
  const int r = fallocate_or_extend(fd, off, len);
  if (r) return errno = r, -1;
  return 0;
}

// ===========================================================================
// Memory
// ===========================================================================

constexpr Bit kMmap[] = {
    {0x1, MAP_SHARED},   {0x2, MAP_PRIVATE},     {0x10, MAP_FIXED},  {0x20, MAP_ANON},
    {0x40, MAP_32BIT},   {0x100, 0},             {0x800, 0},         {0x1000, 0},   // GROWSDOWN DENYWRITE EXECUTABLE
    {0x2000, 0},         {0x4000, 0},            {0x8000, MAP_PREFAULT_READ},       // LOCKED NORESERVE POPULATE
    {0x10000, 0},        {0x20000, 0},           {0x40000, 0},       {0x80000, 0},  // NONBLOCK STACK HUGETLB SYNC
    {0x100000, MAP_FIXED | MAP_EXCL},                                               // FIXED_NOREPLACE
};
// The library's own private anonymous mappings (made through sh_mmap), with
// their protection, kept up to date through munmap and mprotect. With it,
// MADV_DONTNEED on them needs no memory-map lookup from the kernel (which
// counts the resident pages of the whole process each time, under its lock).
struct Region {
  uintptr_t hi;
  int prot;
};
std::mutex g_reg_mu;
std::map<uintptr_t, Region> g_regions;  // lo -> region
uintptr_t page_up(uintptr_t v) {
  const uintptr_t pg = static_cast<uintptr_t>(getpagesize());
  return (v + pg - 1) & ~(pg - 1);
}
void regions_cut(uintptr_t lo, uintptr_t hi) {  // (g_reg_mu held)
  auto it = g_regions.lower_bound(lo);
  if (it != g_regions.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) it = prev;
  }
  while (it != g_regions.end() && it->first < hi) {
    const uintptr_t a = it->first, b = it->second.hi;
    const int pr = it->second.prot;
    it = g_regions.erase(it);
    if (a < lo) g_regions[a] = {lo, pr};
    if (b > hi) {
      g_regions[hi] = {b, pr};
      break;
    }
  }
}
void regions_add(uintptr_t lo, uintptr_t hi, int prot) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  regions_cut(lo, hi);
  g_regions[lo] = {hi, prot};
}
void regions_remove(uintptr_t lo, uintptr_t hi) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  regions_cut(lo, hi);
}
void regions_protect(uintptr_t lo, uintptr_t hi, int prot) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  std::vector<std::pair<uintptr_t, uintptr_t>> inside;
  auto it = g_regions.lower_bound(lo);
  if (it != g_regions.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) it = prev;
  }
  for (; it != g_regions.end() && it->first < hi; ++it)
    inside.push_back({std::max(it->first, lo), std::min(it->second.hi, hi)});
  regions_cut(lo, hi);
  for (const auto& r : inside) g_regions[r.first] = {r.second, prot};
}
// The tracked pieces of [lo, hi); true if they cover all of it.
bool regions_cover(uintptr_t lo, uintptr_t hi, std::vector<std::pair<uintptr_t, Region>>& out) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  out.clear();
  auto it = g_regions.lower_bound(lo);
  if (it != g_regions.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) it = prev;
  }
  uintptr_t at = lo;
  for (; it != g_regions.end() && it->first < hi; ++it) {
    if (it->first > at) return false;  // a gap
    const uintptr_t b = std::min(it->second.hi, hi);
    out.push_back({at, {b, it->second.prot}});
    at = b;
  }
  return at >= hi;
}

void* sh_mmap(void* a, size_t n, int prot, int f, int fd, off_t off) {
  unsigned lf = static_cast<unsigned>(f);
  if (lf & 0x40000) lf &= ~0xfc000000u;  // MAP_HUGE_* size bits
  if ((lf & 3) == 3) lf &= ~2u;          // MAP_SHARED_VALIDATE
  const int b = static_cast<int>(bits_to_bsd(lf, kMmap, "mmap flags"));
  if (b & MAP_ANON) fd = -1;
  void* r = mmap(a, n, prot & 7, b, fd, off);
  if (r != MAP_FAILED) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(r), hi = page_up(lo + n);
    if ((b & MAP_ANON) && (b & MAP_PRIVATE)) regions_add(lo, hi, prot & 7);
    else regions_remove(lo, hi);
    // Code the library makes for itself: its direct syscalls are Linux's too (bsdsys.hpp).
    if ((prot & PROT_EXEC) && (b & MAP_ANON)) bsdsys_add_range(lo, hi);
    else bsdsys_remove_range(lo, hi);
  }
  return r;
}
int sh_munmap(void* a, size_t n) {
  const int r = munmap(a, n);
  if (r == 0) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(a), hi = page_up(lo + n);
    regions_remove(lo, hi);
    bsdsys_remove_range(lo, hi);
  }
  return r;
}
int sh_mprotect(void* a, size_t n, int prot) {
  const int r = mprotect(a, n, prot & 7);
  if (r == 0) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(a), hi = page_up(lo + n);
    regions_protect(lo, hi, prot & 7);
    if (prot & PROT_EXEC) bsdsys_add_range(lo, hi);
  }
  return r;
}

// The process's mappings over [lo, hi), from the kernel (KERN_PROC_VMMAP).
struct Seg {
  uintptr_t lo, hi;
  int prot;
  bool private_anon;
};
bool vm_segments(uintptr_t lo, uintptr_t hi, std::vector<Seg>& out) {
  count(g_n_vmmap);
  static std::mutex mu;
  static std::vector<char> buf;
  std::lock_guard<std::mutex> l(mu);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, getpid()};
  for (int tries = 0; tries < 4; ++tries) {
    size_t len = 0;
    if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0) return false;
    len += len / 2 + 4096;
    if (buf.size() < len) buf.resize(len);
    len = buf.size();
    if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) != 0) {
      if (errno == ENOMEM) continue;
      return false;
    }
    out.clear();
    for (size_t p = 0; p + sizeof(int) <= len;) {
      const auto* kv = reinterpret_cast<const kinfo_vmentry*>(buf.data() + p);
      if (kv->kve_structsize <= 0) break;
      p += static_cast<size_t>(kv->kve_structsize);
      const uintptr_t s = kv->kve_start, e = kv->kve_end;
      if (e <= lo || s >= hi) continue;
      int prot = 0;
      if (kv->kve_protection & KVME_PROT_READ) prot |= PROT_READ;
      if (kv->kve_protection & KVME_PROT_WRITE) prot |= PROT_WRITE;
      if (kv->kve_protection & KVME_PROT_EXEC) prot |= PROT_EXEC;
      // Anonymous memory (no object yet, or a swap-backed one) that is not
      // System V / POSIX shared memory (memfd included). FreeBSD sets
      // KVME_FLAG_COW only on private file mappings, so it cannot tell here.
      const bool anon = kv->kve_type == KVME_TYPE_SWAP || kv->kve_type == KVME_TYPE_DEFAULT ||
                        kv->kve_type == KVME_TYPE_NONE;
      const bool shm = kv->kve_flags & (KVME_FLAG_SYSVSHM | KVME_FLAG_POSIXSHM);
      out.push_back({std::max(s, lo), std::min(e, hi), prot, anon && !shm});
    }
    return true;
  }
  return false;
}

// Linux's MADV_DONTNEED on private anonymous memory drops the pages, and the
// next touch reads zeros; memory allocators count on that. FreeBSD's may keep
// the old contents, so those ranges get fresh zero pages instead (same
// protection). Everything else (files, shared memory) gets FreeBSD's DONTNEED.
// (FreeBSD cannot tell MAP_SHARED|MAP_ANON memory from private here; it would
// only matter for memory shared with a forked child.)
int madv_dontneed(void* a, size_t n) {
  count(g_n_dontneed);
  const uintptr_t lo = reinterpret_cast<uintptr_t>(a), hi = lo + n;
  {
    // Memory the library mapped itself: fresh zero pages, same protection.
    std::vector<std::pair<uintptr_t, Region>> pieces;
    if (regions_cover(lo, page_up(hi), pieces)) {
      for (const auto& pc : pieces)
        if (mmap(reinterpret_cast<void*>(pc.first), pc.second.hi - pc.first, pc.second.prot,
                 MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED)
          return -1;
      return 0;
    }
  }
  std::vector<Seg> segs;
  if (!vm_segments(lo, hi, segs)) return madvise(a, n, MADV_DONTNEED);
  for (const Seg& s : segs) {
    void* p = reinterpret_cast<void*>(s.lo);
    const size_t len = s.hi - s.lo;
    if (s.private_anon) {
      if (mmap(p, len, s.prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == MAP_FAILED) return -1;
    } else if (madvise(p, len, MADV_DONTNEED) != 0) {
      return -1;
    }
  }
  return 0;
}
int sh_madvise(void* a, size_t n, int adv) {
  switch (adv) {
    case 0: return madvise(a, n, MADV_NORMAL);
    case 1: return madvise(a, n, MADV_RANDOM);
    case 2: return madvise(a, n, MADV_SEQUENTIAL);
    case 3: return madvise(a, n, MADV_WILLNEED);
    case 4: return madv_dontneed(a, n);
    case 8: return madvise(a, n, MADV_FREE);
    case 9: return madv_dontneed(a, n);                    // REMOVE
    case 10: return minherit(a, n, INHERIT_NONE);          // DONTFORK
    case 11: return minherit(a, n, INHERIT_COPY);          // DOFORK
    case 16: return madvise(a, n, MADV_NOCORE);            // DONTDUMP
    case 17: return madvise(a, n, MADV_CORE);              // DODUMP
    case 18: return minherit(a, n, INHERIT_ZERO);          // WIPEONFORK
    case 19: return minherit(a, n, INHERIT_COPY);          // KEEPONFORK
    case 22: case 23: return madvise(a, n, MADV_WILLNEED); // POPULATE_READ/WRITE
    case 12: case 13: case 14: case 15: case 20: case 21:  // MERGEABLE, HUGEPAGE, COLD, PAGEOUT: hints
      return 0;
  }
  note_once("madvise advice %d: not on FreeBSD", adv);
  return errno = EINVAL, -1;
}
int sh_posix_madvise(void* a, size_t n, int adv) {
  if (adv == 4) return 0;  // POSIX_MADV_DONTNEED: a hint (it never discards data)
  return sh_madvise(a, n, adv) == 0 ? 0 : lx_errno(errno);
}
int sh_msync(void* a, size_t n, int f) {
  static constexpr Bit k[] = {{1, MS_ASYNC}, {2, MS_INVALIDATE}, {4, MS_SYNC}};
  return msync(a, n, static_cast<int>(bits_to_bsd(static_cast<unsigned>(f), k, "msync flags")));
}
int sh_mlockall(int f) { return mlockall(f & (MCL_CURRENT | MCL_FUTURE)); }
int sh_mlock2(const void* a, size_t n, int) { return mlock(a, n); }

// mremap, for private anonymous memory: grow in place when the pages after
// it are free, else (MREMAP_MAYMOVE) map a new block and copy.
void* sh_mremap(void* old, size_t olds, size_t news, int f, ...) {
  const size_t pg = static_cast<size_t>(getpagesize());
  olds = (olds + pg - 1) & ~(pg - 1);
  news = (news + pg - 1) & ~(pg - 1);
  if (f & ~1) {
    note_once("mremap flags %#x: only MREMAP_MAYMOVE is supported", f);
    return errno = EINVAL, MAP_FAILED;
  }
  char* o = static_cast<char*>(old);
  if (news <= olds) {
    if (news < olds) {
      munmap(o + news, olds - news);
      regions_remove(reinterpret_cast<uintptr_t>(o) + news, reinterpret_cast<uintptr_t>(o) + olds);
    }
    return old;
  }
  std::vector<Seg> segs;
  const uintptr_t lo = reinterpret_cast<uintptr_t>(old);
  if (!vm_segments(lo, lo + olds, segs) || segs.empty()) return errno = EFAULT, MAP_FAILED;
  const int prot = segs[0].prot;
  for (const Seg& s : segs)
    if (!s.private_anon || s.prot != prot) {
      note_once("mremap: only private anonymous memory can be resized here");
      return errno = ENOMEM, MAP_FAILED;
    }
  if (mmap(o + olds, news - olds, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0) != MAP_FAILED) {
    regions_add(reinterpret_cast<uintptr_t>(o), reinterpret_cast<uintptr_t>(o) + news, prot);
    return old;
  }
  if (!(f & 1)) return errno = ENOMEM, MAP_FAILED;
  void* n = mmap(nullptr, news, prot | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (n == MAP_FAILED) return MAP_FAILED;
  if (!(prot & PROT_READ)) mprotect(old, olds, prot | PROT_READ);
  memcpy(n, old, olds);
  munmap(old, olds);
  if (prot != (prot | PROT_READ | PROT_WRITE)) mprotect(n, news, prot);
  regions_remove(reinterpret_cast<uintptr_t>(old), reinterpret_cast<uintptr_t>(old) + olds);
  regions_add(reinterpret_cast<uintptr_t>(n), reinterpret_cast<uintptr_t>(n) + news, prot);
  return n;
}

// mallinfo from jemalloc's own statistics.
struct LxMallinfo {
  size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
LxMallinfo sh_mallinfo() {
  LxMallinfo m{};
  uint64_t epoch = 1;
  size_t sz = sizeof epoch;
  mallctl("epoch", &epoch, &sz, &epoch, sz);
  size_t v = 0;
  sz = sizeof v;
  if (mallctl("stats.allocated", &v, &sz, nullptr, 0) == 0) m.uordblks = v;
  sz = sizeof v;
  if (mallctl("stats.mapped", &v, &sz, nullptr, 0) == 0) m.arena = v;
  if (m.arena > m.uordblks) m.fordblks = m.arena - m.uordblks;
  return m;
}
void* sh_memalign(size_t al, size_t n) {
  if (al < sizeof(void*)) al = sizeof(void*);
  void* p = nullptr;
  const int r = posix_memalign(&p, al, n);
  if (r) return errno = r, nullptr;
  return p;
}
void* sh_pvalloc(size_t n) {
  const size_t pg = static_cast<size_t>(getpagesize());
  return sh_memalign(pg, (n + pg - 1) & ~(pg - 1));
}

// ===========================================================================
// Signals
// ===========================================================================

constexpr int kLxSig[32] = {0,       SIGHUP,  SIGINT,  SIGQUIT,   SIGILL,  SIGTRAP, SIGABRT, SIGBUS,
                            SIGFPE,  SIGKILL, SIGUSR1, SIGSEGV,   SIGUSR2, SIGPIPE, SIGALRM, SIGTERM,
                            0,       SIGCHLD, SIGCONT, SIGSTOP,   SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
                            SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO,  0,       SIGSYS};
// (16 SIGSTKFLT and 30 SIGPWR do not exist on FreeBSD.)
constexpr int kMaxSig = _SIG_MAXSIG;
int g_b2l[kMaxSig + 1];

int bsd_sig(int lx) {
  if (lx > 0 && lx < 32) return kLxSig[lx];
  if (lx >= 32 && lx <= 64 && SIGRTMIN + (lx - 32) <= SIGRTMAX) return SIGRTMIN + (lx - 32);
  return 0;
}
int lx_sig(int b) { return b > 0 && b <= kMaxSig ? g_b2l[b] : 0; }
void init_signal_maps() {
  for (int l = 1; l <= 64; ++l)
    if (const int b = bsd_sig(l)) g_b2l[b] = l;
}

// bionic's sigset_t: 64 bits, bit (n-1) for Linux signal n.
using LxSigset = uint64_t;
void set_to_bsd(LxSigset lx, sigset_t* out) {
  sigemptyset(out);
  for (int s = 1; s <= 64; ++s)
    if (lx & (1ULL << (s - 1)))
      if (const int b = bsd_sig(s)) sigaddset(out, b);
}
LxSigset set_to_lx(const sigset_t* in) {
  LxSigset out = 0;
  for (int b = 1; b <= kMaxSig; ++b)
    if (sigismember(in, b) == 1)
      if (const int l = lx_sig(b)) out |= 1ULL << (l - 1);
  return out;
}

int sh_sigaddset(LxSigset* s, int sig) {
  if (!s || sig < 1 || sig > 64) return errno = EINVAL, -1;
  *s |= 1ULL << (sig - 1);
  return 0;
}
int sh_sigdelset(LxSigset* s, int sig) {
  if (!s || sig < 1 || sig > 64) return errno = EINVAL, -1;
  *s &= ~(1ULL << (sig - 1));
  return 0;
}
int sh_sigismember(const LxSigset* s, int sig) {
  if (!s || sig < 1 || sig > 64) return errno = EINVAL, -1;
  return (*s >> (sig - 1)) & 1;
}
int sh_sigemptyset(LxSigset* s) {
  if (!s) return errno = EINVAL, -1;
  return *s = 0, 0;
}
int sh_sigfillset(LxSigset* s) {
  if (!s) return errno = EINVAL, -1;
  return *s = ~0ULL, 0;
}
int sh_sigisemptyset(const LxSigset* s) { return *s == 0; }
int sh_sigorset(LxSigset* d, const LxSigset* a, const LxSigset* b) { return *d = *a | *b, 0; }
int sh_sigandset(LxSigset* d, const LxSigset* a, const LxSigset* b) { return *d = *a & *b, 0; }

// struct sigaction as bionic LP64 lays it out (as in shims.cpp).
struct LxSigaction {
  int sa_flags;
  void* handler;
  LxSigset sa_mask;
  void (*sa_restorer)();
};
constexpr unsigned kLxSaSiginfo = 4, kLxSaRestorer = 0x04000000;
constexpr Bit kSaFlags[] = {
    {1, SA_NOCLDSTOP},          {2, SA_NOCLDWAIT},       {0x08000000, SA_ONSTACK},
    {0x10000000, SA_RESTART},   {0x40000000, SA_NODEFER}, {0x80000000, SA_RESETHAND},
};

// siginfo_t and ucontext_t as Linux x86_64 lays them out.
struct LxSiginfo {
  int si_signo, si_errno, si_code, pad;
  union {
    struct {
      int pid;
      uint32_t uid;
      uint64_t value;  // si_value (sigqueue, timers)
    } kill;
    struct {
      int pid;
      uint32_t uid;
      int status;
    } chld;
    struct {
      void* addr;
    } fault;
    char raw[112];
  } u;
};
static_assert(sizeof(LxSiginfo) == 128, "Linux siginfo_t is 128 bytes");

struct LxStack {
  void* ss_sp;
  int ss_flags;
  size_t ss_size;
};
struct LxUcontext {
  uint64_t uc_flags;
  LxUcontext* uc_link;
  LxStack uc_stack;
  uint64_t gregs[23];
  void* fpregs;
  uint64_t reserved1[8];
  LxSigset uc_sigmask;
  alignas(16) uint8_t fpregs_mem[512];
};
static_assert(offsetof(LxUcontext, gregs) == 40 && offsetof(LxUcontext, uc_sigmask) == 296 &&
                  offsetof(LxUcontext, fpregs_mem) == 304,
              "bionic x86_64 ucontext_t layout");
enum { R8, R9, R10, R11, R12, R13, R14, R15, RDI, RSI, RBP, RBX, RDX, RAX, RCX, RSP, RIP, EFL, CSGSFS, ERR, TRAPNO, OLDMASK, CR2 };

int ss_flags_to_lx(int b) { return (b & SS_ONSTACK ? 1 : 0) | (b & SS_DISABLE ? 2 : 0); }
int ss_flags_to_bsd(int l) { return (l & 1 ? SS_ONSTACK : 0) | (l & 2 ? SS_DISABLE : 0); }

int code_to_lx(int bsig, int c) {
  switch (c) {
    case SI_USER: return 0;
    case SI_QUEUE: return -1;
    case SI_TIMER: return -2;
    case SI_MESGQ: return -3;
    case SI_ASYNCIO: return -4;
    case SI_LWP: return -6;  // SI_TKILL
    case SI_KERNEL: return 0x80;
  }
  if (bsig == SIGFPE) {  // the integer codes are swapped
    if (c == FPE_INTOVF) return 2;
    if (c == FPE_INTDIV) return 1;
    if (c > 8) return 7;
  }
  return c;
}
// FreeBSD's trap numbers -> x86 exception vectors (what Linux reports).
uint64_t trap_to_lx(uint32_t t) {
  switch (t) {
    case T_DIVIDE: return 0;
    case T_TRCTRAP: return 1;
    case T_BPTFLT: return 3;
    case T_PRIVINFLT: return 6;
    case T_PROTFLT: return 13;
    case T_PAGEFLT: return 14;
    case T_ARITHTRAP: return 16;
    case T_ALIGNFLT: return 17;
    case T_XMMFLT: return 19;
  }
  return 0;
}

void info_to_lx(int bsig, const siginfo_t* bi, LxSiginfo* li) {
  memset(li, 0, sizeof *li);
  li->si_signo = lx_sig(bsig);
  if (!bi) return;
  li->si_errno = lx_errno(bi->si_errno);
  li->si_code = code_to_lx(bsig, bi->si_code);
  switch (bsig) {
    case SIGSEGV: case SIGBUS: case SIGILL: case SIGFPE: case SIGTRAP:
      li->u.fault.addr = bi->si_addr;
      break;
    case SIGCHLD:
      li->u.chld.pid = bi->si_pid;
      li->u.chld.uid = bi->si_uid;
      li->u.chld.status = bi->si_status;
      break;
    default:
      if (bi->si_code == SI_TIMER) {
        li->u.kill.pid = bi->si_timerid;
        li->u.kill.uid = static_cast<uint32_t>(bi->si_overrun);
      } else {
        li->u.kill.pid = bi->si_pid;
        li->u.kill.uid = bi->si_uid;
      }
      li->u.kill.value = reinterpret_cast<uint64_t>(bi->si_value.sival_ptr);
  }
}

void ctx_to_lx(const ucontext_t* uc, LxUcontext* lu) {
  memset(lu, 0, offsetof(LxUcontext, fpregs_mem));
  const mcontext_t& m = uc->uc_mcontext;
  lu->uc_stack.ss_sp = uc->uc_stack.ss_sp;
  lu->uc_stack.ss_size = uc->uc_stack.ss_size;
  lu->uc_stack.ss_flags = ss_flags_to_lx(uc->uc_stack.ss_flags);
  uint64_t* g = lu->gregs;
  g[R8] = m.mc_r8, g[R9] = m.mc_r9, g[R10] = m.mc_r10, g[R11] = m.mc_r11;
  g[R12] = m.mc_r12, g[R13] = m.mc_r13, g[R14] = m.mc_r14, g[R15] = m.mc_r15;
  g[RDI] = m.mc_rdi, g[RSI] = m.mc_rsi, g[RBP] = m.mc_rbp, g[RBX] = m.mc_rbx;
  g[RDX] = m.mc_rdx, g[RAX] = m.mc_rax, g[RCX] = m.mc_rcx, g[RSP] = m.mc_rsp;
  g[RIP] = m.mc_rip, g[EFL] = m.mc_rflags;
  g[CSGSFS] = (m.mc_cs & 0xffff) | (uint64_t{m.mc_gs} << 16) | (uint64_t{m.mc_fs} << 32);
  g[ERR] = m.mc_err;
  g[TRAPNO] = trap_to_lx(m.mc_trapno);
  g[CR2] = m.mc_addr;
  if (m.mc_fpformat == _MC_FPFMT_XMM) {
    memcpy(lu->fpregs_mem, m.mc_fpstate, sizeof lu->fpregs_mem);
    lu->fpregs = lu->fpregs_mem;
  }
  lu->uc_sigmask = set_to_lx(&uc->uc_sigmask);
}
// What the handler changed goes back (a handler may move RIP on purpose).
void ctx_from_lx(const LxUcontext* lu, ucontext_t* uc) {
  mcontext_t& m = uc->uc_mcontext;
  const uint64_t* g = lu->gregs;
  m.mc_r8 = g[R8], m.mc_r9 = g[R9], m.mc_r10 = g[R10], m.mc_r11 = g[R11];
  m.mc_r12 = g[R12], m.mc_r13 = g[R13], m.mc_r14 = g[R14], m.mc_r15 = g[R15];
  m.mc_rdi = g[RDI], m.mc_rsi = g[RSI], m.mc_rbp = g[RBP], m.mc_rbx = g[RBX];
  m.mc_rdx = g[RDX], m.mc_rax = g[RAX], m.mc_rcx = g[RCX], m.mc_rsp = g[RSP];
  m.mc_rip = g[RIP];
  // Only the flags a program may change (CF PF AF ZF SF TF DF OF, AC).
  constexpr uint64_t kUserFlags = 0x40dd5;
  m.mc_rflags = (m.mc_rflags & ~kUserFlags) | (g[EFL] & kUserFlags);
  if (m.mc_fpformat == _MC_FPFMT_XMM && lu->fpregs == lu->fpregs_mem)
    memcpy(m.mc_fpstate, lu->fpregs_mem, sizeof lu->fpregs_mem);
  sigset_t keep = uc->uc_sigmask;
  set_to_bsd(lu->uc_sigmask, &uc->uc_sigmask);
  for (int b = 1; b <= kMaxSig; ++b)  // FreeBSD's own signals (no Linux number) stay as they were
    if (!lx_sig(b) && sigismember(&keep, b) == 1) sigaddset(&uc->uc_sigmask, b);
}

// The library's handlers, by FreeBSD signal number. FreeBSD calls tramp(),
// which calls the library's handler with Linux numbers and layouts.
struct LibHandler {
  std::atomic<void*> fn{nullptr};
  std::atomic<int> flags{0};
  std::atomic<LxSigset> mask{0};
};
LibHandler g_lib[kMaxSig + 1];
// The handler that was there before the library's (tonic's own, usually),
// handed to the library as rev_tramp so it can chain to it.
struct sigaction g_prev[kMaxSig + 1];
bool g_have_prev[kMaxSig + 1];
std::mutex g_sig_mu;
thread_local const siginfo_t* t_info = nullptr;
thread_local void* t_uctx = nullptr;

void tramp(int bsig, siginfo_t* bi, void* buc) {
  const int saved_errno = errno, saved_lib_errno = t_errno;
  void* fn = g_lib[bsig].fn.load(std::memory_order_acquire);
  const int lflags = g_lib[bsig].flags.load(std::memory_order_relaxed);
  const siginfo_t* prev_info = t_info;
  void* prev_uctx = t_uctx;
  t_info = bi;
  t_uctx = buc;
  if (fn && (lflags & kLxSaSiginfo)) {
    LxSiginfo li;
    info_to_lx(bsig, bi, &li);
    LxUcontext lu;
    auto* uc = static_cast<ucontext_t*>(buc);
    ctx_to_lx(uc, &lu);
    reinterpret_cast<void (*)(int, void*, void*)>(fn)(lx_sig(bsig), &li, &lu);
    ctx_from_lx(&lu, uc);
  } else if (fn) {
    reinterpret_cast<void (*)(int)>(fn)(lx_sig(bsig));
  }
  t_info = prev_info;
  t_uctx = prev_uctx;
  errno = saved_errno;
  t_errno = saved_lib_errno;
}

// What the library gets as "the previous handler" when that was a FreeBSD
// one. Calling it runs the original with FreeBSD's own siginfo and context
// (those of the signal being handled, when called from the library's handler).
void rev_tramp(int lsig, void*, void*) {
  const int bsig = bsd_sig(lsig);
  if (!bsig || !g_have_prev[bsig]) return;
  const struct sigaction p = g_prev[bsig];
  if (p.sa_flags & SA_SIGINFO) {
    if (t_info && t_info->si_signo == bsig) {
      p.sa_sigaction(bsig, const_cast<siginfo_t*>(t_info), t_uctx);
    } else {
      siginfo_t si{};
      si.si_signo = bsig;
      si.si_code = SI_USER;
      si.si_pid = getpid();
      ucontext_t uc{};
      getcontext(&uc);
      p.sa_sigaction(bsig, &si, &uc);
    }
  } else if (p.sa_handler == SIG_DFL) {
    struct sigaction d{};
    d.sa_handler = SIG_DFL;
    sigaction(bsig, &d, nullptr);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, bsig);
    sigprocmask(SIG_UNBLOCK, &s, nullptr);
    raise(bsig);
  } else if (p.sa_handler != SIG_IGN) {
    p.sa_handler(bsig);
  }
}

int sh_sigaction(int lsig, const LxSigaction* act, LxSigaction* old) {
  const int bsig = bsd_sig(lsig);
  if (!bsig) return errno = EINVAL, -1;
  std::lock_guard<std::mutex> l(g_sig_mu);
  struct sigaction cur{};
  if (sigaction(bsig, nullptr, &cur) != 0) return -1;
  LxSigaction was{};
  const bool ours = (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == tramp;
  if (ours) {
    was.handler = g_lib[bsig].fn.load();
    was.sa_flags = g_lib[bsig].flags.load();
    was.sa_mask = g_lib[bsig].mask.load();
  } else {
    void* h = (cur.sa_flags & SA_SIGINFO) ? reinterpret_cast<void*>(cur.sa_sigaction)
                                          : reinterpret_cast<void*>(cur.sa_handler);
    was.sa_flags = static_cast<int>(bits_to_lx(static_cast<unsigned>(cur.sa_flags), kSaFlags));
    was.sa_mask = set_to_lx(&cur.sa_mask);
    if (h == reinterpret_cast<void*>(SIG_DFL) || h == reinterpret_cast<void*>(SIG_IGN)) {
      was.handler = h;
    } else {
      g_prev[bsig] = cur;
      g_have_prev[bsig] = true;
      was.handler = reinterpret_cast<void*>(&rev_tramp);
      was.sa_flags |= static_cast<int>(kLxSaSiginfo);
    }
  }
  if (act) {
    struct sigaction n{};
    set_to_bsd(act->sa_mask, &n.sa_mask);
    const unsigned lf = static_cast<unsigned>(act->sa_flags) & ~(kLxSaSiginfo | kLxSaRestorer);
    const int bf = static_cast<int>(bits_to_bsd(lf, kSaFlags, "sigaction flags"));
    void* h = act->handler;
    if (h == reinterpret_cast<void*>(SIG_DFL) || h == reinterpret_cast<void*>(SIG_IGN)) {
      n.sa_handler = reinterpret_cast<void (*)(int)>(h);
      n.sa_flags = bf;
    } else if (h == reinterpret_cast<void*>(&rev_tramp)) {
      if (g_have_prev[bsig]) n = g_prev[bsig];
      else n.sa_handler = SIG_DFL;
    } else {
      g_lib[bsig].flags.store(act->sa_flags);
      g_lib[bsig].mask.store(act->sa_mask);
      g_lib[bsig].fn.store(h, std::memory_order_release);
      n.sa_sigaction = tramp;
      n.sa_flags = bf | SA_SIGINFO;
      fprintf(stderr, "[sigaction] library installs handler for signal %d (%s)\n", lsig, strsignal(bsig));
    }
    if (sigaction(bsig, &n, nullptr) != 0) return -1;
  }
  if (old) *old = was;
  return 0;
}

using SigHandler = void (*)(int);
SigHandler signal_with(int lsig, SigHandler h, unsigned lxflags) {
  LxSigaction a{}, o{};
  a.handler = reinterpret_cast<void*>(h);
  a.sa_flags = static_cast<int>(lxflags);
  if (sh_sigaction(lsig, &a, &o) != 0) return SIG_ERR;
  return reinterpret_cast<SigHandler>(o.handler);
}
SigHandler sh_signal(int s, SigHandler h) { return signal_with(s, h, 0x10000000); }       // SA_RESTART (bionic: BSD semantics)
SigHandler sh_sysv_signal(int s, SigHandler h) { return signal_with(s, h, 0xC0000000); }  // RESETHAND | NODEFER

int how_to_bsd(int how) { return how == 0 ? SIG_BLOCK : how == 1 ? SIG_UNBLOCK : how == 2 ? SIG_SETMASK : -1; }
// Changes the calling thread's mask. Returns 0 or a FreeBSD errno.
int mask_change(int how, const LxSigset* set, LxSigset* old) {
  sigset_t s, o, *sp = nullptr;
  int bhow = SIG_BLOCK;
  if (set) {
    bhow = how_to_bsd(how);
    if (bhow < 0) return EINVAL;
    set_to_bsd(*set, &s);
    if (bhow == SIG_SETMASK) {
      sigset_t cur;
      pthread_sigmask(SIG_BLOCK, nullptr, &cur);
      for (int b = 1; b <= kMaxSig; ++b)
        if (!lx_sig(b) && sigismember(&cur, b) == 1) sigaddset(&s, b);
    }
    sp = &s;
  }
  const int r = pthread_sigmask(bhow, sp, old ? &o : nullptr);
  if (r == 0 && old) *old = set_to_lx(&o);
  return r;
}
int sh_pthread_sigmask(int how, const LxSigset* set, LxSigset* old) { return rc(mask_change(how, set, old)); }
int sh_sigprocmask(int how, const LxSigset* set, LxSigset* old) {
  const int r = mask_change(how, set, old);
  return r ? (errno = r, -1) : 0;
}
int sh_sigsuspend(const LxSigset* set) {
  sigset_t s;
  set_to_bsd(*set, &s);
  return sigsuspend(&s);
}
int sh_sigpending(LxSigset* set) {
  sigset_t s;
  const int r = sigpending(&s);
  if (r == 0) *set = set_to_lx(&s);
  return r;
}
int sh_sigwait(const LxSigset* set, int* sig) {
  sigset_t s;
  set_to_bsd(*set, &s);
  int b = 0;
  const int r = sigwait(&s, &b);
  if (r == 0 && sig) *sig = lx_sig(b);
  return rc(r);
}
int sh_sigtimedwait(const LxSigset* set, LxSiginfo* info, const timespec* ts) {
  sigset_t s;
  set_to_bsd(*set, &s);
  siginfo_t bi{};
  const int b = ts ? sigtimedwait(&s, &bi, ts) : sigwaitinfo(&s, &bi);
  if (b < 0) return -1;
  if (info) info_to_lx(b, &bi, info);
  return lx_sig(b);
}
int sh_sigwaitinfo(const LxSigset* set, LxSiginfo* info) { return sh_sigtimedwait(set, info, nullptr); }

int sig_or_einval(int lsig) {
  if (lsig == 0) return 0;
  const int b = bsd_sig(lsig);
  return b ? b : -1;
}
int sh_kill(pid_t p, int s) {
  const int b = sig_or_einval(s);
  return b < 0 ? (errno = EINVAL, -1) : kill(p, b);
}
int sh_killpg(pid_t p, int s) {
  const int b = sig_or_einval(s);
  return b < 0 ? (errno = EINVAL, -1) : killpg(p, b);
}
int sh_raise(int s) {
  const int b = sig_or_einval(s);
  return b < 0 ? (errno = EINVAL, -1) : raise(b);
}
int sh_pthread_kill(pthread_t t, int s) {
  const int b = sig_or_einval(s);
  return b < 0 ? 22 : rc(pthread_kill(t, b));
}
int sh_tgkill(pid_t tgid, pid_t tid, int s) {
  const int b = sig_or_einval(s);
  if (b < 0) return errno = EINVAL, -1;
  return tgid == getpid() ? thr_kill(tid, b) : thr_kill2(tgid, tid, b);
}
int sh_tkill(pid_t tid, int s) {
  const int b = sig_or_einval(s);
  return b < 0 ? (errno = EINVAL, -1) : thr_kill(tid, b);
}
int sh_sigqueue(pid_t p, int s, const sigval v) {
  const int b = sig_or_einval(s);
  return b < 0 ? (errno = EINVAL, -1) : sigqueue(p, b, v);
}
char* sh_strsignal(int s) {
  if (const int b = bsd_sig(s)) return strsignal(b);
  static thread_local char buf[32];
  snprintf(buf, sizeof buf, "Unknown signal %d", s);
  return buf;
}
void sh_psignal(int s, const char* m) {
  if (const int b = bsd_sig(s)) psignal(b, m);
  else fprintf(stderr, "%s%sUnknown signal %d\n", m ? m : "", m && *m ? ": " : "", s);
}
int sh_sigaltstack(const LxStack* ss, LxStack* old) {
  stack_t n{}, o{};
  if (ss) {
    n.ss_sp = ss->ss_sp;
    n.ss_size = ss->ss_size;
    n.ss_flags = ss_flags_to_bsd(ss->ss_flags);
  }
  const int r = sigaltstack(ss ? &n : nullptr, old ? &o : nullptr);
  if (r == 0 && old) {
    old->ss_sp = o.ss_sp;
    old->ss_size = o.ss_size;
    old->ss_flags = ss_flags_to_lx(o.ss_flags);
  }
  return r;
}
// glibc's numbers, as Linux tonic hands out (bsd_sig maps them onto FreeBSD's
// real-time range).
int sh_sigrtmin() { return 34; }
int sh_sigrtmax() { return 64; }

// ===========================================================================
// Threads
// ===========================================================================

// pthread_once_t is an int on bionic; FreeBSD's is 16 bytes and would be
// written past the end of the library's variables.
std::mutex g_once_mu;
std::condition_variable g_once_cv;
int sh_pthread_once(int* once, void (*fn)()) {
  auto* a = reinterpret_cast<std::atomic<int>*>(once);
  if (a->load(std::memory_order_acquire) == 2) return 0;
  int expect = 0;
  if (a->compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) {
    fn();
    {
      std::lock_guard<std::mutex> l(g_once_mu);
      a->store(2, std::memory_order_release);
    }
    g_once_cv.notify_all();
    return 0;
  }
  std::unique_lock<std::mutex> l(g_once_mu);
  g_once_cv.wait(l, [&] { return a->load(std::memory_order_acquire) == 2; });
  return 0;
}

// Mutex types: bionic NORMAL 0, RECURSIVE 1, ERRORCHECK 2.
int sh_mutexattr_settype(pthread_mutexattr_t* a, int t) {
  const int b = t == 0 ? PTHREAD_MUTEX_NORMAL : t == 1 ? PTHREAD_MUTEX_RECURSIVE : t == 2 ? PTHREAD_MUTEX_ERRORCHECK : -1;
  return b < 0 ? 22 : rc(pthread_mutexattr_settype(a, b));
}
int sh_mutexattr_gettype(const pthread_mutexattr_t* a, int* t) {
  int b = 0;
  const int r = pthread_mutexattr_gettype(a, &b);
  if (r == 0) *t = b == PTHREAD_MUTEX_RECURSIVE ? 1 : b == PTHREAD_MUTEX_ERRORCHECK ? 2 : 0;
  return rc(r);
}

// bionic's PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP and ..._ERRORCHECK_... are
// not zero (the type sits in the first word), and FreeBSD would take that word
// for a pointer. Such a mutex is set up properly the first time it is used.
constexpr uintptr_t kBionicRecursiveInit = 0x4000, kBionicErrorcheckInit = 0x8000;
void init_static_mutex(pthread_mutex_t* m, uintptr_t v) {
  static std::mutex mu;
  std::lock_guard<std::mutex> l(mu);
  auto* w = reinterpret_cast<std::atomic<uintptr_t>*>(m);
  if (w->load(std::memory_order_acquire) != v) return;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, v == kBionicRecursiveInit ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_t tmp;
  pthread_mutex_init(&tmp, &a);
  pthread_mutexattr_destroy(&a);
  w->store(reinterpret_cast<uintptr_t>(tmp), std::memory_order_release);
}
inline pthread_mutex_t* fix_mutex(pthread_mutex_t* m) {
  const uintptr_t v = reinterpret_cast<std::atomic<uintptr_t>*>(m)->load(std::memory_order_acquire);
  if (__builtin_expect(v == kBionicRecursiveInit || v == kBionicErrorcheckInit, 0)) init_static_mutex(m, v);
  return m;
}
int sh_mutex_lock(pthread_mutex_t* m) { return rc(pthread_mutex_lock(fix_mutex(m))); }
int sh_mutex_trylock(pthread_mutex_t* m) { return rc(pthread_mutex_trylock(fix_mutex(m))); }
int sh_mutex_unlock(pthread_mutex_t* m) { return rc(pthread_mutex_unlock(fix_mutex(m))); }
int sh_mutex_destroy(pthread_mutex_t* m) { return rc(pthread_mutex_destroy(fix_mutex(m))); }
int sh_mutex_timedlock(pthread_mutex_t* m, const timespec* t) {
  const int r = pthread_mutex_timedlock(fix_mutex(m), t);
  if (r == ETIMEDOUT) note_timeout(g_w_mutex, CLOCK_REALTIME, ts_ns(t));
  return rc(r);
}
int sh_mutex_clocklock(pthread_mutex_t* m, clockid_t c, const timespec* t) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return 22;
  const timespec rt = abs_to_realtime(b, t);
  const int r = pthread_mutex_timedlock(fix_mutex(m), &rt);
  if (r == ETIMEDOUT) note_timeout(g_w_mutex, b, ts_ns(t));
  return rc(r);
}
int sh_mutex_timedlock_monotonic(pthread_mutex_t* m, const timespec* t) { return sh_mutex_clocklock(m, 1, t); }
int sh_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { return rc(pthread_cond_wait(c, fix_mutex(m))); }
// (The clock a condition variable uses is set when it is made; the stats
// assume CLOCK_REALTIME for pthread_cond_timedwait, which is the default. A
// cond made with CLOCK_MONOTONIC shows here as hugely "late" or never late.)
int sh_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  const int r = pthread_cond_timedwait(c, fix_mutex(m), t);
  if (r == ETIMEDOUT && g_stats) {
    // Guess the clock from the deadline: a monotonic one is far smaller.
    const clockid_t k = ts_ns(t) < now_ns(CLOCK_REALTIME) / 2 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    note_timeout(g_w_cond, k, ts_ns(t));
  }
  return rc(r);
}
int sh_cond_clockwait(pthread_cond_t* c, pthread_mutex_t* m, clockid_t k, const timespec* t) {
  clockid_t b;
  if (!clock_to_bsd(k, &b)) return 22;
  const timespec rt = abs_to_realtime(b, t);
  const int r = pthread_cond_timedwait(c, fix_mutex(m), &rt);
  if (r == ETIMEDOUT) note_timeout(g_w_cond, b, ts_ns(t));
  return rc(r);
}
int sh_cond_timedwait_monotonic(pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) {
  return sh_cond_clockwait(c, m, 1, t);
}
int sh_cond_timedwait_relative(pthread_cond_t* c, pthread_mutex_t* m, const timespec* rel) {
  const timespec r = rel_to_realtime(rel);
  return rc(pthread_cond_timedwait(c, fix_mutex(m), &r));
}
int sh_condattr_setclock(pthread_condattr_t* a, clockid_t c) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return 22;
  return rc(pthread_condattr_setclock(a, b));
}
int sh_condattr_getclock(const pthread_condattr_t* a, clockid_t* c) {
  clockid_t b = CLOCK_REALTIME;
  const int r = pthread_condattr_getclock(a, &b);
  if (r == 0) *c = clock_to_lx(b);
  return rc(r);
}
int sh_rwlock_clockrdlock(pthread_rwlock_t* l, clockid_t c, const timespec* t) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return 22;
  const timespec r = abs_to_realtime(b, t);
  return rc(pthread_rwlock_timedrdlock(l, &r));
}
int sh_rwlock_clockwrlock(pthread_rwlock_t* l, clockid_t c, const timespec* t) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return 22;
  const timespec r = abs_to_realtime(b, t);
  return rc(pthread_rwlock_timedwrlock(l, &r));
}
int sh_rwlock_timedrdlock_monotonic(pthread_rwlock_t* l, const timespec* t) { return sh_rwlock_clockrdlock(l, 1, t); }
int sh_rwlock_timedwrlock_monotonic(pthread_rwlock_t* l, const timespec* t) { return sh_rwlock_clockwrlock(l, 1, t); }
int sh_rwlockattr_setkind(void*, int) { return 0; }  // bionic's writer preference: a hint
int sh_rwlockattr_getkind(const void*, int* k) { return *k = 0, 0; }

int sh_sem_clockwait(sem_t* s, clockid_t c, const timespec* t) {
  clockid_t b;
  if (!clock_to_bsd(c, &b)) return errno = EINVAL, -1;
  return sem_clockwait_np(s, b, TIMER_ABSTIME, t, nullptr);
}
int sh_sem_timedwait_monotonic(sem_t* s, const timespec* t) { return sh_sem_clockwait(s, 1, t); }

// pthread_barrierattr_t is an int on bionic; FreeBSD's is a pointer.
int sh_barrierattr_init(int* a) { return *a = 0, 0; }
int sh_barrierattr_destroy(int*) { return 0; }
int sh_barrierattr_setpshared(int* a, int v) { return *a = v, 0; }
int sh_barrierattr_getpshared(const int* a, int* v) { return *v = *a, 0; }
int sh_barrier_init(pthread_barrier_t* b, const int* a, unsigned n) {
  pthread_barrierattr_t h;
  pthread_barrierattr_init(&h);
  if (a && *a) pthread_barrierattr_setpshared(&h, PTHREAD_PROCESS_SHARED);
  const int r = pthread_barrier_init(b, &h, n);
  pthread_barrierattr_destroy(&h);
  return rc(r);
}

int policy_to_bsd(int p) { return p == 1 ? SCHED_FIFO : p == 2 ? SCHED_RR : SCHED_OTHER; }  // BATCH, IDLE: OTHER
int policy_to_lx(int p) { return p == SCHED_FIFO ? 1 : p == SCHED_RR ? 2 : 0; }
int sh_attr_setschedpolicy(pthread_attr_t* a, int p) { return rc(pthread_attr_setschedpolicy(a, policy_to_bsd(p))); }
int sh_attr_getschedpolicy(const pthread_attr_t* a, int* p) {
  int b = 0;
  const int r = pthread_attr_getschedpolicy(a, &b);
  if (r == 0) *p = policy_to_lx(b);
  return rc(r);
}
int sh_setschedparam(pthread_t t, int p, const sched_param* s) { return rc(pthread_setschedparam(t, policy_to_bsd(p), s)); }
int sh_getschedparam(pthread_t t, int* p, sched_param* s) {
  int b = 0;
  const int r = pthread_getschedparam(t, &b, s);
  if (r == 0) *p = policy_to_lx(b);
  return rc(r);
}
int sh_setschedprio(pthread_t t, int prio) {  // not in FreeBSD's libthr
  int pol = 0;
  sched_param sp{};
  int r = pthread_getschedparam(t, &pol, &sp);
  if (r == 0) {
    sp.sched_priority = prio;
    r = pthread_setschedparam(t, pol, &sp);
  }
  return rc(r);
}
int sh_sched_get_priority_max(int p) { return sched_get_priority_max(policy_to_bsd(p)); }
int sh_sched_get_priority_min(int p) { return sched_get_priority_min(policy_to_bsd(p)); }
int sh_sched_setscheduler(pid_t pid, int p, const sched_param* s) { return sched_setscheduler(pid, policy_to_bsd(p), s); }
int sh_sched_getscheduler(pid_t pid) {
  const int r = sched_getscheduler(pid);
  return r < 0 ? r : policy_to_lx(r);
}
// bionic: PTHREAD_SCOPE_SYSTEM 0, PROCESS 1; PTHREAD_EXPLICIT_SCHED 0, INHERIT 1.
int sh_attr_setscope(pthread_attr_t* a, int s) {
  return rc(pthread_attr_setscope(a, s == 1 ? PTHREAD_SCOPE_PROCESS : PTHREAD_SCOPE_SYSTEM));
}
int sh_attr_getscope(const pthread_attr_t* a, int* s) {
  int b = 0;
  const int r = pthread_attr_getscope(a, &b);
  if (r == 0) *s = b == PTHREAD_SCOPE_PROCESS ? 1 : 0;
  return rc(r);
}
int sh_attr_setinheritsched(pthread_attr_t* a, int s) {
  return rc(pthread_attr_setinheritsched(a, s == 1 ? PTHREAD_INHERIT_SCHED : PTHREAD_EXPLICIT_SCHED));
}
int sh_attr_getinheritsched(const pthread_attr_t* a, int* s) {
  int b = 0;
  const int r = pthread_attr_getinheritsched(a, &b);
  if (r == 0) *s = b == PTHREAD_INHERIT_SCHED ? 1 : 0;
  return rc(r);
}
int sh_getattr_np(pthread_t t, pthread_attr_t* a) {
  int r = pthread_attr_init(a);
  if (r == 0) r = pthread_attr_get_np(t, a);
  return rc(r);
}
pid_t sh_gettid() { return pthread_getthreadid_np(); }
pid_t sh_pthread_gettid_np(pthread_t t) {
  if (pthread_equal(t, pthread_self())) return pthread_getthreadid_np();
  note_once("pthread_gettid_np for another thread: FreeBSD can only tell a thread its own id");
  return -1;
}

// bionic's pthread_cleanup_push/pop (no cancellation on Android, so only
// pop(1) runs the routine).
struct LxCleanup {
  LxCleanup* prev;
  void (*routine)(void*);
  void* arg;
};
thread_local LxCleanup* t_cleanup = nullptr;
void sh_cleanup_push(LxCleanup* c, void (*r)(void*), void* a) {
  c->prev = t_cleanup;
  c->routine = r;
  c->arg = a;
  t_cleanup = c;
}
void sh_cleanup_pop(LxCleanup* c, int execute) {
  t_cleanup = c->prev;
  if (execute && c->routine) c->routine(c->arg);
}

int sh_sched_getaffinity(pid_t pid, size_t n, void* set) {
  cpuset_t s;
  CPU_ZERO(&s);
  const int which = pid == 0 ? CPU_WHICH_TID : pid == getpid() ? CPU_WHICH_PID : CPU_WHICH_TID;
  if (cpuset_getaffinity(CPU_LEVEL_WHICH, which, pid == 0 ? -1 : pid, sizeof s, &s) != 0) return -1;
  memset(set, 0, n);
  memcpy(set, &s, std::min(n, sizeof s));
  return 0;
}
int sh_sched_setaffinity(pid_t pid, size_t n, const void* set) {
  cpuset_t s;
  CPU_ZERO(&s);
  memcpy(&s, set, std::min(n, sizeof s));
  const int which = pid == 0 ? CPU_WHICH_TID : pid == getpid() ? CPU_WHICH_PID : CPU_WHICH_TID;
  return cpuset_setaffinity(CPU_LEVEL_WHICH, which, pid == 0 ? -1 : pid, sizeof s, &s);
}
int sh_sched_cpucount(size_t n, const void* set) {
  int c = 0;
  const auto* p = static_cast<const unsigned char*>(set);
  for (size_t i = 0; i < n; ++i) c += __builtin_popcount(p[i]);
  return c;
}
void* sh_sched_cpualloc(size_t count) { return calloc(1, ((count + 63) / 64) * 8); }
void sh_sched_cpufree(void* p) { free(p); }

int sh_setpriority(int which, id_t who, int prio) {
  if (which == PRIO_PROCESS && who != 0 && static_cast<pid_t>(who) != getpid()) {
    // On Linux a thread id here sets that one thread's nice value; FreeBSD has
    // no per-thread nice. Left as it is.
    note_once("setpriority on a thread: FreeBSD has no per-thread nice value; left unchanged");
    return 0;
  }
  return setpriority(which, who, prio);
}
int sh_getpriority(int which, id_t who) {
  if (which == PRIO_PROCESS && who != 0 && static_cast<pid_t>(who) != getpid()) who = 0;
  return getpriority(which, who);
}

// sched_yield: Roblox's idle workers yield in a tight loop, which spins at 100%
// on FreeBSD. A thread yielding back to back (cycle counter, gap g_yield_gap)
// naps instead, but only once idle for twice the measured nap cost (>= 2 ms).
thread_local unsigned t_yields = 0;
thread_local uint64_t t_last_yield = 0;
thread_local uint64_t t_idle_since = 0;
uint64_t g_yield_gap = 400000;      // (until measured: 200 us at 2 GHz)
uint64_t g_idle_before_nap = 4000000;  // (until measured: 2 ms at 2 GHz)
double g_nap_late_us = 0;
void measure_yield_gap() {
  timespec a{}, b{};
  clock_gettime(CLOCK_MONOTONIC, &a);
  const uint64_t c0 = __builtin_ia32_rdtsc();
  const timespec nap = {0, 10000000};  // 10 ms
  nanosleep(&nap, nullptr);
  const uint64_t c1 = __builtin_ia32_rdtsc();
  clock_gettime(CLOCK_MONOTONIC, &b);
  const double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
  if (!(ns > 0 && c1 > c0)) return;
  const double per_ns = (c1 - c0) / ns;
  g_cyc_per_ns = per_ns;
  g_yield_gap = static_cast<uint64_t>(per_ns * 200000.0);
  // How late a short nap comes back (the worst of a few).
  double late = 0;
  for (int i = 0; i < 8; ++i) {
    const timespec s = {0, 50000};
    const uint64_t t0 = __builtin_ia32_rdtsc();
    nanosleep(&s, nullptr);
    late = std::max(late, (__builtin_ia32_rdtsc() - t0) / per_ns - 50000.0);
  }
  g_nap_late_us = late / 1000.0;
  g_idle_before_nap = static_cast<uint64_t>(per_ns * std::max(2000000.0, 2.0 * late));
}
bool g_yield_plain = false;  // TONIC_YIELD=plain: FreeBSD's sched_yield as it is (for comparison)
int sh_sched_yield() {
  count(g_n_yield);
  if (g_yield_plain) return sched_yield();
  const uint64_t now = __builtin_ia32_rdtsc();
  t_yields = now - t_last_yield < g_yield_gap ? t_yields + 1 : 0;  // (t_last_yield is taken after a nap)
  t_last_yield = now;
  if (t_yields == 0) t_idle_since = now;
  if (t_yields < 64 || now - t_idle_since < g_idle_before_nap) return sched_yield();
  count(g_n_nap);
  // The longer it has been idle, the longer the nap: 50 us at first (so a
  // burst of small jobs, as while a game loads, is picked up at once), up to
  // 1 ms for a worker with nothing to do.
  const unsigned naps = t_yields - 64;
  const long ns = std::min(50000L << std::min(naps / 32, 5u), 1000000L);
  const timespec nap = {0, ns};
  nanosleep(&nap, nullptr);
  t_last_yield = __builtin_ia32_rdtsc();
  return 0;
}

// ===========================================================================
// futex (through syscall()), as FreeBSD's umtx
// ===========================================================================

long do_futex(uint32_t* addr, int op, uint32_t val, const timespec* ts, uint32_t* addr2, uint32_t val3) {
  count(g_n_futex);
  const bool priv = op & 128, realtime = op & 256;
  const int cmd = op & 127;
  const int wait_op = priv ? UMTX_OP_WAIT_UINT_PRIVATE : UMTX_OP_WAIT_UINT;
  const int wake_op = priv ? UMTX_OP_WAKE_PRIVATE : UMTX_OP_WAKE;
  switch (cmd) {
    case 0:    // FUTEX_WAIT (relative timeout)
    case 9: {  // FUTEX_WAIT_BITSET (absolute timeout)
      if (cmd == 9 && val3 != 0xffffffffu) note_once("futex bitsets other than ~0: woken as ~0");
      if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != val) return errno = EAGAIN, -1;
      int r;
      if (!ts) {
        r = _umtx_op(addr, wait_op, val, nullptr, nullptr);
      } else if (cmd == 0) {
        const uint64_t deadline = g_stats ? now_ns(CLOCK_MONOTONIC) + ts_ns(ts) : 0;
        r = _umtx_op(addr, wait_op, val, reinterpret_cast<void*>(sizeof(timespec)), const_cast<timespec*>(ts));
        if (r != 0 && errno == ETIMEDOUT) note_timeout(g_w_futex, CLOCK_MONOTONIC, deadline);
      } else {
        _umtx_time t{};
        t._timeout = *ts;
        t._flags = UMTX_ABSTIME;
        t._clockid = realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        r = _umtx_op(addr, wait_op, val, reinterpret_cast<void*>(sizeof t), &t);
        if (r != 0 && errno == ETIMEDOUT) note_timeout(g_w_futex, t._clockid, ts_ns(ts));
      }
      return r == 0 ? 0 : -1;
    }
    case 1:   // FUTEX_WAKE
    case 10:  // FUTEX_WAKE_BITSET
      return _umtx_op(addr, wake_op, val, nullptr, nullptr) == 0 ? 0 : -1;
    case 3:    // FUTEX_REQUEUE
    case 4: {  // FUTEX_CMP_REQUEUE: waking everyone instead is allowed (spurious wake-ups)
      if (cmd == 4 && __atomic_load_n(addr, __ATOMIC_ACQUIRE) != val3) return errno = EAGAIN, -1;
      return _umtx_op(addr, wake_op, INT_MAX, nullptr, nullptr) == 0 ? 0 : -1;
    }
    case 5: {  // FUTEX_WAKE_OP
      const uint32_t enc = val3;
      const int opnum = (enc >> 28) & 7, cmp = (enc >> 24) & 15;
      uint32_t oparg = (enc >> 12) & 0xfff, cmparg = enc & 0xfff;
      if ((enc >> 28) & 8) oparg = 1u << (oparg & 31);
      uint32_t old = __atomic_load_n(addr2, __ATOMIC_RELAXED), nv;
      do {
        switch (opnum) {
          case 0: nv = oparg; break;
          case 1: nv = old + oparg; break;
          case 2: nv = old | oparg; break;
          case 3: nv = old & ~oparg; break;
          case 4: nv = old ^ oparg; break;
          default: return errno = ENOSYS, -1;
        }
      } while (!__atomic_compare_exchange_n(addr2, &old, nv, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
      bool hit = false;
      const int o = static_cast<int>(old), c = static_cast<int>(cmparg);
      switch (cmp) {
        case 0: hit = o == c; break;
        case 1: hit = o != c; break;
        case 2: hit = o < c; break;
        case 3: hit = o <= c; break;
        case 4: hit = o > c; break;
        case 5: hit = o >= c; break;
      }
      _umtx_op(addr, wake_op, val, nullptr, nullptr);
      if (hit) _umtx_op(addr2, wake_op, static_cast<u_long>(reinterpret_cast<uintptr_t>(ts)), nullptr, nullptr);
      return 0;
    }
  }
  note_once("futex op %d: not translated", cmd);
  return errno = ENOSYS, -1;
}

// ===========================================================================
// Sockets
// ===========================================================================

int af_to_bsd(int f) {
  switch (f) {
    case 0: return AF_UNSPEC;
    case 1: return AF_UNIX;
    case 2: return AF_INET;
    case 10: return AF_INET6;
  }
  return -1;
}
int af_to_lx(int f) {
  switch (f) {
    case AF_UNSPEC: return 0;
    case AF_UNIX: return 1;
    case AF_INET: return 2;
    case AF_INET6: return 10;
  }
  return f;
}

// sockaddr: Linux starts with a 16-bit family; FreeBSD with a length byte and
// an 8-bit family. The rest of sockaddr_in and sockaddr_in6 matches.
bool sa_to_bsd(const void* lx, socklen_t len, sockaddr_storage* out, socklen_t* outlen) {
  if (len < 2) return errno = EINVAL, false;
  uint16_t fam;
  memcpy(&fam, lx, 2);
  const auto* p = static_cast<const char*>(lx);
  memset(out, 0, sizeof *out);
  switch (fam) {
    case 2: {
      if (len < 8) return errno = EINVAL, false;
      auto* s = reinterpret_cast<sockaddr_in*>(out);
      s->sin_len = sizeof *s;
      s->sin_family = AF_INET;
      memcpy(&s->sin_port, p + 2, 2);
      memcpy(&s->sin_addr, p + 4, 4);
      *outlen = sizeof *s;
      return true;
    }
    case 10: {
      if (len < 24) return errno = EINVAL, false;
      auto* s = reinterpret_cast<sockaddr_in6*>(out);
      s->sin6_len = sizeof *s;
      s->sin6_family = AF_INET6;
      memcpy(&s->sin6_port, p + 2, 2);
      memcpy(&s->sin6_flowinfo, p + 4, 4);
      memcpy(&s->sin6_addr, p + 8, 16);
      if (len >= 28) memcpy(&s->sin6_scope_id, p + 24, 4);
      *outlen = sizeof *s;
      return true;
    }
    case 1: {
      auto* u = reinterpret_cast<sockaddr_un*>(out);
      u->sun_family = AF_UNIX;
      const size_t n = len - 2;
      if (n == 0) {
        u->sun_len = 2;
        *outlen = 2;
        return true;
      }
      if (p[2] == '\0') {
        note_once("abstract unix socket: FreeBSD has none");
        return errno = ENOENT, false;
      }
      const size_t pl = strnlen(p + 2, n);
      if (pl >= sizeof u->sun_path) return errno = ENAMETOOLONG, false;
      memcpy(u->sun_path, p + 2, pl);
      u->sun_len = static_cast<uint8_t>(offsetof(sockaddr_un, sun_path) + pl + 1);
      *outlen = u->sun_len;
      return true;
    }
    case 0: {
      out->ss_len = static_cast<uint8_t>(std::min<socklen_t>(len, sizeof *out));
      out->ss_family = AF_UNSPEC;
      *outlen = out->ss_len;
      return true;
    }
  }
  note_once("socket address family %u: not on FreeBSD", fam);
  return errno = EAFNOSUPPORT, false;
}
// Writes the Linux form of B (at most CAP bytes) and returns its full length.
socklen_t sa_to_lx(const sockaddr* b, socklen_t blen, void* lx, socklen_t cap) {
  unsigned char t[sizeof(sockaddr_storage)] = {};
  size_t n;
  uint16_t fam = static_cast<uint16_t>(af_to_lx(b->sa_family));
  switch (b->sa_family) {
    case AF_INET: {
      const auto* s = reinterpret_cast<const sockaddr_in*>(b);
      memcpy(t + 2, &s->sin_port, 2);
      memcpy(t + 4, &s->sin_addr, 4);
      n = 16;
      break;
    }
    case AF_INET6: {
      const auto* s = reinterpret_cast<const sockaddr_in6*>(b);
      memcpy(t + 2, &s->sin6_port, 2);
      memcpy(t + 4, &s->sin6_flowinfo, 4);
      memcpy(t + 8, &s->sin6_addr, 16);
      memcpy(t + 24, &s->sin6_scope_id, 4);
      n = 28;
      break;
    }
    case AF_UNIX: {
      const auto* u = reinterpret_cast<const sockaddr_un*>(b);
      const size_t off = offsetof(sockaddr_un, sun_path);
      const size_t pl = blen > off ? strnlen(u->sun_path, std::min<size_t>(blen - off, sizeof u->sun_path)) : 0;
      memcpy(t + 2, u->sun_path, pl);
      n = pl ? 2 + pl + 1 : 2;
      break;
    }
    case AF_UNSPEC:
      n = 2;
      break;
    default:
      n = std::min<size_t>(blen, sizeof t);
      memcpy(t, b, n);
  }
  memcpy(t, &fam, 2);
  if (lx && cap) memcpy(lx, t, std::min<size_t>(n, cap));
  return static_cast<socklen_t>(n);
}

int socktype_to_bsd(int t) {
  int b = t & 0xf;
  if (t & 0x800) b |= SOCK_NONBLOCK;
  if (t & 0x80000) b |= SOCK_CLOEXEC;
  if (t & ~(0xf | 0x800 | 0x80000)) note_once("socket type bits %#x: not known", t & ~(0xf | 0x800 | 0x80000));
  return b;
}
int sh_socket(int d, int t, int p) {
  const int b = af_to_bsd(d);
  if (b < 0) {
    note_once("socket family %d (e.g. netlink): not on FreeBSD", d);
    return errno = EAFNOSUPPORT, -1;
  }
  return socket(b, socktype_to_bsd(t), p);
}
int sh_socketpair(int d, int t, int p, int sv[2]) {
  const int b = af_to_bsd(d);
  if (b < 0) return errno = EAFNOSUPPORT, -1;
  return socketpair(b, socktype_to_bsd(t), p, sv);
}
int sh_bind(int fd, const void* a, socklen_t l) {
  sockaddr_storage s;
  socklen_t sl;
  if (!sa_to_bsd(a, l, &s, &sl)) return -1;
  return bind(fd, reinterpret_cast<sockaddr*>(&s), sl);
}
int sh_connect(int fd, const void* a, socklen_t l) {
  count(g_n_connect);
  sockaddr_storage s;
  socklen_t sl;
  if (!sa_to_bsd(a, l, &s, &sl)) return -1;
  netping_note(reinterpret_cast<sockaddr*>(&s), sl);  // (where it goes, for the stats overlay's ping)
  return connect(fd, reinterpret_cast<sockaddr*>(&s), sl);
}
int addr_out(int r, const sockaddr_storage& s, socklen_t sl, void* a, socklen_t* l) {
  if (r >= 0 && a && l) *l = sa_to_lx(reinterpret_cast<const sockaddr*>(&s), sl, a, *l);
  return r;
}
int sh_accept4(int fd, void* a, socklen_t* l, int f) {
  sockaddr_storage s{};
  socklen_t sl = sizeof s;
  int bf = 0;
  if (f & 0x800) bf |= SOCK_NONBLOCK;
  if (f & 0x80000) bf |= SOCK_CLOEXEC;
  return addr_out(accept4(fd, a ? reinterpret_cast<sockaddr*>(&s) : nullptr, a ? &sl : nullptr, bf), s, sl, a, l);
}
int sh_accept(int fd, void* a, socklen_t* l) { return sh_accept4(fd, a, l, 0); }
int sh_getsockname(int fd, void* a, socklen_t* l) {
  sockaddr_storage s{};
  socklen_t sl = sizeof s;
  return addr_out(getsockname(fd, reinterpret_cast<sockaddr*>(&s), &sl), s, sl, a, l);
}
int sh_getpeername(int fd, void* a, socklen_t* l) {
  sockaddr_storage s{};
  socklen_t sl = sizeof s;
  return addr_out(getpeername(fd, reinterpret_cast<sockaddr*>(&s), &sl), s, sl, a, l);
}

constexpr Bit kMsg[] = {
    {0x1, MSG_OOB},       {0x2, MSG_PEEK},          {0x4, MSG_DONTROUTE}, {0x8, MSG_CTRUNC},
    {0x20, MSG_TRUNC},    {0x40, MSG_DONTWAIT},     {0x80, MSG_EOR},      {0x100, MSG_WAITALL},
    {0x200, 0},           {0x400, 0},               {0x800, 0},           {0x1000, 0},  // FIN SYN CONFIRM RST
    {0x4000, MSG_NOSIGNAL}, {0x8000, 0},                                                // MORE
    {0x10000, MSG_WAITFORONE}, {0x20000000, 0},                                         // FASTOPEN
    {0x40000000, MSG_CMSG_CLOEXEC},
};
constexpr unsigned kLxMsgErrqueue = 0x2000;
int msg_to_bsd(int f) { return static_cast<int>(bits_to_bsd(static_cast<unsigned>(f), kMsg, "send/recv flags")); }
int msg_to_lx(int f) { return static_cast<int>(bits_to_lx(static_cast<unsigned>(f), kMsg)); }

inline ssize_t bytes(std::atomic<uint64_t>& c, ssize_t r) {
  if (g_stats && r > 0) c.fetch_add(static_cast<uint64_t>(r), std::memory_order_relaxed);
  return r;
}
// UDP_SEGMENT (Linux UDP GSO): one send of a big buffer that the kernel cuts
// into datagrams of a given size. FreeBSD has no such thing, so tonic cuts it.
// Per message (a SOL_UDP/UDP_SEGMENT control message) or per socket
// (setsockopt), as on Linux.
constexpr int kLxSolUdp = 17, kLxUdpSegment = 103;
std::mutex g_gso_mu;
std::unordered_map<int, int> g_gso;  // fd -> size set with setsockopt
std::atomic<bool> g_gso_any{false};
int gso_default(int fd) {
  if (!g_gso_any.load(std::memory_order_relaxed)) return 0;
  std::lock_guard<std::mutex> l(g_gso_mu);
  auto it = g_gso.find(fd);
  return it == g_gso.end() ? 0 : it->second;
}
// Sends DATA as datagrams of at most GSO bytes, with M's address and control.
ssize_t send_segments(int fd, msghdr m, const char* data, size_t len, size_t gso, int flags) {
  size_t off = 0;
  while (off < len) {
    iovec v{const_cast<char*>(data + off), std::min(gso, len - off)};
    m.msg_iov = &v;
    m.msg_iovlen = 1;
    if (sendmsg(fd, &m, flags) < 0) return off ? static_cast<ssize_t>(off) : -1;
    off += v.iov_len;
  }
  return static_cast<ssize_t>(off);
}

ssize_t sh_send(int fd, const void* b, size_t n, int f) {
  if (const int gso = gso_default(fd); gso > 0 && n > static_cast<size_t>(gso))
    return bytes(g_net_out, send_segments(fd, msghdr{}, static_cast<const char*>(b), n, static_cast<size_t>(gso), msg_to_bsd(f)));
  return bytes(g_net_out, send(fd, b, n, msg_to_bsd(f)));
}
ssize_t sh_recv(int fd, void* b, size_t n, int f) {
  if (f & kLxMsgErrqueue) return errno = EAGAIN, -1;  // no error queue: nothing queued
  return bytes(g_net_in, recv(fd, b, n, msg_to_bsd(f)));
}
ssize_t sh_sendto(int fd, const void* b, size_t n, int f, const void* a, socklen_t l) {
  if (!a) return bytes(g_net_out, sendto(fd, b, n, msg_to_bsd(f), nullptr, 0));
  sockaddr_storage s;
  socklen_t sl;
  if (!sa_to_bsd(a, l, &s, &sl)) return -1;
  netping_note(reinterpret_cast<sockaddr*>(&s), sl);
  if (const int gso = gso_default(fd); gso > 0 && n > static_cast<size_t>(gso)) {
    msghdr m{};
    m.msg_name = &s;
    m.msg_namelen = sl;
    return bytes(g_net_out, send_segments(fd, m, static_cast<const char*>(b), n, static_cast<size_t>(gso), msg_to_bsd(f)));
  }
  return bytes(g_net_out, sendto(fd, b, n, msg_to_bsd(f), reinterpret_cast<sockaddr*>(&s), sl));
}
ssize_t sh_recvfrom(int fd, void* b, size_t n, int f, void* a, socklen_t* l) {
  if (f & kLxMsgErrqueue) return errno = EAGAIN, -1;
  sockaddr_storage s{};
  socklen_t sl = sizeof s;
  const ssize_t r = recvfrom(fd, b, n, msg_to_bsd(f), a ? reinterpret_cast<sockaddr*>(&s) : nullptr, a ? &sl : nullptr);
  if (r >= 0 && a && l) *l = sa_to_lx(reinterpret_cast<sockaddr*>(&s), sl, a, *l);
  return bytes(g_net_in, r);
}

// struct msghdr and cmsghdr as Linux lays them out.
struct LxMsghdr {
  void* msg_name;
  uint32_t msg_namelen;
  iovec* msg_iov;
  size_t msg_iovlen;
  void* msg_control;
  size_t msg_controllen;
  int msg_flags;
};
static_assert(sizeof(LxMsghdr) == 56, "Linux x86_64 struct msghdr is 56 bytes");
struct LxCmsg {
  size_t cmsg_len;
  int cmsg_level, cmsg_type;
};
constexpr size_t kAlign8(size_t n) { return (n + 7) & ~size_t{7}; }

struct CmsgType {
  int lx_level, lx_type, bsd_level, bsd_type;
};
constexpr CmsgType kCmsg[] = {
    {1, 1, SOL_SOCKET, SCM_RIGHTS},
    {1, 29, SOL_SOCKET, SCM_TIMESTAMP},  // SO_TIMESTAMP: struct timeval on both
    {0, 1, IPPROTO_IP, IP_TOS},
    {0, 1, IPPROTO_IP, IP_RECVTOS},
    {0, 2, IPPROTO_IP, IP_TTL},
    {0, 2, IPPROTO_IP, IP_RECVTTL},
    {41, 50, IPPROTO_IPV6, IPV6_PKTINFO},
    {41, 52, IPPROTO_IPV6, IPV6_HOPLIMIT},
    {41, 67, IPPROTO_IPV6, IPV6_TCLASS},
};
bool cmsg_to_bsd(int ll, int lt, int* bl, int* bt) {
  for (const auto& c : kCmsg)
    if (c.lx_level == ll && c.lx_type == lt) return *bl = c.bsd_level, *bt = c.bsd_type, true;
  return false;
}
bool cmsg_to_lx(int bl, int bt, int* ll, int* lt) {
  for (const auto& c : kCmsg)
    if (c.bsd_level == bl && c.bsd_type == bt) return *ll = c.lx_level, *lt = c.lx_type, true;
  return false;
}
// Linux control messages -> FreeBSD (what sendmsg takes).
int gso_in_control(const LxMsghdr* m) {
  if (!m->msg_control) return 0;
  const char* p = static_cast<const char*>(m->msg_control);
  const char* end = p + m->msg_controllen;
  while (p + sizeof(LxCmsg) <= end) {
    const auto* c = reinterpret_cast<const LxCmsg*>(p);
    if (c->cmsg_len < sizeof(LxCmsg) || p + c->cmsg_len > end) break;
    if (c->cmsg_level == kLxSolUdp && c->cmsg_type == kLxUdpSegment && c->cmsg_len >= sizeof(LxCmsg) + 2) {
      uint16_t v;
      memcpy(&v, p + sizeof(LxCmsg), 2);
      return v;
    }
    p += kAlign8(c->cmsg_len);
  }
  return 0;
}
void control_to_bsd(const LxMsghdr* m, std::vector<char>& out) {
  const char* p = static_cast<const char*>(m->msg_control);
  const char* end = p + m->msg_controllen;
  while (p + sizeof(LxCmsg) <= end) {
    const auto* c = reinterpret_cast<const LxCmsg*>(p);
    if (c->cmsg_len < sizeof(LxCmsg) || p + c->cmsg_len > end) break;
    const size_t dlen = c->cmsg_len - sizeof(LxCmsg);
    int bl, bt;
    if (cmsg_to_bsd(c->cmsg_level, c->cmsg_type, &bl, &bt)) {
      const size_t at = out.size();
      out.resize(at + CMSG_SPACE(dlen));
      auto* h = reinterpret_cast<cmsghdr*>(out.data() + at);
      h->cmsg_len = static_cast<socklen_t>(CMSG_LEN(dlen));
      h->cmsg_level = bl;
      h->cmsg_type = bt;
      memcpy(CMSG_DATA(h), p + sizeof(LxCmsg), dlen);
    } else if (!(c->cmsg_level == kLxSolUdp && c->cmsg_type == kLxUdpSegment)) {  // (that one: send_segments)
      note_once("control message level %d type %d: not on FreeBSD (dropped)", c->cmsg_level, c->cmsg_type);
    }
    p += kAlign8(c->cmsg_len);
  }
}
// FreeBSD control messages -> Linux, into the library's buffer. Returns the
// length used; sets *trunc if something did not fit.
size_t control_to_lx(const msghdr& m, LxMsghdr* lm, bool* trunc) {
  char* out = static_cast<char*>(lm->msg_control);
  const size_t cap = lm->msg_controllen;
  size_t used = 0;
  for (cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
    const size_t dlen = h->cmsg_len - CMSG_LEN(0);
    int ll, lt;
    if (!cmsg_to_lx(h->cmsg_level, h->cmsg_type, &ll, &lt)) {
      note_once("received control message level %d type %d: no Linux form (dropped)", h->cmsg_level, h->cmsg_type);
      continue;
    }
    const size_t space = sizeof(LxCmsg) + kAlign8(dlen);
    if (!out || used + sizeof(LxCmsg) + dlen > cap) {
      *trunc = true;
      break;
    }
    auto* c = reinterpret_cast<LxCmsg*>(out + used);
    c->cmsg_len = sizeof(LxCmsg) + dlen;
    c->cmsg_level = ll;
    c->cmsg_type = lt;
    memcpy(out + used + sizeof(LxCmsg), CMSG_DATA(h), dlen);
    used += std::min(space, cap - used);
  }
  return used;
}
ssize_t sh_sendmsg(int fd, const LxMsghdr* lm, int f) {
  msghdr m{};
  sockaddr_storage s;
  if (lm->msg_name && lm->msg_namelen) {
    socklen_t sl;
    if (!sa_to_bsd(lm->msg_name, lm->msg_namelen, &s, &sl)) return -1;
    netping_note(reinterpret_cast<sockaddr*>(&s), sl);
    m.msg_name = &s;
    m.msg_namelen = sl;
  }
  m.msg_iov = lm->msg_iov;
  m.msg_iovlen = static_cast<int>(lm->msg_iovlen);
  std::vector<char> ctl;
  if (lm->msg_control && lm->msg_controllen) {
    control_to_bsd(lm, ctl);
    m.msg_control = ctl.empty() ? nullptr : ctl.data();
    m.msg_controllen = static_cast<socklen_t>(ctl.size());
  }
  int gso = gso_in_control(lm);
  if (!gso) gso = gso_default(fd);
  if (gso > 0) {
    size_t total = 0;
    for (size_t i = 0; i < lm->msg_iovlen; ++i) total += lm->msg_iov[i].iov_len;
    if (total > static_cast<size_t>(gso)) {
      std::vector<char> flat(total);
      size_t at = 0;
      for (size_t i = 0; i < lm->msg_iovlen; ++i) {
        memcpy(flat.data() + at, lm->msg_iov[i].iov_base, lm->msg_iov[i].iov_len);
        at += lm->msg_iov[i].iov_len;
      }
      return bytes(g_net_out, send_segments(fd, m, flat.data(), total, static_cast<size_t>(gso), msg_to_bsd(f)));
    }
  }
  return bytes(g_net_out, sendmsg(fd, &m, msg_to_bsd(f)));
}
ssize_t sh_recvmsg(int fd, LxMsghdr* lm, int f) {
  if (f & kLxMsgErrqueue) return errno = EAGAIN, -1;
  msghdr m{};
  sockaddr_storage s{};
  if (lm->msg_name) {
    m.msg_name = &s;
    m.msg_namelen = sizeof s;
  }
  m.msg_iov = lm->msg_iov;
  m.msg_iovlen = static_cast<int>(lm->msg_iovlen);
  std::vector<char> ctl;
  if (lm->msg_control && lm->msg_controllen) {
    ctl.resize(lm->msg_controllen + 256);
    m.msg_control = ctl.data();
    m.msg_controllen = static_cast<socklen_t>(ctl.size());
  }
  const ssize_t r = recvmsg(fd, &m, msg_to_bsd(f));
  if (r < 0) return r;
  if (lm->msg_name) lm->msg_namelen = sa_to_lx(reinterpret_cast<sockaddr*>(&s), m.msg_namelen, lm->msg_name, lm->msg_namelen);
  bool trunc = false;
  lm->msg_controllen = m.msg_control ? control_to_lx(m, lm, &trunc) : 0;
  lm->msg_flags = msg_to_lx(m.msg_flags) | (trunc ? 0x8 : 0);
  return r;
}
struct LxMmsghdr {
  LxMsghdr msg_hdr;
  unsigned msg_len;
};
static_assert(sizeof(LxMmsghdr) == 64, "Linux struct mmsghdr is 64 bytes");
int sh_sendmmsg(int fd, LxMmsghdr* v, unsigned n, int f) {
  unsigned i = 0;
  for (; i < n; ++i) {
    const ssize_t r = sh_sendmsg(fd, &v[i].msg_hdr, f);
    if (r < 0) return i ? static_cast<int>(i) : -1;
    v[i].msg_len = static_cast<unsigned>(r);
  }
  return static_cast<int>(i);
}
int sh_recvmmsg(int fd, LxMmsghdr* v, unsigned n, int f, timespec*) {
  unsigned i = 0;
  const int rest = (f & 0x10000) ? (f & ~0x10000) | 0x40 : f;  // MSG_WAITFORONE: only the first one waits
  for (; i < n; ++i) {
    const ssize_t r = sh_recvmsg(fd, &v[i].msg_hdr, i ? rest : (f & ~0x10000));
    if (r < 0) return i ? static_cast<int>(i) : -1;
    v[i].msg_len = static_cast<unsigned>(r);
  }
  return static_cast<int>(i);
}
LxCmsg* sh_cmsg_nxthdr(LxMsghdr* m, LxCmsg* c) {
  if (c->cmsg_len < sizeof(LxCmsg)) return nullptr;
  char* next = reinterpret_cast<char*>(c) + kAlign8(c->cmsg_len);
  char* end = static_cast<char*>(m->msg_control) + m->msg_controllen;
  if (next + sizeof(LxCmsg) > end) return nullptr;
  if (next + kAlign8(reinterpret_cast<LxCmsg*>(next)->cmsg_len) > end) return nullptr;
  return reinterpret_cast<LxCmsg*>(next);
}

// --- socket options ------------------------------------------------------------------
struct Opt {
  int lx_level, lx_name, level, name;
};
constexpr Opt kOpts[] = {
    {1, 1, SOL_SOCKET, SO_DEBUG},        {1, 2, SOL_SOCKET, SO_REUSEADDR},   {1, 3, SOL_SOCKET, SO_TYPE},
    {1, 4, SOL_SOCKET, SO_ERROR},        {1, 5, SOL_SOCKET, SO_DONTROUTE},   {1, 6, SOL_SOCKET, SO_BROADCAST},
    {1, 7, SOL_SOCKET, SO_SNDBUF},       {1, 8, SOL_SOCKET, SO_RCVBUF},      {1, 9, SOL_SOCKET, SO_KEEPALIVE},
    {1, 10, SOL_SOCKET, SO_OOBINLINE},   {1, 13, SOL_SOCKET, SO_LINGER},     {1, 15, SOL_SOCKET, SO_REUSEPORT},
    {1, 18, SOL_SOCKET, SO_RCVLOWAT},    {1, 19, SOL_SOCKET, SO_SNDLOWAT},   {1, 20, SOL_SOCKET, SO_RCVTIMEO},
    {1, 21, SOL_SOCKET, SO_SNDTIMEO},    {1, 29, SOL_SOCKET, SO_TIMESTAMP},  {1, 30, SOL_SOCKET, SO_ACCEPTCONN},
    {1, 32, SOL_SOCKET, SO_SNDBUF},      {1, 33, SOL_SOCKET, SO_RCVBUF},     // *BUFFORCE
    {1, 38, SOL_SOCKET, SO_PROTOCOL},    {1, 39, SOL_SOCKET, SO_DOMAIN},
    {6, 1, IPPROTO_TCP, TCP_NODELAY},    {6, 2, IPPROTO_TCP, TCP_MAXSEG},    {6, 3, IPPROTO_TCP, TCP_NOPUSH},  // CORK
    {6, 4, IPPROTO_TCP, TCP_KEEPIDLE},   {6, 5, IPPROTO_TCP, TCP_KEEPINTVL}, {6, 6, IPPROTO_TCP, TCP_KEEPCNT},
    {6, 13, IPPROTO_TCP, TCP_CONGESTION}, {6, 23, IPPROTO_TCP, TCP_FASTOPEN},
    {0, 1, IPPROTO_IP, IP_TOS},          {0, 2, IPPROTO_IP, IP_TTL},         {0, 3, IPPROTO_IP, IP_HDRINCL},
    {0, 4, IPPROTO_IP, IP_OPTIONS},      {0, 12, IPPROTO_IP, IP_RECVTTL},    {0, 13, IPPROTO_IP, IP_RECVTOS},
    {0, 32, IPPROTO_IP, IP_MULTICAST_IF}, {0, 33, IPPROTO_IP, IP_MULTICAST_TTL}, {0, 34, IPPROTO_IP, IP_MULTICAST_LOOP},
    {0, 35, IPPROTO_IP, IP_ADD_MEMBERSHIP}, {0, 36, IPPROTO_IP, IP_DROP_MEMBERSHIP},
    {41, 16, IPPROTO_IPV6, IPV6_UNICAST_HOPS},   {41, 17, IPPROTO_IPV6, IPV6_MULTICAST_IF},
    {41, 18, IPPROTO_IPV6, IPV6_MULTICAST_HOPS}, {41, 19, IPPROTO_IPV6, IPV6_MULTICAST_LOOP},
    {41, 20, IPPROTO_IPV6, IPV6_JOIN_GROUP},     {41, 21, IPPROTO_IPV6, IPV6_LEAVE_GROUP},
    {41, 26, IPPROTO_IPV6, IPV6_V6ONLY},         {41, 49, IPPROTO_IPV6, IPV6_RECVPKTINFO},
    {41, 51, IPPROTO_IPV6, IPV6_RECVHOPLIMIT},   {41, 66, IPPROTO_IPV6, IPV6_RECVTCLASS},
    {41, 67, IPPROTO_IPV6, IPV6_TCLASS},
};
// Linux-only options that only tune behaviour: accepted, nothing to set.
constexpr Opt kTuning[] = {
    {1, 11, 0, 0},  // SO_NO_CHECK
    {1, 12, 0, 0},  // SO_PRIORITY
    {1, 46, 0, 0},  // SO_BUSY_POLL
    {0, 11, 0, 0},  // IP_RECVERR (nothing is ever queued, see MSG_ERRQUEUE)
    {41, 25, 0, 0}, // IPV6_RECVERR
    {6, 9, 0, 0},   // TCP_DEFER_ACCEPT
    {6, 12, 0, 0},  // TCP_QUICKACK
    {6, 18, 0, 0},  // TCP_USER_TIMEOUT
    {6, 25, 0, 0},  // TCP_NOTSENT_LOWAT
};
const Opt* find_opt(const Opt* t, size_t n, int level, int name) {
  for (size_t i = 0; i < n; ++i)
    if (t[i].lx_level == level && t[i].lx_name == name) return &t[i];
  return nullptr;
}
int sh_setsockopt(int fd, int level, int name, const void* v, socklen_t l) {
  if (level == kLxSolUdp && name == kLxUdpSegment) {  // per-socket GSO size (send_segments)
    int size = 0;
    if (v && l >= sizeof(int)) memcpy(&size, v, sizeof size);
    std::lock_guard<std::mutex> lk(g_gso_mu);
    if (size > 0) g_gso[fd] = size;
    else g_gso.erase(fd);
    g_gso_any = !g_gso.empty();
    return 0;
  }
  // IP(V6)_MTU_DISCOVER -> IP(V6)_DONTFRAG (DO and PROBE mean "don't fragment").
  if ((level == 0 && name == 10) || (level == 41 && name == 23)) {
    int mode = 0;
    if (v && l >= sizeof(int)) memcpy(&mode, v, sizeof mode);
    const int on = mode == 2 || mode == 3;
    return setsockopt(fd, level == 0 ? IPPROTO_IP : IPPROTO_IPV6, level == 0 ? IP_DONTFRAG : IPV6_DONTFRAG, &on, sizeof on);
  }
  if (const Opt* o = find_opt(kOpts, sizeof kOpts / sizeof *kOpts, level, name))
    return setsockopt(fd, o->level, o->name, v, l);
  if (find_opt(kTuning, sizeof kTuning / sizeof *kTuning, level, name)) return 0;
  note_once("setsockopt level %d option %d: not on FreeBSD", level, name);
  return errno = ENOPROTOOPT, -1;
}
int sh_getsockopt(int fd, int level, int name, void* v, socklen_t* l) {
  if (level == kLxSolUdp && name == kLxUdpSegment) {
    const int size = gso_default(fd);
    if (v && l && *l >= sizeof(int)) {
      memcpy(v, &size, sizeof size);
      *l = sizeof size;
    }
    return 0;
  }
  if ((level == 0 && name == 10) || (level == 41 && name == 23)) {
    int on = 0;
    socklen_t ol = sizeof on;
    const int r = getsockopt(fd, level == 0 ? IPPROTO_IP : IPPROTO_IPV6, level == 0 ? IP_DONTFRAG : IPV6_DONTFRAG, &on, &ol);
    if (r == 0 && v && l && *l >= sizeof(int)) {
      const int mode = on ? 2 : 0;
      memcpy(v, &mode, sizeof mode);
      *l = sizeof mode;
    }
    return r;
  }
  if (const Opt* o = find_opt(kOpts, sizeof kOpts / sizeof *kOpts, level, name)) {
    const int r = getsockopt(fd, o->level, o->name, v, l);
    if (r == 0 && v && l && *l >= sizeof(int) && o->level == SOL_SOCKET) {
      int x;
      memcpy(&x, v, sizeof x);
      if (o->name == SO_ERROR) x = lx_errno(x);
      else if (o->name == SO_DOMAIN) x = af_to_lx(x);
      memcpy(v, &x, sizeof x);
    }
    return r;
  }
  if (find_opt(kTuning, sizeof kTuning / sizeof *kTuning, level, name)) {
    if (v && l && *l >= sizeof(int)) {
      memset(v, 0, sizeof(int));
      *l = sizeof(int);
    }
    return 0;
  }
  note_once("getsockopt level %d option %d: not on FreeBSD", level, name);
  return errno = ENOPROTOOPT, -1;
}

// --- poll ----------------------------------------------------------------------------
// The low bits match; POLLWRNORM, POLLWRBAND and POLLRDHUP do not.
short poll_to_bsd(short e) {
  short b = e & 0xff;
  if (e & 0x100) b |= POLLWRNORM;
  if (e & 0x200) b |= POLLWRBAND;
  if (e & 0x2000) b |= POLLRDHUP;
  return b;
}
short poll_to_lx(short b, short asked) {
  short e = b & 0xff & ~POLLWRBAND;
  if ((asked & 0x100) && (b & POLLWRNORM)) e |= 0x100;
  if (b & POLLWRBAND) e |= 0x200;
  if (b & POLLRDHUP) e |= 0x2000;
  return e;
}
template <typename F>
int poll_with(pollfd* fds, nfds_t n, F call) {
  bool plain = true;
  for (nfds_t i = 0; i < n; ++i)
    if (fds[i].events & ~0xff) plain = false;
  if (plain) return call();
  std::vector<short> asked(n);
  for (nfds_t i = 0; i < n; ++i) asked[i] = fds[i].events, fds[i].events = poll_to_bsd(fds[i].events);
  const int r = call();
  for (nfds_t i = 0; i < n; ++i) fds[i].revents = poll_to_lx(fds[i].revents, asked[i]), fds[i].events = asked[i];
  return r;
}
void note_poll(int r, uint64_t t0) {
  if (!g_stats) return;
  g_n_poll.fetch_add(1, std::memory_order_relaxed);
  if (r == 0) {
    g_n_poll_timeout.fetch_add(1, std::memory_order_relaxed);
    g_poll_timeout_ms.fetch_add((now_ns(CLOCK_MONOTONIC) - t0) / 1000000, std::memory_order_relaxed);
  }
}
int sh_poll(pollfd* fds, nfds_t n, int t) {
  const uint64_t t0 = g_stats ? now_ns(CLOCK_MONOTONIC) : 0;
  const int r = poll_with(fds, n, [&] { return poll(fds, n, t); });
  note_poll(r, t0);
  return r;
}
int sh_ppoll(pollfd* fds, nfds_t n, const timespec* ts, const LxSigset* m) {
  sigset_t s;
  if (m) set_to_bsd(*m, &s);
  const uint64_t t0 = g_stats ? now_ns(CLOCK_MONOTONIC) : 0;
  const int r = poll_with(fds, n, [&] { return ppoll(fds, n, ts, m ? &s : nullptr); });
  note_poll(r, t0);
  return r;
}
int sh_pselect(int n, fd_set* r, fd_set* w, fd_set* e, const timespec* ts, const LxSigset* m) {
  sigset_t s;
  if (m) set_to_bsd(*m, &s);
  return pselect(n, r, w, e, ts, m ? &s : nullptr);
}
int sh_epoll_pwait(int ep, void* ev, int max, int t, const LxSigset* m) {
  static auto fn = reinterpret_cast<int (*)(int, void*, int, int, const sigset_t*)>(dlsym(RTLD_DEFAULT, "epoll_pwait"));
  if (!fn) return errno = ENOSYS, -1;
  sigset_t s;
  if (m) set_to_bsd(*m, &s);
  return fn(ep, ev, max, t, m ? &s : nullptr);
}

int sh_ioctl(int fd, unsigned long req, ...) {
  va_list ap;
  va_start(ap, req);
  void* arg = va_arg(ap, void*);
  va_end(ap);
  switch (req) {
    case 0x541B: return ioctl(fd, FIONREAD, arg);
    case 0x5421: return ioctl(fd, FIONBIO, arg);
    case 0x5451: return ioctl(fd, FIOCLEX);
    case 0x5450: return ioctl(fd, FIONCLEX);
    case 0x5413: return ioctl(fd, TIOCGWINSZ, arg);  // struct winsize matches
    case 0x5414: return ioctl(fd, TIOCSWINSZ, arg);
    case 0x540F: return ioctl(fd, TIOCGPGRP, arg);
  }
  note_once("ioctl %#lx: no FreeBSD translation", req);
  return errno = ENOTTY, -1;
}

int sh_inet_pton(int af, const char* s, void* d) {
  const int b = af_to_bsd(af);
  return b < 0 ? (errno = EAFNOSUPPORT, -1) : inet_pton(b, s, d);
}
const char* sh_inet_ntop(int af, const void* s, char* d, socklen_t n) {
  const int b = af_to_bsd(af);
  return b < 0 ? (errno = EAFNOSUPPORT, nullptr) : inet_ntop(b, s, d, n);
}

// getaddrinfo/getnameinfo: shims.cpp already converts the struct and flags
// between bionic and the host; here only families and addresses change.
struct BionicAddrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  socklen_t ai_addrlen;
  char* ai_canonname;
  sockaddr* ai_addr;
  BionicAddrinfo* ai_next;
};
using GaiFn = int (*)(const char*, const char*, const BionicAddrinfo*, BionicAddrinfo**);
using GniFn = int (*)(const sockaddr*, socklen_t, char*, socklen_t, char*, socklen_t, int);
GaiFn g_prev_gai;
GniFn g_prev_gni;
int sh_getaddrinfo(const char* node, const char* serv, const BionicAddrinfo* hints, BionicAddrinfo** res) {
  BionicAddrinfo h{};
  if (hints) {
    h = *hints;
    const int f = af_to_bsd(h.ai_family);
    if (f < 0) return 5;  // bionic EAI_FAMILY
    h.ai_family = f;
    h.ai_socktype &= 0xf;
  }
  const int r = g_prev_gai(node, serv, hints ? &h : nullptr, res);
  if (r == 0)
    for (BionicAddrinfo* p = *res; p; p = p->ai_next) {
      p->ai_family = af_to_lx(p->ai_family);
      if (p->ai_addr) {
        unsigned char t[sizeof(sockaddr_storage)];
        const socklen_t n = sa_to_lx(p->ai_addr, p->ai_addrlen, t, sizeof t);
        memcpy(p->ai_addr, t, std::min(n, p->ai_addrlen));
      }
    }
  return r;
}
int sh_getnameinfo(const void* sa, socklen_t len, char* h, socklen_t hl, char* s, socklen_t sl, int f) {
  sockaddr_storage b;
  socklen_t bl;
  if (!sa_to_bsd(sa, len, &b, &bl)) return 5;  // EAI_FAMILY
  return g_prev_gni(reinterpret_cast<sockaddr*>(&b), bl, h, hl, s, sl, f);
}

// getifaddrs: addresses only (no link-layer entries, which Android apps do
// not get either), in Linux's layout.
struct LxIfaddrs {
  LxIfaddrs* ifa_next;
  char* ifa_name;
  unsigned ifa_flags;
  void* ifa_addr;
  void* ifa_netmask;
  void* ifa_ifu;
  void* ifa_data;
};
unsigned iff_to_lx(unsigned b) {
  unsigned l = b & (IFF_UP | IFF_BROADCAST | IFF_DEBUG | IFF_LOOPBACK | IFF_POINTOPOINT | IFF_NOARP | IFF_PROMISC | IFF_ALLMULTI);
  if (b & IFF_DRV_RUNNING) l |= 0x40 | 0x10000;  // IFF_RUNNING, IFF_LOWER_UP
  if (b & IFF_MULTICAST) l |= 0x1000;
  return l;
}
int sh_getifaddrs(LxIfaddrs** out) {
  ifaddrs* h;
  if (getifaddrs(&h) != 0) return -1;
  LxIfaddrs* head = nullptr;
  LxIfaddrs** tail = &head;
  constexpr size_t kSa = 32;
  for (ifaddrs* p = h; p; p = p->ifa_next) {
    if (!p->ifa_addr || (p->ifa_addr->sa_family != AF_INET && p->ifa_addr->sa_family != AF_INET6)) continue;
    const size_t nl = strlen(p->ifa_name) + 1;
    auto* e = static_cast<LxIfaddrs*>(calloc(1, sizeof(LxIfaddrs) + 3 * kSa + nl));
    if (!e) break;
    char* blk = reinterpret_cast<char*>(e + 1);
    e->ifa_flags = iff_to_lx(p->ifa_flags);
    e->ifa_addr = blk;
    sa_to_lx(p->ifa_addr, p->ifa_addr->sa_len, blk, kSa);
    if (p->ifa_netmask) {
      e->ifa_netmask = blk + kSa;
      sockaddr_storage m{};
      memcpy(&m, p->ifa_netmask, std::min<size_t>(p->ifa_netmask->sa_len ? p->ifa_netmask->sa_len : sizeof m, sizeof m));
      m.ss_family = p->ifa_addr->sa_family;  // FreeBSD may leave a netmask's family unset
      sa_to_lx(reinterpret_cast<sockaddr*>(&m), sizeof m, blk + kSa, kSa);
    }
    if (p->ifa_dstaddr) {
      e->ifa_ifu = blk + 2 * kSa;
      sa_to_lx(p->ifa_dstaddr, p->ifa_dstaddr->sa_len, blk + 2 * kSa, kSa);
    }
    e->ifa_name = blk + 3 * kSa;
    memcpy(e->ifa_name, p->ifa_name, nl);
    *tail = e;
    tail = &e->ifa_next;
  }
  freeifaddrs(h);
  *out = head;
  return 0;
}
void sh_freeifaddrs(LxIfaddrs* p) {
  while (p) {
    LxIfaddrs* n = p->ifa_next;
    free(p);
    p = n;
  }
}

// ===========================================================================
// System information (the real FreeBSD answers, in Linux's layouts)
// ===========================================================================

struct LxUtsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
int sh_uname(LxUtsname* u) {
  struct utsname b;
  if (uname(&b) != 0) return -1;
  memset(u, 0, sizeof *u);
  snprintf(u->sysname, sizeof u->sysname, "%s", b.sysname);
  snprintf(u->nodename, sizeof u->nodename, "%s", b.nodename);
  snprintf(u->release, sizeof u->release, "%s", b.release);
  snprintf(u->version, sizeof u->version, "%s", b.version);
  // The same architecture, by the name Linux uses for it.
  snprintf(u->machine, sizeof u->machine, "%s", !strcmp(b.machine, "amd64") ? "x86_64" : b.machine);
  if (getdomainname(u->domainname, sizeof u->domainname - 1) != 0 || !u->domainname[0])
    snprintf(u->domainname, sizeof u->domainname, "(none)");
  return 0;
}

struct LxSysinfo {
  long uptime;
  unsigned long loads[3];
  unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  unsigned short procs, pad;
  unsigned long totalhigh, freehigh;
  unsigned mem_unit;
  char f[4];
};
static_assert(sizeof(LxSysinfo) == 112, "Linux x86_64 struct sysinfo is 112 bytes");
template <typename T>
T sysctl_num(const char* name) {
  T v{};
  size_t n = sizeof v;
  if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) return T{};
  return v;
}
int sh_sysinfo(LxSysinfo* s) {
  memset(s, 0, sizeof *s);
  timespec up{};
  clock_gettime(CLOCK_UPTIME, &up);
  s->uptime = up.tv_sec;
  double l[3] = {};
  if (getloadavg(l, 3) == 3)
    for (int i = 0; i < 3; ++i) s->loads[i] = static_cast<unsigned long>(l[i] * 65536.0);
  const unsigned long pg = static_cast<unsigned long>(getpagesize());
  s->totalram = sysctl_num<unsigned long>("hw.physmem");
  s->freeram = sysctl_num<unsigned>("vm.stats.vm.v_free_count") * pg;
  s->mem_unit = 1;
  return 0;
}

uint32_t g_at_random[4];
char g_exe_path[1024];
unsigned long sh_getauxval(unsigned long type) {
  switch (type) {
    case 3: case 4: case 5: case 7: case 8: case 9: {  // PHDR PHENT PHNUM BASE FLAGS ENTRY: same numbers
      unsigned long v = 0;
      if (type == 4 || type == 5 || type == 8) {
        int i = 0;
        if (elf_aux_info(static_cast<int>(type), &i, sizeof i) == 0) return static_cast<unsigned long>(i);
      } else if (elf_aux_info(static_cast<int>(type), &v, sizeof v) == 0) {
        return v;
      }
      break;
    }
    case 6: return static_cast<unsigned long>(getpagesize());
    case 11: return getuid();
    case 12: return geteuid();
    case 13: return getgid();
    case 14: return getegid();
    case 15: return reinterpret_cast<unsigned long>("x86_64");
    case 16: {  // AT_HWCAP on x86: CPUID leaf 1, EDX
      unsigned a, b, c, d;
      if (__get_cpuid(1, &a, &b, &c, &d)) return d;
      return 0;
    }
    case 17: return static_cast<unsigned long>(sysconf(_SC_CLK_TCK));
    case 23: return static_cast<unsigned long>(issetugid());
    case 25: return reinterpret_cast<unsigned long>(g_at_random);
    case 26: return 0;  // AT_HWCAP2
    case 31: return reinterpret_cast<unsigned long>(g_exe_path);
  }
  errno = ENOENT;
  return 0;
}

int sh_prctl(int op, unsigned long a2, unsigned long, unsigned long, unsigned long) {
  switch (op) {
    case 15: {  // PR_SET_NAME
      char n[16] = {};
      if (a2) strncpy(n, reinterpret_cast<const char*>(a2), sizeof n - 1);
      pthread_set_name_np(pthread_self(), n);
      return 0;
    }
    case 16: {  // PR_GET_NAME
      if (a2) pthread_get_name_np(pthread_self(), reinterpret_cast<char*>(a2), 16);
      return 0;
    }
    case 1: {  // PR_SET_PDEATHSIG
      int b = sig_or_einval(static_cast<int>(a2));
      if (b < 0) return errno = EINVAL, -1;
      return procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &b);
    }
    case 3: return 1;  // PR_GET_DUMPABLE
    case 4: return 0;  // PR_SET_DUMPABLE
    case 29: return 0; // PR_SET_TIMERSLACK
    case 38: {         // PR_SET_NO_NEW_PRIVS
      int v = PROC_NO_NEW_PRIVS_ENABLE;
      return procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, &v);
    }
  }
  note_once("prctl option %#x: not on FreeBSD", op);
  return errno = EINVAL, -1;
}

struct LxRlimit {
  uint64_t cur, max;
};
int rlimit_to_bsd(int r) {
  switch (r) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_RSS;
    case 6: return RLIMIT_NPROC;
    case 7: return RLIMIT_NOFILE;
    case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
  }
  return -1;
}
uint64_t rlim_to_lx(rlim_t v) { return v == RLIM_INFINITY ? ~0ULL : static_cast<uint64_t>(v); }
rlim_t rlim_to_bsd(uint64_t v) { return v == ~0ULL || v > static_cast<uint64_t>(RLIM_INFINITY) ? RLIM_INFINITY : static_cast<rlim_t>(v); }
int sh_prlimit(pid_t pid, int res, const LxRlimit* n, LxRlimit* o) {
  if (pid != 0 && pid != getpid()) return errno = EPERM, -1;
  const int b = rlimit_to_bsd(res);
  if (b < 0) {  // Linux-only limits (LOCKS, SIGPENDING, NICE, RTPRIO...): nothing enforces them here
    if (o) o->cur = o->max = ~0ULL;
    return 0;
  }
  rlimit r;
  if (o || n) {
    if (getrlimit(b, &r) != 0) return -1;
    if (o) o->cur = rlim_to_lx(r.rlim_cur), o->max = rlim_to_lx(r.rlim_max);
  }
  if (n) {
    r.rlim_cur = rlim_to_bsd(n->cur);
    r.rlim_max = rlim_to_bsd(n->max);
    return setrlimit(b, &r);
  }
  return 0;
}
int sh_getrlimit(int res, LxRlimit* o) { return sh_prlimit(0, res, nullptr, o); }
int sh_setrlimit(int res, const LxRlimit* n) { return sh_prlimit(0, res, n, nullptr); }

// syscall(): Linux x86_64 numbers. The common ones are carried out with
// FreeBSD's calls; anything else fails with ENOSYS (as on a kernel that lacks
// it) rather than running whatever FreeBSD has at that number.
long sh_syscall(long n, ...) {
  va_list ap;
  va_start(ap, n);
  long a[6];
  for (long& x : a) x = va_arg(ap, long);
  va_end(ap);
  auto p = [&](int i) { return reinterpret_cast<void*>(a[i]); };
  switch (n) {
    case 0: return read(static_cast<int>(a[0]), p(1), static_cast<size_t>(a[2]));
    case 1: return write(static_cast<int>(a[0]), p(1), static_cast<size_t>(a[2]));
    case 2: return sh_open(static_cast<const char*>(p(0)), static_cast<int>(a[1]), static_cast<int>(a[2]));
    case 3: return close(static_cast<int>(a[0]));
    case 9: {
      void* r = sh_mmap(p(0), static_cast<size_t>(a[1]), static_cast<int>(a[2]), static_cast<int>(a[3]),
                        static_cast<int>(a[4]), static_cast<off_t>(a[5]));
      return r == MAP_FAILED ? -1 : reinterpret_cast<long>(r);
    }
    case 10: return sh_mprotect(p(0), static_cast<size_t>(a[1]), static_cast<int>(a[2]));
    case 11: return sh_munmap(p(0), static_cast<size_t>(a[1]));
    case 13: return sh_sigaction(static_cast<int>(a[0]), static_cast<const LxSigaction*>(p(1)), static_cast<LxSigaction*>(p(2)));
    case 14: return sh_sigprocmask(static_cast<int>(a[0]), static_cast<const LxSigset*>(p(1)), static_cast<LxSigset*>(p(2)));
    case 16: return sh_ioctl(static_cast<int>(a[0]), static_cast<unsigned long>(a[1]), p(2));
    case 24: return sh_sched_yield();
    case 28: return sh_madvise(p(0), static_cast<size_t>(a[1]), static_cast<int>(a[2]));
    case 35: return nanosleep(static_cast<const timespec*>(p(0)), static_cast<timespec*>(p(1)));
    case 39: return getpid();
    case 62: return sh_kill(static_cast<pid_t>(a[0]), static_cast<int>(a[1]));
    case 72: return sh_fcntl(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2]);
    case 96: return gettimeofday(static_cast<timeval*>(p(0)), nullptr);
    case 110: return getppid();
    case 4: return sh_stat(static_cast<const char*>(p(0)), static_cast<LxStat*>(p(1)));
    case 6: return sh_lstat(static_cast<const char*>(p(0)), static_cast<LxStat*>(p(1)));
    case 21: return sh_access(static_cast<const char*>(p(0)), static_cast<int>(a[1]));
    case 89: return sh_readlink(static_cast<const char*>(p(0)), static_cast<char*>(p(1)), static_cast<size_t>(a[2]));
    case 262: return sh_fstatat(static_cast<int>(a[0]), static_cast<const char*>(p(1)), static_cast<LxStat*>(p(2)), static_cast<int>(a[3]));
    case 267: return sh_readlinkat(static_cast<int>(a[0]), static_cast<const char*>(p(1)), static_cast<char*>(p(2)), static_cast<size_t>(a[3]));
    case 137: return sh_statfs(static_cast<const char*>(p(0)), static_cast<LxStatfs*>(p(1)));
    case 138: return sh_fstatfs(static_cast<int>(a[0]), static_cast<LxStatfs*>(p(1)));
    case 131: return sh_sigaltstack(static_cast<const LxStack*>(p(0)), static_cast<LxStack*>(p(1)));
    case 157: return sh_prctl(static_cast<int>(a[0]), static_cast<unsigned long>(a[1]), 0, 0, 0);
    case 186: return pthread_getthreadid_np();
    case 200: return sh_tkill(static_cast<pid_t>(a[0]), static_cast<int>(a[1]));
    case 201: return time(static_cast<time_t*>(p(0)));
    case 202:
      return do_futex(static_cast<uint32_t*>(p(0)), static_cast<int>(a[1]), static_cast<uint32_t>(a[2]),
                      static_cast<const timespec*>(p(3)), static_cast<uint32_t*>(p(4)), static_cast<uint32_t>(a[5]));
    case 203: return sh_sched_setaffinity(static_cast<pid_t>(a[0]), static_cast<size_t>(a[1]), p(2));
    case 204: {  // the raw call returns the size copied
      const int r = sh_sched_getaffinity(static_cast<pid_t>(a[0]), static_cast<size_t>(a[1]), p(2));
      return r < 0 ? r : std::min<long>(a[1], static_cast<long>(sizeof(cpuset_t)));
    }
    case 228: return sh_clock_gettime(static_cast<clockid_t>(a[0]), static_cast<timespec*>(p(1)));
    case 229: return sh_clock_getres(static_cast<clockid_t>(a[0]), static_cast<timespec*>(p(1)));
    case 230: {
      const int r = sh_clock_nanosleep(static_cast<clockid_t>(a[0]), static_cast<int>(a[1]),
                                       static_cast<const timespec*>(p(2)), static_cast<timespec*>(p(3)));
      return r ? (errno = bsd_errno(r), -1) : 0;
    }
    case 231: _exit(static_cast<int>(a[0]));
    case 234: return sh_tgkill(static_cast<pid_t>(a[0]), static_cast<pid_t>(a[1]), static_cast<int>(a[2]));
    case 257: return sh_openat(static_cast<int>(a[0]), static_cast<const char*>(p(1)), static_cast<int>(a[2]), static_cast<int>(a[3]));
    case 302:
      return sh_prlimit(static_cast<pid_t>(a[0]), static_cast<int>(a[1]), static_cast<const LxRlimit*>(p(2)),
                        static_cast<LxRlimit*>(p(3)));
    case 309: {  // getcpu
      const int c = sched_getcpu();
      if (c < 0) return -1;
      if (p(0)) *static_cast<unsigned*>(p(0)) = static_cast<unsigned>(c);
      if (p(1)) *static_cast<unsigned*>(p(1)) = 0;
      return 0;
    }
    case 318: return getrandom(p(0), static_cast<size_t>(a[1]), static_cast<unsigned>(a[2]));
    case 319: return memfd_create(static_cast<const char*>(p(0)), static_cast<unsigned>(a[1]));
  }
  note_once("syscall %ld: not translated (ENOSYS)", n);
  return errno = ENOSYS, -1;
}

// A `syscall` instruction executed by the library itself (bsdsys.cpp hands
// it here): the same translation as syscall() above, so /proc and everything
// else answer exactly as they do through libc, plus the few calls libc's
// syscall() is never asked for. -1/errno comes back as -(Linux errno).
long raw_linux_call(long n, const long* a) {
  auto p = [&](int i) { return reinterpret_cast<void*>(a[i]); };
  switch (n) {
    case 5: return sh_fstat(static_cast<int>(a[0]), static_cast<LxStat*>(p(1)));
    case 8: return lseek(static_cast<int>(a[0]), static_cast<off_t>(a[1]), static_cast<int>(a[2]));  // SEEK_* the same
    case 17: return pread(static_cast<int>(a[0]), p(1), static_cast<size_t>(a[2]), static_cast<off_t>(a[3]));
    case 18: return pwrite(static_cast<int>(a[0]), p(1), static_cast<size_t>(a[2]), static_cast<off_t>(a[3]));
    case 60: pthread_exit(nullptr);  // exit: this thread only
    case 102: return getuid();
    case 104: return getgid();
    case 107: return geteuid();
    case 108: return getegid();
  }
  return sh_syscall(n, a[0], a[1], a[2], a[3], a[4], a[5]);
}

// --- dlopen/dlsym: bionic's RTLD_NOLOAD and RTLD_DEFAULT differ ---------------------------
using DlopenFn = void* (*)(const char*, int);
using DlsymFn = void* (*)(void*, const char*);
DlopenFn g_prev_dlopen;
DlsymFn g_prev_dlsym;
void* sh_dlopen(const char* n, int f) {
  int b = f & 3;  // RTLD_LAZY 1, RTLD_NOW 2 on both
  if (!b) b = RTLD_NOW;
  if (f & 0x100) b |= RTLD_GLOBAL;
  if (f & 0x4) b |= RTLD_NOLOAD;
  if (f & 0x1000) b |= RTLD_NODELETE;
  return g_prev_dlopen(n, b);
}
void* sh_dlsym(void* h, const char* n) { return g_prev_dlsym(h ? h : RTLD_DEFAULT, n); }  // bionic RTLD_DEFAULT is 0

// --- users -----------------------------------------------------------------------------
struct LxPasswd {
  char *pw_name, *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  char *pw_gecos, *pw_dir, *pw_shell;
};
bool copy_pw(const passwd* h, LxPasswd* o, char* buf, size_t n) {
  size_t used = 0;
  auto put = [&](const char* s) -> char* {
    const size_t l = strlen(s ? s : "") + 1;
    if (used + l > n) return nullptr;
    char* d = buf + used;
    memcpy(d, s ? s : "", l);
    used += l;
    return d;
  };
  o->pw_uid = h->pw_uid;
  o->pw_gid = h->pw_gid;
  return (o->pw_name = put(h->pw_name)) && (o->pw_passwd = put(h->pw_passwd)) && (o->pw_gecos = put(h->pw_gecos)) &&
         (o->pw_dir = put(h->pw_dir)) && (o->pw_shell = put(h->pw_shell));
}
thread_local LxPasswd t_pw;
thread_local char t_pwbuf[1024];
LxPasswd* sh_getpwuid(uid_t u) {
  passwd hp, *r = nullptr;
  char hb[1024];
  if (getpwuid_r(u, &hp, hb, sizeof hb, &r) != 0 || !r) return nullptr;
  return copy_pw(r, &t_pw, t_pwbuf, sizeof t_pwbuf) ? &t_pw : nullptr;
}
LxPasswd* sh_getpwnam(const char* nm) {
  passwd hp, *r = nullptr;
  char hb[1024];
  if (getpwnam_r(nm, &hp, hb, sizeof hb, &r) != 0 || !r) return nullptr;
  return copy_pw(r, &t_pw, t_pwbuf, sizeof t_pwbuf) ? &t_pw : nullptr;
}
int sh_getpwuid_r(uid_t u, LxPasswd* o, char* buf, size_t n, LxPasswd** res) {
  passwd hp, *r = nullptr;
  std::vector<char> hb(std::max<size_t>(n, 1024));
  *res = nullptr;
  const int e = getpwuid_r(u, &hp, hb.data(), hb.size(), &r);
  if (e) return rc(e);
  if (!r) return 0;
  if (!copy_pw(r, o, buf, n)) return 34;  // ERANGE
  *res = o;
  return 0;
}
int sh_getpwnam_r(const char* nm, LxPasswd* o, char* buf, size_t n, LxPasswd** res) {
  passwd hp, *r = nullptr;
  std::vector<char> hb(std::max<size_t>(n, 1024));
  *res = nullptr;
  const int e = getpwnam_r(nm, &hp, hb.data(), hb.size(), &r);
  if (e) return rc(e);
  if (!r) return 0;
  if (!copy_pw(r, o, buf, n)) return 34;
  *res = o;
  return 0;
}

// --- time zone variables (FreeBSD has a timezone() function instead) -------------------------
long g_timezone = 0;
int g_daylight = 0;
void sh_tzset() {
  tzset();
  const time_t now = time(nullptr);
  struct tm lt{};
  localtime_r(&now, &lt);
  g_timezone = -(lt.tm_gmtoff - (lt.tm_isdst > 0 ? 3600 : 0));
  g_daylight = tzname[1] && tzname[1][0] && strcmp(tzname[0], tzname[1]) != 0;
}

// --- locale ------------------------------------------------------------------------------
// bionic: CTYPE 0, NUMERIC 1, TIME 2, COLLATE 3, MONETARY 4, MESSAGES 5, ALL 6
// (7-12: Linux-only categories).
int lc_to_bsd(int c) {
  static const int k[] = {LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE, LC_MONETARY, LC_MESSAGES, LC_ALL};
  return c >= 0 && c <= 6 ? k[c] : -1;
}
int lc_mask_to_bsd(int m) {
  static const int k[] = {LC_CTYPE_MASK, LC_NUMERIC_MASK, LC_TIME_MASK, LC_COLLATE_MASK, LC_MONETARY_MASK, LC_MESSAGES_MASK};
  int b = 0;
  for (int i = 0; i < 6; ++i)
    if (m & (1 << i)) b |= k[i];
  return b;
}
char* sh_setlocale(int c, const char* n) {
  const int b = lc_to_bsd(c);
  if (b < 0) return const_cast<char*>("C");  // LC_PAPER and friends: nothing to set
  return setlocale(b, n);
}
locale_t sh_newlocale(int m, const char* n, locale_t base) { return newlocale(lc_mask_to_bsd(m), n, base); }
// bionic numbers nl_langinfo items from CODESET = 1; FreeBSD from 0.
char* sh_nl_langinfo(int item) {
  if (item == 1) return const_cast<char*>("UTF-8");  // what the conversions below use
  if (item >= 2 && item <= 54) return nl_langinfo(item - 1);
  if (item == 55) return nl_langinfo(CRNCYSTR);
  return const_cast<char*>("");
}
char* sh_nl_langinfo_l(int item, locale_t l) {
  if (item == 1) return const_cast<char*>("UTF-8");
  if (item >= 2 && item <= 54) return nl_langinfo_l(item - 1, l);
  if (item == 55) return nl_langinfo_l(CRNCYSTR, l);
  return const_cast<char*>("");
}

// --- multibyte text ----------------------------------------------------------------------------
// bionic's mbstate_t is 8 bytes and its conversions are UTF-8. FreeBSD's
// mbstate_t is 128 bytes, so its functions would write past the library's
// states; these are UTF-8 conversions that keep their state in 8 bytes.
struct MbState {
  uint8_t count;
  uint8_t bytes[3];
  uint16_t c16_pending;  // mbrtoc16: the low surrogate still to hand out
  uint16_t c16_high;     // c16rtomb: a high surrogate waiting for its pair
};
static_assert(sizeof(MbState) == 8, "fits bionic's mbstate_t");
constexpr size_t kIlseq = static_cast<size_t>(-1), kIncomplete = static_cast<size_t>(-2);

size_t utf8_encode(char32_t c, char* out) {
  if (c < 0x80) return out[0] = static_cast<char>(c), 1;
  if (c < 0x800) {
    out[0] = static_cast<char>(0xC0 | (c >> 6));
    out[1] = static_cast<char>(0x80 | (c & 0x3F));
    return 2;
  }
  if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return kIlseq;
  if (c < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (c >> 12));
    out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (c >> 18));
  out[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (c & 0x3F));
  return 4;
}
size_t sh_mbrtoc32(char32_t* pc, const char* s, size_t n, void* ps) {
  static thread_local MbState own;
  auto* st = ps ? static_cast<MbState*>(ps) : &own;
  if (!s) {
    s = "";
    n = 1;
    pc = nullptr;
  }
  if (n == 0) return kIncomplete;
  uint8_t b[4];
  size_t have = st->count, used = 0;
  memcpy(b, st->bytes, have);
  if (have == 0) b[have++] = static_cast<uint8_t>(s[used++]);
  const uint8_t lead = b[0];
  const size_t len = lead < 0x80 ? 1 : lead < 0xC2 ? 0 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : lead < 0xF5 ? 4 : 0;
  if (!len) {
    st->count = 0;
    return errno = EILSEQ, kIlseq;
  }
  while (have < len && used < n) {
    const uint8_t c = static_cast<uint8_t>(s[used++]);
    if ((c & 0xC0) != 0x80) {
      st->count = 0;
      return errno = EILSEQ, kIlseq;
    }
    b[have++] = c;
  }
  if (have < len) {
    st->count = static_cast<uint8_t>(have);
    memcpy(st->bytes, b, have);
    return kIncomplete;
  }
  char32_t c;
  switch (len) {
    case 1: c = lead; break;
    case 2: c = (char32_t{lead} & 0x1F) << 6 | (b[1] & 0x3F); break;
    case 3: c = (char32_t{lead} & 0x0F) << 12 | char32_t(b[1] & 0x3F) << 6 | (b[2] & 0x3F); break;
    default: c = (char32_t{lead} & 0x07) << 18 | char32_t(b[1] & 0x3F) << 12 | char32_t(b[2] & 0x3F) << 6 | (b[3] & 0x3F);
  }
  st->count = 0;
  if ((len == 3 && (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF))) || (len == 4 && (c < 0x10000 || c > 0x10FFFF)))
    return errno = EILSEQ, kIlseq;
  if (pc) *pc = c;
  return c ? used : 0;
}
size_t sh_mbrtowc(wchar_t* pwc, const char* s, size_t n, void* ps) {
  char32_t c = 0;
  const size_t r = sh_mbrtoc32(&c, s, n, ps);
  if (pwc && s && r <= 4) *pwc = static_cast<wchar_t>(c);
  return r;
}
size_t sh_mbrlen(const char* s, size_t n, void* ps) {
  static thread_local MbState own;
  return sh_mbrtowc(nullptr, s, n, ps ? ps : &own);
}
int sh_mbsinit(const void* ps) {
  const auto* st = static_cast<const MbState*>(ps);
  return !st || (st->count == 0 && st->c16_pending == 0 && st->c16_high == 0);
}
size_t sh_c32rtomb(char* s, char32_t c, void* ps) {
  char tmp[4];
  if (!s) {
    if (ps) memset(ps, 0, sizeof(MbState));
    return 1;
  }
  const size_t r = utf8_encode(c, tmp);
  if (r == kIlseq) return errno = EILSEQ, kIlseq;
  memcpy(s, tmp, r);
  return r;
}
size_t sh_wcrtomb(char* s, wchar_t wc, void* ps) {
  if (wc < 0) return s ? (errno = EILSEQ, kIlseq) : 1;
  return sh_c32rtomb(s, static_cast<char32_t>(wc), ps);
}
size_t sh_mbrtoc16(char16_t* pc, const char* s, size_t n, void* ps) {
  static thread_local MbState own;
  auto* st = ps ? static_cast<MbState*>(ps) : &own;
  if (st->c16_pending) {
    if (pc) *pc = st->c16_pending;
    st->c16_pending = 0;
    return static_cast<size_t>(-3);
  }
  char32_t c = 0;
  const size_t r = sh_mbrtoc32(&c, s, n, st);
  if (r <= 4 && s) {
    if (c >= 0x10000) {
      c -= 0x10000;
      if (pc) *pc = static_cast<char16_t>(0xD800 + (c >> 10));
      st->c16_pending = static_cast<uint16_t>(0xDC00 + (c & 0x3FF));
    } else if (pc) {
      *pc = static_cast<char16_t>(c);
    }
  }
  return r;
}
size_t sh_c16rtomb(char* s, char16_t c, void* ps) {
  static thread_local MbState own;
  auto* st = ps ? static_cast<MbState*>(ps) : &own;
  if (!s) {
    memset(st, 0, sizeof *st);
    return 1;
  }
  if (st->c16_high) {
    if (c < 0xDC00 || c > 0xDFFF) {
      st->c16_high = 0;
      return errno = EILSEQ, kIlseq;
    }
    const char32_t full = 0x10000 + ((char32_t(st->c16_high) - 0xD800) << 10) + (c - 0xDC00);
    st->c16_high = 0;
    return sh_c32rtomb(s, full, nullptr);
  }
  if (c >= 0xD800 && c <= 0xDBFF) return st->c16_high = c, 0;
  if (c >= 0xDC00 && c <= 0xDFFF) return errno = EILSEQ, kIlseq;
  return sh_c32rtomb(s, c, nullptr);
}
size_t sh_mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, void* ps) {
  static thread_local MbState own;
  if (!ps) ps = &own;
  const char* s = *src;
  size_t out = 0, left = nms;
  while ((!dst || out < len) && left) {
    wchar_t wc = 0;
    const size_t r = sh_mbrtowc(&wc, s, left, ps);
    if (r == kIlseq) {
      if (dst) *src = s;
      return kIlseq;
    }
    if (r == kIncomplete) {
      s += left;
      left = 0;
      break;
    }
    if (r == 0) {
      if (dst) {
        dst[out] = 0;
        *src = nullptr;
      }
      return out;
    }
    if (dst) dst[out] = wc;
    ++out;
    s += r;
    left -= r;
  }
  if (dst) *src = s;
  return out;
}
size_t sh_mbsrtowcs(wchar_t* dst, const char** src, size_t len, void* ps) {
  return sh_mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}
size_t sh_wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, void*) {
  const wchar_t* s = *src;
  size_t out = 0;
  for (size_t i = 0; i < nwc; ++i) {
    const wchar_t wc = s[i];
    if (wc == 0) {
      if (dst) {
        if (out >= len) {
          *src = s + i;
          return out;
        }
        dst[out] = 0;
        *src = nullptr;
      }
      return out;
    }
    char b[4];
    const size_t r = wc < 0 ? kIlseq : utf8_encode(static_cast<char32_t>(wc), b);
    if (r == kIlseq) {
      if (dst) *src = s + i;
      return errno = EILSEQ, kIlseq;
    }
    if (dst) {
      if (out + r > len) {
        *src = s + i;
        return out;
      }
      memcpy(dst + out, b, r);
    }
    out += r;
  }
  if (dst) *src = s + nwc;
  return out;
}
size_t sh_wcsrtombs(char* dst, const wchar_t** src, size_t len, void* ps) {
  return sh_wcsnrtombs(dst, src, SIZE_MAX, len, ps);
}
int sh_mbtowc(wchar_t* pwc, const char* s, size_t n) {
  static thread_local MbState st;
  if (!s) return memset(&st, 0, sizeof st), 0;
  const size_t r = sh_mbrtowc(pwc, s, n, &st);
  if (r == kIlseq || r == kIncomplete) {
    memset(&st, 0, sizeof st);
    return errno = EILSEQ, -1;
  }
  return static_cast<int>(r);
}
int sh_mblen(const char* s, size_t n) { return sh_mbtowc(nullptr, s, n); }
int sh_wctomb(char* s, wchar_t wc) {
  if (!s) return 0;
  const size_t r = sh_wcrtomb(s, wc, nullptr);
  return r == kIlseq ? -1 : static_cast<int>(r);
}
size_t sh_mbstowcs(wchar_t* d, const char* s, size_t n) {
  MbState st{};
  return sh_mbsrtowcs(d, &s, n, &st);
}
size_t sh_wcstombs(char* d, const wchar_t* s, size_t n) { return sh_wcsrtombs(d, &s, n, nullptr); }
wint_t sh_btowc(int c) { return c == EOF || c < 0 || c >= 0x80 ? WEOF : static_cast<wint_t>(c); }
int sh_wctob(wint_t c) { return c < 0x80 ? static_cast<int>(c) : EOF; }
size_t sh_mb_cur_max() { return 4; }

// --- _FORTIFY_SOURCE entry points glibc has and FreeBSD may not ------------------------------
void* sh_memcpy_chk(void* d, const void* s, size_t n, size_t) { return memcpy(d, s, n); }
void* sh_memmove_chk(void* d, const void* s, size_t n, size_t) { return memmove(d, s, n); }
void* sh_memset_chk(void* d, int c, size_t n, size_t) { return memset(d, c, n); }
char* sh_strcpy_chk(char* d, const char* s, size_t) { return strcpy(d, s); }
char* sh_stpcpy_chk(char* d, const char* s, size_t) { return stpcpy(d, s); }
char* sh_strncpy_chk(char* d, const char* s, size_t n, size_t) { return strncpy(d, s, n); }
char* sh_stpncpy_chk(char* d, const char* s, size_t n, size_t) { return stpncpy(d, s, n); }
char* sh_strcat_chk(char* d, const char* s, size_t) { return strcat(d, s); }
char* sh_strncat_chk(char* d, const char* s, size_t n, size_t) { return strncat(d, s, n); }
size_t sh_strlcpy_chk(char* d, const char* s, size_t n, size_t) { return strlcpy(d, s, n); }
size_t sh_strlcat_chk(char* d, const char* s, size_t n, size_t) { return strlcat(d, s, n); }
char* sh_strrchr_chk(const char* s, int c, size_t) { return const_cast<char*>(strrchr(s, c)); }
void* sh_memchr_chk(const void* s, int c, size_t n, size_t) { return const_cast<void*>(memchr(s, c, n)); }
void* sh_memrchr_chk(const void* s, int c, size_t n, size_t) { return const_cast<void*>(memrchr(s, c, n)); }
// FreeBSD's vsnprintf refuses sizes over INT_MAX ("unknown" is SIZE_MAX).
int sh_vsprintf_chk(char* b, int, size_t sl, const char* f, va_list ap) {
  return sl > INT_MAX ? vsprintf(b, f, ap) : vsnprintf(b, sl, f, ap);
}
int sh_sprintf_chk(char* b, int fl, size_t sl, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  const int r = sh_vsprintf_chk(b, fl, sl, f, ap);
  va_end(ap);
  return r;
}
int sh_vsnprintf_chk(char* b, size_t n, int, size_t, const char* f, va_list ap) {
  return vsnprintf(b, std::min<size_t>(n, INT_MAX), f, ap);
}
int sh_snprintf_chk(char* b, size_t n, int fl, size_t sl, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  const int r = sh_vsnprintf_chk(b, n, fl, sl, f, ap);
  va_end(ap);
  return r;
}
char* sh_fgets_chk(char* b, int n, FILE* f, size_t) { return fgets(b, n, f); }
size_t sh_fread_chk(void* b, size_t sz, size_t n, FILE* f, size_t) { return fread(b, sz, n, f); }
ssize_t sh_read_chk(int fd, void* b, size_t n, size_t) { return read(fd, b, n); }
ssize_t sh_pread_chk(int fd, void* b, size_t n, off_t o, size_t) { return pread(fd, b, n, o); }
ssize_t sh_pwrite_chk(int fd, const void* b, size_t n, off_t o, size_t) { return pwrite(fd, b, n, o); }
ssize_t sh_readlink_chk(const char* p, char* b, size_t n, size_t) { return sh_readlink(p, b, n); }
ssize_t sh_readlinkat_chk(int d, const char* p, char* b, size_t n, size_t) { return sh_readlinkat(d, p, b, n); }
char* sh_getcwd_chk(char* b, size_t n, size_t) { return getcwd(b, n); }
ssize_t sh_recvfrom_chk(int fd, void* b, size_t n, size_t, int f, void* a, socklen_t* l) { return sh_recvfrom(fd, b, n, f, a, l); }
ssize_t sh_sendto_chk(int fd, const void* b, size_t n, size_t, int f, const void* a, socklen_t l) {
  return sh_sendto(fd, b, n, f, a, l);
}
int sh_poll_chk(pollfd* fds, nfds_t n, int t, size_t) { return sh_poll(fds, n, t); }
int sh_ppoll_chk(pollfd* fds, nfds_t n, const timespec* ts, const LxSigset* m, size_t) { return sh_ppoll(fds, n, ts, m); }

// GNU basename never modifies its argument.
const char* sh_gnu_basename(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}
void* sh_rawmemchr(const void* s, int c) {
  const auto* p = static_cast<const unsigned char*>(s);
  while (*p != static_cast<unsigned char>(c)) ++p;
  return const_cast<unsigned char*>(p);
}


// ===========================================================================
// More of /proc, as Linux shows it (what FreeBSD's linprocfs gives a Linux
// program too). Every figure is FreeBSD's real one for this process or this
// machine; where FreeBSD has no such thing (OOM score, capabilities) the value
// is what Linux shows an ordinary unprivileged process. Seen in the log as the
// files Roblox reads during play (status, stat, auxv, cpuinfo, cmdline, fd).
// ===========================================================================

bool kinfo_of(pid_t pid, kinfo_proc& kp) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  size_t n = sizeof kp;
  memset(&kp, 0, sizeof kp);
  return sysctl(mib, 4, &kp, &n, nullptr, 0) == 0 && n >= sizeof kp;
}
// "/proc/<digits>/NAME" -> pid, or -1.
pid_t pid_path(const char* p, const char* name) {
  if (!p || strncmp(p, "/proc/", 6) != 0) return -1;
  const char* d = p + 6;
  char* end = nullptr;
  const long v = strtol(d, &end, 10);
  if (end == d || *end != '/' || strcmp(end + 1, name) != 0 || v < 0) return -1;
  return static_cast<pid_t>(v);
}
char lx_state(int stat) {
  switch (stat) {
    case 2: case 1: return 'R';  // SRUN, SIDL (sys/proc.h numbers)
    case 4: return 'T';  // SSTOP
    case 5: return 'Z';  // SZOMB
    case 7: return 'D';  // SLOCK
    default: return 'S';
  }
}
const char* lx_state_name(char c) {
  switch (c) {
    case 'R': return "R (running)";
    case 'T': return "T (stopped)";
    case 'Z': return "Z (zombie)";
    case 'D': return "D (disk sleep)";
    default: return "S (sleeping)";
  }
}
long clk_tck() { return sysconf(_SC_CLK_TCK); }
unsigned long long tv_ticks(const timeval& t) {
  return static_cast<unsigned long long>(t.tv_sec) * clk_tck() + static_cast<unsigned long long>(t.tv_usec) * clk_tck() / 1000000;
}
int tracer_pid() {
  int t = -1;
  if (procctl(P_PID, getpid(), PROC_TRACE_STATUS, &t) != 0) return 0;
  return t < 0 ? 0 : t;
}

bool proc_cmdline(pid_t pid, std::string& text) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, pid};
  size_t n = 0;
  if (sysctl(mib, 4, nullptr, &n, nullptr, 0) != 0) return false;
  text.resize(n + 1);
  if (sysctl(mib, 4, &text[0], &n, nullptr, 0) != 0) return false;
  text.resize(n);  // NUL-separated, as Linux's
  return true;
}

bool proc_self_stat(std::string& text) {
  kinfo_proc kp;
  if (!kinfo_of(getpid(), kp)) return false;
  timeval boot{};
  size_t bl = sizeof boot;
  sysctlbyname("kern.boottime", &boot, &bl, nullptr, 0);
  timeval since{kp.ki_start.tv_sec - boot.tv_sec, kp.ki_start.tv_usec - boot.tv_usec};
  if (since.tv_usec < 0) since.tv_sec -= 1, since.tv_usec += 1000000;
  const LxSigset pend = set_to_lx(&kp.ki_siglist), blk = set_to_lx(&kp.ki_sigmask),
                 ign = set_to_lx(&kp.ki_sigignore), cgt = set_to_lx(&kp.ki_sigcatch);
  char b[1024];
  snprintf(b, sizeof b,
           "%d (%s) %c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu %llu %llu %d %d %d 0 %llu %llu %ld "
           "18446744073709551615 0 0 0 0 0 %llu %llu %llu %llu 0 0 0 17 %d 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
           kp.ki_pid, kp.ki_comm, lx_state(kp.ki_stat), kp.ki_ppid, kp.ki_pgid, kp.ki_sid, 0, kp.ki_tpgid, 0u,
           static_cast<unsigned long>(kp.ki_rusage.ru_minflt), static_cast<unsigned long>(kp.ki_rusage_ch.ru_minflt),
           static_cast<unsigned long>(kp.ki_rusage.ru_majflt), static_cast<unsigned long>(kp.ki_rusage_ch.ru_majflt),
           tv_ticks(kp.ki_rusage.ru_utime), tv_ticks(kp.ki_rusage.ru_stime), tv_ticks(kp.ki_rusage_ch.ru_utime),
           tv_ticks(kp.ki_rusage_ch.ru_stime), 20 + kp.ki_nice, static_cast<int>(kp.ki_nice), kp.ki_numthreads,
           tv_ticks(since), static_cast<unsigned long long>(kp.ki_size), static_cast<long>(kp.ki_rssize),
           static_cast<unsigned long long>(pend), static_cast<unsigned long long>(blk),
           static_cast<unsigned long long>(ign), static_cast<unsigned long long>(cgt), kp.ki_lastcpu);
  text = b;
  return true;
}

bool proc_self_status(std::string& text) {
  kinfo_proc kp;
  if (!kinfo_of(getpid(), kp)) return false;
  const uint64_t pg = static_cast<uint64_t>(getpagesize());
  unsigned um = 022;
#ifdef KERN_PROC_UMASK
  {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_UMASK, getpid()};
    u_short u = 0;
    size_t n = sizeof u;
    if (sysctl(mib, 4, &u, &n, nullptr, 0) == 0) um = u;
  }
#endif
  std::string groups;
  for (int i = 0; i < kp.ki_ngroups && i < KI_NGROUPS; ++i) groups += std::to_string(kp.ki_groups[i]) + ' ';
  const char st = lx_state(kp.ki_stat);
  const unsigned long long vm = kp.ki_size / 1024, rss = kp.ki_rssize * pg / 1024;
  const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  char b[2048];
  snprintf(b, sizeof b,
           "Name:\t%.15s\nUmask:\t%04o\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t%d\n"
           "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t256\nGroups:\t%s\n"
           "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t%8llu kB\n"
           "VmRSS:\t%8llu kB\nVmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmExe:\t%8llu kB\nVmLib:\t       0 kB\n"
           "VmPTE:\t       0 kB\nVmSwap:\t       0 kB\nThreads:\t%d\nSigQ:\t0/%ld\n"
           "SigPnd:\t0000000000000000\nShdPnd:\t%016llx\nSigBlk:\t%016llx\nSigIgn:\t%016llx\nSigCgt:\t%016llx\n"
           "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n"
           "CapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\n"
           "Cpus_allowed_list:\t0-%ld\nvoluntary_ctxt_switches:\t%ld\nnonvoluntary_ctxt_switches:\t%ld\n",
           kp.ki_comm, um, lx_state_name(st), kp.ki_pid, kp.ki_pid, kp.ki_ppid, tracer_pid(),
           static_cast<unsigned>(kp.ki_ruid), static_cast<unsigned>(kp.ki_uid), static_cast<unsigned>(kp.ki_svuid),
           static_cast<unsigned>(kp.ki_uid), static_cast<unsigned>(kp.ki_rgid), static_cast<unsigned>(kp.ki_groups[0]),
           static_cast<unsigned>(kp.ki_svgid), static_cast<unsigned>(kp.ki_groups[0]), groups.c_str(), vm, vm, rss, rss,
           static_cast<unsigned long long>(kp.ki_dsize * pg / 1024),
           static_cast<unsigned long long>(kp.ki_ssize * pg / 1024),
           static_cast<unsigned long long>(kp.ki_tsize * pg / 1024), kp.ki_numthreads,
           static_cast<long>(sysconf(_SC_CHILD_MAX)), static_cast<unsigned long long>(set_to_lx(&kp.ki_siglist)),
           static_cast<unsigned long long>(set_to_lx(&kp.ki_sigmask)),
           static_cast<unsigned long long>(set_to_lx(&kp.ki_sigignore)),
           static_cast<unsigned long long>(set_to_lx(&kp.ki_sigcatch)), ncpu > 0 ? ncpu - 1 : 0,
           static_cast<long>(kp.ki_rusage.ru_nvcsw), static_cast<long>(kp.ki_rusage.ru_nivcsw));
  text = b;
  return true;
}

// The same values getauxval gives the library, as (type, value) pairs.
bool proc_self_auxv(std::string& text) {
  static const unsigned long kTypes[] = {3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 23, 25, 26, 31};
  for (unsigned long t : kTypes) {
    errno = 0;
    const unsigned long v = sh_getauxval(t);
    if (errno == ENOENT) continue;
    const uint64_t pair[2] = {t, v};
    text.append(reinterpret_cast<const char*>(pair), sizeof pair);
  }
  const uint64_t end[2] = {0, 0};
  text.append(reinterpret_cast<const char*>(end), sizeof end);
  return true;
}

// /proc/cpuinfo from CPUID, in Linux's x86 layout.
bool proc_cpuinfo(std::string& text) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0, &a, &b, &c, &d)) return false;
  const unsigned max_leaf = a;
  char vendor[13] = {};
  memcpy(vendor, &b, 4), memcpy(vendor + 4, &d, 4), memcpy(vendor + 8, &c, 4);
  unsigned e1a = 0, e1b = 0, e1c = 0, e1d = 0;
  __get_cpuid(1, &e1a, &e1b, &e1c, &e1d);
  unsigned family = (e1a >> 8) & 0xf, model = (e1a >> 4) & 0xf;
  if (family == 0xf) family += (e1a >> 20) & 0xff;
  if (family >= 6) model += ((e1a >> 16) & 0xf) << 4;
  unsigned l7b = 0, l7c = 0, l7d = 0, x = 0;
  if (max_leaf >= 7) __get_cpuid_count(7, 0, &x, &l7b, &l7c, &l7d);
  unsigned xmax = 0, x1c = 0, x1d = 0, xa = 0, xb = 0, phys = 36, virt = 48;
  __get_cpuid(0x80000000, &xmax, &xa, &xb, &x);
  if (xmax >= 0x80000001) __get_cpuid(0x80000001, &x, &x, &x1c, &x1d);
  if (xmax >= 0x80000008 && __get_cpuid(0x80000008, &xa, &x, &x, &x)) phys = xa & 0xff, virt = (xa >> 8) & 0xff;
  struct Flag { unsigned* reg; int bit; const char* name; };
  const Flag flags[] = {
      {&e1d, 0, "fpu"},     {&e1d, 1, "vme"},      {&e1d, 2, "de"},        {&e1d, 3, "pse"},      {&e1d, 4, "tsc"},
      {&e1d, 5, "msr"},     {&e1d, 6, "pae"},      {&e1d, 7, "mce"},       {&e1d, 8, "cx8"},      {&e1d, 9, "apic"},
      {&e1d, 11, "sep"},    {&e1d, 12, "mtrr"},    {&e1d, 13, "pge"},      {&e1d, 14, "mca"},     {&e1d, 15, "cmov"},
      {&e1d, 16, "pat"},    {&e1d, 17, "pse36"},   {&e1d, 19, "clflush"},  {&e1d, 23, "mmx"},     {&e1d, 24, "fxsr"},
      {&e1d, 25, "sse"},    {&e1d, 26, "sse2"},    {&e1d, 28, "ht"},       {&x1d, 11, "syscall"}, {&x1d, 20, "nx"},
      {&x1d, 26, "pdpe1gb"}, {&x1d, 27, "rdtscp"}, {&x1d, 29, "lm"},       {&e1c, 0, "pni"},      {&e1c, 1, "pclmulqdq"},
      {&e1c, 9, "ssse3"},   {&e1c, 12, "fma"},     {&e1c, 13, "cx16"},     {&e1c, 19, "sse4_1"},  {&e1c, 20, "sse4_2"},
      {&e1c, 21, "x2apic"}, {&e1c, 22, "movbe"},   {&e1c, 23, "popcnt"},   {&e1c, 25, "aes"},     {&e1c, 26, "xsave"},
      {&e1c, 28, "avx"},    {&e1c, 29, "f16c"},    {&e1c, 30, "rdrand"},   {&e1c, 31, "hypervisor"},
      {&x1c, 0, "lahf_lm"}, {&x1c, 5, "abm"},      {&x1c, 8, "3dnowprefetch"},
      {&l7b, 0, "fsgsbase"}, {&l7b, 3, "bmi1"},    {&l7b, 5, "avx2"},      {&l7b, 7, "smep"},     {&l7b, 8, "bmi2"},
      {&l7b, 9, "erms"},    {&l7b, 10, "invpcid"}, {&l7b, 16, "avx512f"},  {&l7b, 18, "rdseed"},  {&l7b, 19, "adx"},
      {&l7b, 20, "smap"},   {&l7b, 23, "clflushopt"}, {&l7b, 29, "sha_ni"}, {&l7c, 9, "vaes"},    {&l7c, 10, "vpclmulqdq"},
  };
  std::string fl;
  for (const auto& f : flags)
    if (*f.reg & (1u << f.bit)) fl += (fl.empty() ? "" : " "), fl += f.name;
  char name[256] = {};
  size_t nl = sizeof name - 1;
  if (sysctlbyname("hw.model", name, &nl, nullptr, 0) != 0) snprintf(name, sizeof name, "%s", vendor);
  int mhz = 0;
  size_t ml = sizeof mhz;
  sysctlbyname("hw.clockrate", &mhz, &ml, nullptr, 0);
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  for (long i = 0; i < n; ++i) {
    char blk[768];
    snprintf(blk, sizeof blk,
             "processor\t: %ld\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n"
             "cpu MHz\t\t: %d.000\nphysical id\t: 0\nsiblings\t: %ld\ncore id\t\t: %ld\ncpu cores\t: %ld\n"
             "apicid\t\t: %ld\ninitial apicid\t: %ld\nfpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\n"
             "flags\t\t: ",
             i, vendor, family, model, name, e1a & 0xf, mhz, n, i, n, i, i, max_leaf);
    text += blk;
    text += fl;
    snprintf(blk, sizeof blk,
             "\nbogomips\t: %d.00\nclflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: %u bits physical, %u bits "
             "virtual\npower management:\n\n",
             mhz * 2, phys, virt);
    text += blk;
  }
  return true;
}

// The files above (and other processes' cmdline), for proc_file.
bool proc_more_path(const char* p) {
  if (!p) return false;
  for (const char* n : {"status", "stat", "auxv", "oom_score", "oom_score_adj", "oom_adj", "cmdline", "comm"})
    if (is_self(p, n)) return true;
  return !strcmp(p, "/proc/cpuinfo") || !strcmp(p, "/proc/sys/vm/overcommit_memory") || pid_path(p, "cmdline") >= 0;
}
// false with errno set when the file cannot be made (e.g. that process is gone).
bool proc_more(const char* p, std::string& text) {
  if (is_self(p, "status")) return proc_self_status(text) || (errno = EACCES, false);
  if (is_self(p, "stat")) return proc_self_stat(text) || (errno = EACCES, false);
  if (is_self(p, "auxv")) return proc_self_auxv(text);
  if (is_self(p, "oom_score") || is_self(p, "oom_score_adj") || is_self(p, "oom_adj")) return text = "0\n", true;
  if (is_self(p, "comm")) {
    kinfo_proc kp;
    if (!kinfo_of(getpid(), kp)) return errno = EACCES, false;
    text = std::string(kp.ki_comm).substr(0, 15) + '\n';
    return true;
  }
  if (is_self(p, "cmdline")) return proc_cmdline(getpid(), text) || (errno = EACCES, false);
  if (!strcmp(p, "/proc/cpuinfo")) return proc_cpuinfo(text) || (errno = EACCES, false);
  if (!strcmp(p, "/proc/sys/vm/overcommit_memory")) {
    int oc = 0;  // FreeBSD's vm.overcommit bit 0: strict swap accounting (Linux mode 2)
    size_t n = sizeof oc;
    sysctlbyname("vm.overcommit", &oc, &n, nullptr, 0);
    text = (oc & 1) ? "2\n" : "0\n";
    return true;
  }
  const pid_t pid = pid_path(p, "cmdline");
  if (pid >= 0) {
    if (proc_cmdline(pid, text)) return true;
    return errno = ENOENT, false;  // (no such process, as Linux)
  }
  return errno = ENOENT, false;
}

// /proc/self/fd: FreeBSD keeps it at /dev/fd (fdescfs). The directory and its
// entries are opened there; readlink on an entry says what the descriptor is,
// in Linux's words ("/path", "socket:[n]", "pipe:[n]", "anon_inode:[...]").
const char* proc_fd_redirect(const char* p, char* buf, size_t n) {
  if (is_self(p, "fd") || is_self(p, "fd/")) return "/dev/fd";
  for (const char* pre : {"/proc/self/fd/", "/proc/thread-self/fd/"})
    if (p && !strncmp(p, pre, strlen(pre))) return snprintf(buf, n, "/dev/fd/%s", p + strlen(pre)), buf;
  char mine[48];
  const int l = snprintf(mine, sizeof mine, "/proc/%d/fd/", static_cast<int>(getpid()));
  if (p && !strncmp(p, mine, static_cast<size_t>(l))) return snprintf(buf, n, "/dev/fd/%s", p + l), buf;
  return nullptr;
}
void fdescfs_check() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct statfs s;
    if (statfs("/dev/fd", &s) != 0 || strcmp(s.f_fstypename, "fdescfs") != 0)
      fprintf(stderr, "[bsdabi] /proc/self/fd: /dev/fd is not fdescfs here, so only 0-2 are listed "
                      "(mount -t fdescfs fdesc /dev/fd)\n");
  });
}
ssize_t proc_fd_link(const char* p, char* b, size_t n, bool* handled) {
  char buf[64];
  const char* r = proc_fd_redirect(p, buf, sizeof buf);
  *handled = r && strncmp(r, "/dev/fd/", 8) == 0;
  if (!*handled) return -1;
  char* end = nullptr;
  const long fd = strtol(r + 8, &end, 10);
  if (end == r + 8 || *end) return errno = ENOENT, -1;
  struct stat st;
  if (fstat(static_cast<int>(fd), &st) != 0) return errno = ENOENT, -1;
  std::string out;
#ifdef F_KINFO
  kinfo_file kf;
  memset(&kf, 0, sizeof kf);
  kf.kf_structsize = KINFO_FILE_SIZE;
  if (fcntl(static_cast<int>(fd), F_KINFO, &kf) == 0) {
    switch (kf.kf_type) {
      case KF_TYPE_VNODE: out = kf.kf_path[0] ? kf.kf_path : ""; break;
      case KF_TYPE_SOCKET: out = "socket:[" + std::to_string(st.st_ino) + "]"; break;
      case KF_TYPE_PIPE: case KF_TYPE_FIFO: out = "pipe:[" + std::to_string(st.st_ino) + "]"; break;
      case KF_TYPE_KQUEUE: out = "anon_inode:[eventpoll]"; break;  // (epoll-shim's epoll is a kqueue)
#ifdef KF_TYPE_EVENTFD
      case KF_TYPE_EVENTFD: out = "anon_inode:[eventfd]"; break;
#endif
#ifdef KF_TYPE_TIMERFD
      case KF_TYPE_TIMERFD: out = "anon_inode:[timerfd]"; break;
#endif
      case KF_TYPE_SHM: out = kf.kf_path[0] ? std::string("/memfd:") + kf.kf_path + " (deleted)" : "/dev/shm/?"; break;
      default: break;
    }
  }
#endif
  if (out.empty()) {
    if (S_ISSOCK(st.st_mode)) out = "socket:[" + std::to_string(st.st_ino) + "]";
    else if (S_ISFIFO(st.st_mode)) out = "pipe:[" + std::to_string(st.st_ino) + "]";
    else out = "anon_inode:[" + std::to_string(st.st_ino) + "]";
  }
  const size_t c = std::min(out.size(), n);
  memcpy(b, out.data(), c);
  return static_cast<ssize_t>(c);
}

}  // namespace

void init_bsd_abi() {
  // Mesa's GL threading: the driver's share of each GL call runs on a thread
  // of its own, so Roblox's render thread gets to the next frame sooner. In
  // the VirtualBox VM (llvmpipe) it was 14.8 against 10.6 fps averaged over 3
  // minutes in the same game. Before the window opens, which is when Mesa reads
  // it. mesa_glthread=false turns it off again.
  setenv("mesa_glthread", "true", 0);
  init_errno_maps();
  init_signal_maps();
  arc4random_buf(g_at_random, sizeof g_at_random);
  const std::string exe = [] {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char b[1024];
    size_t n = sizeof b;
    return sysctl(mib, 4, b, &n, nullptr, 0) == 0 ? std::string(b, strnlen(b, n)) : std::string();
  }();
  snprintf(g_exe_path, sizeof g_exe_path, "%s", exe.c_str());
  sh_tzset();

  // Wrapped (registered by shims.cpp / vkshim.cpp); the originals stay in front.
  g_prev_gai = lookup_prev<GaiFn>("getaddrinfo", nullptr);
  g_prev_gni = lookup_prev<GniFn>("getnameinfo", nullptr);
  g_prev_dlopen = lookup_prev<DlopenFn>("dlopen", &dlopen);
  g_prev_dlsym = lookup_prev<DlsymFn>("dlsym", &dlsym);
  if (g_prev_gai) reg("getaddrinfo", &sh_getaddrinfo);
  if (g_prev_gni) reg("getnameinfo", &sh_getnameinfo);
  reg("dlopen", &sh_dlopen);
  reg("dlsym", &sh_dlsym);
  reg("android_dlopen_ext", &sh_enosys);

  // errno
  reg("__errno", &sh_errno);
  reg("strerror", &sh_strerror);
  reg("strerror_l", &sh_strerror_l);
  reg("strerror_r", &sh_strerror_r);
  reg("__gnu_strerror_r", &sh_gnu_strerror_r);
  reg("perror", &sh_perror);
  reg("__get_h_errno", &sh_get_h_errno);

  // stdio's variables (FreeBSD names them __stdinp and so on)
  register_shim("stdin", &__stdinp);
  register_shim("stdout", &__stdoutp);
  register_shim("stderr", &__stderrp);
  register_shim("timezone", &g_timezone);
  register_shim("daylight", &g_daylight);
  reg("tzset", &sh_tzset);

  // files
  reg("open", &sh_open);
  reg("open64", &sh_open);
  reg("openat", &sh_openat);
  reg("openat64", &sh_openat);
  reg("__open_2", &sh_open_2);
  reg("__openat_2", &sh_openat_2);
  reg("fcntl", &sh_fcntl);
  reg("fcntl64", &sh_fcntl);
  reg("pipe2", &sh_pipe2);
  reg("dup3", &sh_dup3);
  reg("mkostemp", &sh_mkostemp);
  reg("mkostemp64", &sh_mkostemp);
  reg("mkostemps", &sh_mkostemps);
  reg("eventfd", &sh_eventfd);
  reg("timerfd_create", &sh_timerfd_create);
  reg("timerfd_settime", &sh_timerfd_settime);
  reg("epoll_create1", &sh_epoll_create1);
  reg("epoll_pwait", &sh_epoll_pwait);
  for (const char* n : {"stat", "stat64"}) reg(n, &sh_stat);
  for (const char* n : {"lstat", "lstat64"}) reg(n, &sh_lstat);
  for (const char* n : {"fstat", "fstat64"}) reg(n, &sh_fstat);
  for (const char* n : {"fstatat", "fstatat64"}) reg(n, &sh_fstatat);
  for (const char* n : {"statfs", "statfs64"}) reg(n, &sh_statfs);
  for (const char* n : {"fstatfs", "fstatfs64"}) reg(n, &sh_fstatfs);
  for (const char* n : {"statvfs", "statvfs64"}) reg(n, &sh_statvfs);
  for (const char* n : {"fstatvfs", "fstatvfs64"}) reg(n, &sh_fstatvfs);
  for (const char* n : {"readdir", "readdir64"}) reg(n, &sh_readdir);
  for (const char* n : {"readdir_r", "readdir64_r"}) reg(n, &sh_readdir_r);
  for (const char* n : {"scandir", "scandir64"}) reg(n, &sh_scandir);
  for (const char* n : {"alphasort", "alphasort64"}) reg(n, &sh_alphasort);
  reg("closedir", &sh_closedir);
  reg("faccessat", &sh_faccessat);
  reg("access", &sh_access);
  reg("readlink", &sh_readlink);
  reg("readlinkat", &sh_readlinkat);
  reg("opendir", &sh_opendir);
  reg("unlinkat", &sh_unlinkat);
  reg("fchmodat", &sh_fchmodat);
  reg("fchownat", &sh_fchownat);
  reg("linkat", &sh_linkat);
  reg("renameat2", &sh_renameat2);
  reg("utimensat", &sh_utimensat);
  reg("futimens", &sh_futimens);
  for (const char* n : {"sendfile", "sendfile64"}) reg(n, &sh_sendfile);
  for (const char* n : {"fallocate", "fallocate64"}) reg(n, &sh_fallocate);
  // The *64 names glibc and bionic export for calls that are the same here.
  reg("lseek64", &lseek);
  reg("pread64", &pread);
  reg("pwrite64", &pwrite);
  reg("preadv64", &preadv);
  reg("pwritev64", &pwritev);
  reg("ftruncate64", &ftruncate);
  reg("truncate64", &truncate);
  reg("creat64", &creat);
  reg("lockf64", &lockf);
  reg("mkstemp64", &mkstemp);
  reg("fopen", &sh_fopen);
  reg("fopen64", &sh_fopen);
  reg("freopen64", &freopen);
  reg("fseeko64", &fseeko);
  reg("ftello64", &ftello);
  reg("fgetpos64", &fgetpos);
  reg("fsetpos64", &fsetpos);
  reg("tmpfile64", &tmpfile);
  REG_RC(posix_fadvise);
  reg("posix_fadvise64", &Rc<decltype(&::posix_fadvise), &::posix_fadvise>::call);
  reg("posix_fallocate", &sh_posix_fallocate);
  reg("posix_fallocate64", &sh_posix_fallocate);
  // Linux-only calls: fail as on a kernel without them.
  for (const char* n : {"inotify_init", "inotify_init1", "inotify_add_watch", "inotify_rm_watch", "signalfd", "statx",
                        "splice", "tee", "vmsplice", "process_vm_readv", "process_vm_writev", "pidfd_open",
                        "fanotify_init", "fanotify_mark", "name_to_handle_at", "open_by_handle_at", "unshare", "setns",
                        "capget", "capset", "getdents64"})
    reg(n, &sh_enosys);

  // memory
  reg("mmap", &sh_mmap);
  reg("mmap64", &sh_mmap);
  reg("mprotect", &sh_mprotect);
  reg("munmap", &sh_munmap);
  reg("madvise", &sh_madvise);
  reg("posix_madvise", &sh_posix_madvise);
  reg("msync", &sh_msync);
  reg("mlockall", &sh_mlockall);
  reg("mlock2", &sh_mlock2);
  reg("mremap", &sh_mremap);
  reg("mallinfo", &sh_mallinfo);
  reg("mallinfo2", &sh_mallinfo);
  reg("memalign", &sh_memalign);
  reg("pvalloc", &sh_pvalloc);
  REG_RC(posix_memalign);

  // time
  reg("clock_gettime", &sh_clock_gettime);
  reg("clock_getres", &sh_clock_getres);
  reg("clock_settime", &sh_clock_settime);
  reg("clock_nanosleep", &sh_clock_nanosleep);

  // signals
  for (const char* n : {"sigaction", "sigaction64"}) reg(n, &sh_sigaction);
  for (const char* n : {"signal", "bsd_signal"}) reg(n, &sh_signal);
  reg("sysv_signal", &sh_sysv_signal);
  for (const char* n : {"sigprocmask", "sigprocmask64"}) reg(n, &sh_sigprocmask);
  for (const char* n : {"pthread_sigmask", "pthread_sigmask64"}) reg(n, &sh_pthread_sigmask);
  for (const char* n : {"sigaddset", "sigaddset64"}) reg(n, &sh_sigaddset);
  for (const char* n : {"sigdelset", "sigdelset64"}) reg(n, &sh_sigdelset);
  for (const char* n : {"sigismember", "sigismember64"}) reg(n, &sh_sigismember);
  for (const char* n : {"sigemptyset", "sigemptyset64"}) reg(n, &sh_sigemptyset);
  for (const char* n : {"sigfillset", "sigfillset64"}) reg(n, &sh_sigfillset);
  reg("sigisemptyset", &sh_sigisemptyset);
  reg("sigorset", &sh_sigorset);
  reg("sigandset", &sh_sigandset);
  for (const char* n : {"sigsuspend", "sigsuspend64"}) reg(n, &sh_sigsuspend);
  for (const char* n : {"sigpending", "sigpending64"}) reg(n, &sh_sigpending);
  for (const char* n : {"sigwait", "sigwait64"}) reg(n, &sh_sigwait);
  for (const char* n : {"sigwaitinfo", "sigwaitinfo64"}) reg(n, &sh_sigwaitinfo);
  for (const char* n : {"sigtimedwait", "sigtimedwait64"}) reg(n, &sh_sigtimedwait);
  reg("kill", &sh_kill);
  reg("killpg", &sh_killpg);
  reg("raise", &sh_raise);
  reg("pthread_kill", &sh_pthread_kill);
  reg("tgkill", &sh_tgkill);
  reg("tkill", &sh_tkill);
  reg("sigqueue", &sh_sigqueue);
  reg("strsignal", &sh_strsignal);
  reg("psignal", &sh_psignal);
  reg("sigaltstack", &sh_sigaltstack);
  reg("__libc_current_sigrtmin", &sh_sigrtmin);
  reg("__libc_current_sigrtmax", &sh_sigrtmax);
  // setjmp is FreeBSD's _setjmp (shims.cpp), which saves no signal mask;
  // FreeBSD's longjmp would restore one from bytes _setjmp never wrote.
  reg("longjmp", &_longjmp);

  // threads
  reg("pthread_once", &sh_pthread_once);
  reg("pthread_mutexattr_settype", &sh_mutexattr_settype);
  reg("pthread_mutexattr_gettype", &sh_mutexattr_gettype);
  reg("pthread_mutex_lock", &sh_mutex_lock);
  reg("pthread_mutex_trylock", &sh_mutex_trylock);
  reg("pthread_mutex_unlock", &sh_mutex_unlock);
  reg("pthread_mutex_destroy", &sh_mutex_destroy);
  reg("pthread_mutex_timedlock", &sh_mutex_timedlock);
  reg("pthread_mutex_clocklock", &sh_mutex_clocklock);
  reg("pthread_mutex_timedlock_monotonic_np", &sh_mutex_timedlock_monotonic);
  reg("pthread_cond_wait", &sh_cond_wait);
  reg("pthread_cond_timedwait", &sh_cond_timedwait);
  reg("pthread_cond_clockwait", &sh_cond_clockwait);
  reg("pthread_cond_timedwait_monotonic_np", &sh_cond_timedwait_monotonic);
  reg("pthread_cond_timedwait_monotonic", &sh_cond_timedwait_monotonic);
  reg("pthread_cond_timedwait_relative_np", &sh_cond_timedwait_relative);
  reg("pthread_condattr_setclock", &sh_condattr_setclock);
  reg("pthread_condattr_getclock", &sh_condattr_getclock);
  reg("pthread_rwlock_clockrdlock", &sh_rwlock_clockrdlock);
  reg("pthread_rwlock_clockwrlock", &sh_rwlock_clockwrlock);
  reg("pthread_rwlock_timedrdlock_monotonic_np", &sh_rwlock_timedrdlock_monotonic);
  reg("pthread_rwlock_timedwrlock_monotonic_np", &sh_rwlock_timedwrlock_monotonic);
  reg("pthread_rwlockattr_setkind_np", &sh_rwlockattr_setkind);
  reg("pthread_rwlockattr_getkind_np", &sh_rwlockattr_getkind);
  reg("sem_clockwait", &sh_sem_clockwait);
  reg("sem_timedwait_monotonic_np", &sh_sem_timedwait_monotonic);
  reg("pthread_barrierattr_init", &sh_barrierattr_init);
  reg("pthread_barrierattr_destroy", &sh_barrierattr_destroy);
  reg("pthread_barrierattr_setpshared", &sh_barrierattr_setpshared);
  reg("pthread_barrierattr_getpshared", &sh_barrierattr_getpshared);
  reg("pthread_barrier_init", &sh_barrier_init);
  reg("pthread_attr_setschedpolicy", &sh_attr_setschedpolicy);
  reg("pthread_attr_getschedpolicy", &sh_attr_getschedpolicy);
  reg("pthread_setschedparam", &sh_setschedparam);
  reg("pthread_getschedparam", &sh_getschedparam);
  reg("pthread_attr_setscope", &sh_attr_setscope);
  reg("pthread_attr_getscope", &sh_attr_getscope);
  reg("pthread_attr_setinheritsched", &sh_attr_setinheritsched);
  reg("pthread_attr_getinheritsched", &sh_attr_getinheritsched);
  reg("pthread_getattr_np", &sh_getattr_np);
  reg("pthread_gettid_np", &sh_pthread_gettid_np);
  reg("gettid", &sh_gettid);
  reg("__pthread_cleanup_push", &sh_cleanup_push);
  reg("__pthread_cleanup_pop", &sh_cleanup_pop);
  reg("sched_get_priority_max", &sh_sched_get_priority_max);
  reg("sched_get_priority_min", &sh_sched_get_priority_min);
  reg("sched_setscheduler", &sh_sched_setscheduler);
  reg("sched_getscheduler", &sh_sched_getscheduler);
  reg("sched_getaffinity", &sh_sched_getaffinity);
  reg("sched_setaffinity", &sh_sched_setaffinity);
  reg("__sched_cpucount", &sh_sched_cpucount);
  reg("__sched_cpualloc", &sh_sched_cpualloc);
  reg("__sched_cpufree", &sh_sched_cpufree);
  reg("sched_yield", &sh_sched_yield);
  reg("setpriority", &sh_setpriority);
  reg("getpriority", &sh_getpriority);
  // Return an error number: FreeBSD's become Linux's.
  REG_RC(pthread_create);
  REG_RC(pthread_join);
  REG_RC(pthread_detach);
  REG_RC(pthread_mutex_init);
  REG_RC(pthread_cond_init);
  REG_RC(pthread_cond_destroy);
  REG_RC(pthread_cond_signal);
  REG_RC(pthread_cond_broadcast);
  REG_RC(pthread_rwlock_init);
  REG_RC(pthread_rwlock_destroy);
  REG_RC(pthread_rwlock_rdlock);
  REG_RC(pthread_rwlock_wrlock);
  REG_RC(pthread_rwlock_tryrdlock);
  REG_RC(pthread_rwlock_trywrlock);
  REG_RC(pthread_rwlock_unlock);
  REG_RC(pthread_rwlock_timedrdlock);
  REG_RC(pthread_rwlock_timedwrlock);
  REG_RC(pthread_key_create);
  REG_RC(pthread_key_delete);
  REG_RC(pthread_setspecific);
  REG_RC(pthread_attr_init);
  REG_RC(pthread_attr_destroy);
  REG_RC(pthread_attr_setstacksize);
  REG_RC(pthread_attr_getstacksize);
  REG_RC(pthread_attr_setstack);
  REG_RC(pthread_attr_getstack);
  REG_RC(pthread_attr_setdetachstate);
  REG_RC(pthread_attr_getdetachstate);
  REG_RC(pthread_attr_setguardsize);
  REG_RC(pthread_attr_getguardsize);
  REG_RC(pthread_attr_setschedparam);
  REG_RC(pthread_attr_getschedparam);
  REG_RC(pthread_spin_init);
  REG_RC(pthread_spin_destroy);
  REG_RC(pthread_spin_lock);
  REG_RC(pthread_spin_trylock);
  REG_RC(pthread_spin_unlock);
  REG_RC(pthread_barrier_destroy);
  REG_RC(pthread_barrier_wait);
  REG_RC(pthread_mutexattr_init);
  REG_RC(pthread_mutexattr_destroy);
  REG_RC(pthread_mutexattr_setpshared);
  REG_RC(pthread_mutexattr_getpshared);
  REG_RC(pthread_mutexattr_setprotocol);
  REG_RC(pthread_mutexattr_getprotocol);
  REG_RC(pthread_condattr_init);
  REG_RC(pthread_condattr_destroy);
  REG_RC(pthread_condattr_setpshared);
  REG_RC(pthread_condattr_getpshared);
  REG_RC(pthread_rwlockattr_init);
  REG_RC(pthread_rwlockattr_destroy);
  REG_RC(pthread_rwlockattr_setpshared);
  REG_RC(pthread_rwlockattr_getpshared);
  REG_RC(pthread_setname_np);
  REG_RC(pthread_getname_np);
  REG_RC(pthread_atfork);
  REG_RC(pthread_getcpuclockid);
  reg("pthread_setschedprio", &sh_setschedprio);

  // sockets
  reg("socket", &sh_socket);
  reg("socketpair", &sh_socketpair);
  reg("bind", &sh_bind);
  reg("connect", &sh_connect);
  reg("accept", &sh_accept);
  reg("accept4", &sh_accept4);
  reg("getsockname", &sh_getsockname);
  reg("getpeername", &sh_getpeername);
  reg("send", &sh_send);
  reg("recv", &sh_recv);
  reg("sendto", &sh_sendto);
  reg("recvfrom", &sh_recvfrom);
  reg("sendmsg", &sh_sendmsg);
  reg("recvmsg", &sh_recvmsg);
  reg("sendmmsg", &sh_sendmmsg);
  reg("recvmmsg", &sh_recvmmsg);
  reg("__cmsg_nxthdr", &sh_cmsg_nxthdr);
  reg("setsockopt", &sh_setsockopt);
  reg("getsockopt", &sh_getsockopt);
  reg("poll", &sh_poll);
  reg("ppoll", &sh_ppoll);
  reg("ppoll64", &sh_ppoll);
  reg("pselect", &sh_pselect);
  reg("pselect64", &sh_pselect);
  reg("ioctl", &sh_ioctl);
  reg("inet_pton", &sh_inet_pton);
  reg("inet_ntop", &sh_inet_ntop);
  reg("getifaddrs", &sh_getifaddrs);
  reg("freeifaddrs", &sh_freeifaddrs);

  // system
  reg("uname", &sh_uname);
  reg("sysinfo", &sh_sysinfo);
  reg("getauxval", &sh_getauxval);
  reg("prctl", &sh_prctl);
  reg("syscall", &sh_syscall);
  for (const char* n : {"prlimit", "prlimit64"}) reg(n, &sh_prlimit);
  for (const char* n : {"getrlimit", "getrlimit64"}) reg(n, &sh_getrlimit);
  for (const char* n : {"setrlimit", "setrlimit64"}) reg(n, &sh_setrlimit);
  reg("getpwuid", &sh_getpwuid);
  reg("getpwnam", &sh_getpwnam);
  reg("getpwuid_r", &sh_getpwuid_r);
  reg("getpwnam_r", &sh_getpwnam_r);

  // locale and text
  reg("setlocale", &sh_setlocale);
  reg("newlocale", &sh_newlocale);
  reg("nl_langinfo", &sh_nl_langinfo);
  reg("nl_langinfo_l", &sh_nl_langinfo_l);
  reg("__ctype_get_mb_cur_max", &sh_mb_cur_max);
  reg("mbrtowc", &sh_mbrtowc);
  reg("mbrlen", &sh_mbrlen);
  reg("mbsinit", &sh_mbsinit);
  reg("wcrtomb", &sh_wcrtomb);
  reg("mbrtoc32", &sh_mbrtoc32);
  reg("c32rtomb", &sh_c32rtomb);
  reg("mbrtoc16", &sh_mbrtoc16);
  reg("c16rtomb", &sh_c16rtomb);
  reg("mbsrtowcs", &sh_mbsrtowcs);
  reg("mbsnrtowcs", &sh_mbsnrtowcs);
  reg("wcsrtombs", &sh_wcsrtombs);
  reg("wcsnrtombs", &sh_wcsnrtombs);
  reg("mbtowc", &sh_mbtowc);
  reg("mblen", &sh_mblen);
  reg("wctomb", &sh_wctomb);
  reg("mbstowcs", &sh_mbstowcs);
  reg("wcstombs", &sh_wcstombs);
  reg("btowc", &sh_btowc);
  reg("wctob", &sh_wctob);
  reg("__gnu_basename", &sh_gnu_basename);
  reg("rawmemchr", &sh_rawmemchr);

  // _FORTIFY_SOURCE
  reg("__memcpy_chk", &sh_memcpy_chk);
  reg("__memmove_chk", &sh_memmove_chk);
  reg("__memset_chk", &sh_memset_chk);
  reg("__strcpy_chk", &sh_strcpy_chk);
  reg("__stpcpy_chk", &sh_stpcpy_chk);
  reg("__strncpy_chk", &sh_strncpy_chk);
  reg("__stpncpy_chk", &sh_stpncpy_chk);
  reg("__strcat_chk", &sh_strcat_chk);
  reg("__strncat_chk", &sh_strncat_chk);
  reg("__strlcpy_chk", &sh_strlcpy_chk);
  reg("__strlcat_chk", &sh_strlcat_chk);
  reg("__strrchr_chk", &sh_strrchr_chk);
  reg("__memchr_chk", &sh_memchr_chk);
  reg("__memrchr_chk", &sh_memrchr_chk);
  reg("__vsprintf_chk", &sh_vsprintf_chk);
  reg("__sprintf_chk", &sh_sprintf_chk);
  reg("__vsnprintf_chk", &sh_vsnprintf_chk);
  reg("__snprintf_chk", &sh_snprintf_chk);
  reg("__fgets_chk", &sh_fgets_chk);
  reg("__fread_chk", &sh_fread_chk);
  reg("__read_chk", &sh_read_chk);
  reg("__pread_chk", &sh_pread_chk);
  reg("__pread64_chk", &sh_pread_chk);
  reg("__pwrite_chk", &sh_pwrite_chk);
  reg("__pwrite64_chk", &sh_pwrite_chk);
  reg("__readlink_chk", &sh_readlink_chk);
  reg("__readlinkat_chk", &sh_readlinkat_chk);
  reg("__getcwd_chk", &sh_getcwd_chk);
  reg("__recvfrom_chk", &sh_recvfrom_chk);
  reg("__sendto_chk", &sh_sendto_chk);
  reg("__poll_chk", &sh_poll_chk);
  reg("__ppoll_chk", &sh_ppoll_chk);
  reg("__ppoll64_chk", &sh_ppoll_chk);

  measure_yield_gap();
  init_fast_clock();
  if (g_nap_late_us > 1000) {
    char hz[16] = "?";
    int v = 0;
    size_t l = sizeof v;
    if (sysctlbyname("kern.hz", &v, &l, nullptr, 0) == 0) snprintf(hz, sizeof hz, "%d", v);
    fprintf(stderr,
            "[bsdabi] short sleeps come back %.1f ms late (kern.hz=%s). Games run smoother with\n"
            "[bsdabi] kern.hz=1000 in /boot/loader.conf (then reboot).\n",
            g_nap_late_us / 1000.0, hz);
  }
  if (const char* y = getenv("TONIC_YIELD"); y && !strcmp(y, "plain")) {
    g_yield_plain = true;
    fprintf(stderr, "[bsdabi] TONIC_YIELD=plain: sched_yield is not slowed down\n");
  }
  if (const char* st = getenv("TONIC_STATS"); st && *st && *st != '0') {
    g_stats = true;
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto rate = [](std::atomic<uint64_t>& c) { return static_cast<unsigned long long>(c.exchange(0) / 5); };
        fprintf(stderr,
                "[bsdabi] per second: clock_gettime %llu (%llu real reads), futex %llu, sched_yield %llu (naps %llu), "
                "madvise(DONTNEED) %llu (memory-map lookups %llu)\n",
                rate(g_n_clock), rate(g_n_clock_real), rate(g_n_futex), rate(g_n_yield), rate(g_n_nap), rate(g_n_dontneed), rate(g_n_vmmap));
        {
          const auto polls = g_n_poll.exchange(0), to = g_n_poll_timeout.exchange(0), to_ms = g_poll_timeout_ms.exchange(0);
          fprintf(stderr,
                  "[bsdabi]   network per second: poll %llu (%llu ended by timeout, %.0f ms spent in those), "
                  "connect %llu, in %llu KB, out %llu KB (socket calls only)\n",
                  static_cast<unsigned long long>(polls / 5), static_cast<unsigned long long>(to / 5), to_ms / 5.0,
                  rate(g_n_connect), rate(g_net_in) / 1024, rate(g_net_out) / 1024);
        }
        for (WaitStat* w : {&g_w_futex, &g_w_cond, &g_w_sleep, &g_w_mutex}) {
          const auto t = w->timeouts.exchange(0), l = w->late.exchange(0), worst = w->worst_us.exchange(0);
          if (t)
            fprintf(stderr, "[bsdabi]   timed %s waits: %llu ended, %llu more than 5 ms late (worst %.1f ms)\n", w->name,
                    static_cast<unsigned long long>(t), static_cast<unsigned long long>(l), worst / 1000.0);
        }
      }
    }).detach();
  }
  fprintf(stderr, "[bsdabi] FreeBSD translation layer ready\n");
}

long bsd_raw_syscall(long nr, const long args[6]) {
  // A raw syscall leaves errno alone: the library's (t_errno) and FreeBSD's.
  const int saved_errno = errno, saved_lib_errno = t_errno;
  errno = 0;
  const long r = raw_linux_call(nr, args);
  int e = errno;
  if (r == -1 && !e && t_errno != saved_lib_errno) e = bsd_errno(t_errno);  // (set as Linux's directly)
  errno = saved_errno;
  t_errno = saved_lib_errno;
  if (r == -1 && e) return -static_cast<long>(lx_errno(e));
  return r;
}

}  // namespace tonic

#endif  // __FreeBSD__
