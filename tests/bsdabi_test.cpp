// FreeBSD: the bionic/Linux translations in src/bsdabi.cpp, called the way
// Roblox's library calls them (looked up by name, Linux numbers and layouts).
// make test-bsdabi
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/bsdabi.hpp"
#include "../src/shims.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}
template <typename F>
F fn(const char* name) {
  return reinterpret_cast<F>(tonic::resolve_symbol(name, false));
}
int lx_errno() { return *fn<int* (*)()>("__errno")(); }

// Linux values used below.
constexpr int L_O_WRONLY = 1, L_O_CREAT = 0x40, L_O_TRUNC = 0x200, L_O_NONBLOCK = 0x800, L_O_CLOEXEC = 0x80000;
constexpr int L_EAGAIN = 11, L_ENOENT = 2;
constexpr int L_SIGUSR1 = 10, L_SA_SIGINFO = 4;
constexpr int L_AF_INET = 2, L_SOCK_DGRAM = 2, L_SOCK_NONBLOCK = 0x800, L_SOL_SOCKET = 1, L_SO_ERROR = 4, L_SO_REUSEADDR = 2;

volatile int g_sig = 0, g_sig_code = 99;
void on_usr1(int s, void* info, void*) {
  g_sig = s;
  g_sig_code = static_cast<const int*>(info)[2];
}

}  // namespace

int main() {
  tonic::init_shims(false);
  tonic::init_bsd_abi();
  const std::string dir = "/tmp/tonic-bsdabi-" + std::to_string(getpid());
  mkdir(dir.c_str(), 0700);

  puts("errno and files:");
  auto lopen = fn<int (*)(const char*, int, ...)>("open");
  check(lopen("/nonexistent/x", 0) < 0 && lx_errno() == L_ENOENT, "open of a missing file -> ENOENT (2)");
  const std::string f = dir + "/f";
  int fd = lopen(f.c_str(), L_O_CREAT | L_O_WRONLY | L_O_TRUNC | L_O_CLOEXEC, 0600);
  check(fd >= 0, "open with Linux O_CREAT|O_TRUNC|O_CLOEXEC");
  auto lfcntl = fn<int (*)(int, int, ...)>("fcntl");
  check(lfcntl(fd, 1) == 1, "fcntl(F_GETFD) has FD_CLOEXEC");
  check((lfcntl(fd, 3) & 3) == L_O_WRONLY, "fcntl(F_GETFL) is O_WRONLY");
  check(write(fd, "hello", 5) == 5, "write");
  struct { unsigned char st[144]; unsigned char guard[64]; } sb;
  memset(sb.guard, 0xAB, sizeof sb.guard);
  check(fn<int (*)(int, void*)>("fstat")(fd, sb.st) == 0, "fstat");
  int64_t size;
  memcpy(&size, sb.st + 48, 8);
  check(size == 5, "Linux struct stat: st_size at offset 48");
  bool guard_ok = true;
  for (unsigned char c : sb.guard) guard_ok &= c == 0xAB;
  check(guard_ok, "fstat wrote only 144 bytes");
  close(fd);

  int p[2];
  check(fn<int (*)(int*, int)>("pipe2")(p, L_O_NONBLOCK | L_O_CLOEXEC) == 0, "pipe2(O_NONBLOCK|O_CLOEXEC)");
  char c;
  check(read(p[0], &c, 1) < 0 && lx_errno() == L_EAGAIN, "read on an empty non-blocking pipe -> EAGAIN (11)");
  struct { int fd; short events, revents; } pf = {p[1], 4 | 0x100 /* POLLOUT|POLLWRNORM */, 0};
  check(fn<int (*)(void*, unsigned long, int)>("poll")(&pf, 1, 0) == 1 && (pf.revents & 0x104) == 0x104,
        "poll: POLLOUT and Linux POLLWRNORM");

  auto lreaddir = fn<void* (*)(void*)>("readdir");
  void* d = fn<void* (*)(const char*)>("opendir")(dir.c_str());
  bool found = false;
  while (const char* e = static_cast<const char*>(lreaddir(d)))
    if (!strcmp(e + 19, "f")) found = true;
  fn<int (*)(void*)>("closedir")(d);
  check(found, "readdir: Linux struct dirent (d_name at 19)");

  puts("memory:");
  auto lmmap = fn<void* (*)(void*, size_t, int, int, int, long)>("mmap");
  auto* m = static_cast<unsigned char*>(lmmap(nullptr, 8192, 3, 0x22 /* PRIVATE|ANONYMOUS */, -1, 0));
  check(m != reinterpret_cast<void*>(-1), "mmap(MAP_PRIVATE|MAP_ANONYMOUS)");
  memset(m, 0x5A, 8192);
  check(fn<int (*)(void*, size_t, int)>("madvise")(m, 8192, 4 /* DONTNEED */) == 0 && m[0] == 0 && m[8191] == 0,
        "madvise(MADV_DONTNEED) leaves zero pages, as on Linux");
  fn<int (*)(void*, size_t, int)>("mprotect")(m, 4096, 1 /* PROT_READ */);
  check(fn<int (*)(void*, size_t, int)>("madvise")(m, 8192, 4) == 0 && m[0] == 0,
        "DONTNEED after mprotect: zero pages, protection kept");
  memset(m + 4096, 0x5A, 4096);  // the second page is still writable
  check(m[4096] == 0x5A, "the part mprotect did not touch stays writable");
  fn<int (*)(void*, size_t)>("munmap")(m, 8192);

  puts("time:");
  auto lclock = fn<int (*)(int, timespec*)>("clock_gettime");
  timespec a{}, b{};
  lclock(1, &a);  // Linux CLOCK_MONOTONIC (FreeBSD's 1 is CLOCK_VIRTUAL)
  usleep(20000);
  lclock(1, &b);
  const double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
  check(dt > 0.015 && dt < 1.0, "clock_gettime(CLOCK_MONOTONIC) follows wall time");

  puts("signals:");
  struct { int flags; void* handler; uint64_t mask; void* restorer; unsigned char guard[32]; } act{}, old{};
  act.flags = L_SA_SIGINFO;
  act.handler = reinterpret_cast<void*>(&on_usr1);
  memset(old.guard, 0xAB, sizeof old.guard);
  auto lsigaction = fn<int (*)(int, const void*, void*)>("sigaction");
  check(lsigaction(L_SIGUSR1, &act, nullptr) == 0, "sigaction(SIGUSR1 = 10, SA_SIGINFO)");
  check(lsigaction(L_SIGUSR1, nullptr, &old) == 0 && old.handler == act.handler, "sigaction gives the handler back");
  fn<int (*)(int)>("raise")(L_SIGUSR1);
  check(g_sig == L_SIGUSR1, "handler called with Linux SIGUSR1 (10)");
  check(g_sig_code == -6 || g_sig_code == 0, "si_code in Linux numbering");
  uint64_t set = 0, was = 0;
  fn<int (*)(uint64_t*, int)>("sigaddset")(&set, L_SIGUSR1);
  check(fn<int (*)(int, const uint64_t*, uint64_t*)>("pthread_sigmask")(0 /* SIG_BLOCK */, &set, nullptr) == 0,
        "pthread_sigmask(SIG_BLOCK = 0)");
  fn<int (*)(int, const uint64_t*, uint64_t*)>("pthread_sigmask")(0, nullptr, &was);
  check(was & (1ULL << (L_SIGUSR1 - 1)), "mask reads back with Linux bit numbering");
  fn<int (*)(int, const uint64_t*, uint64_t*)>("pthread_sigmask")(1 /* SIG_UNBLOCK */, &set, nullptr);

  puts("threads:");
  struct { int once; unsigned char guard[16]; } og{};
  memset(og.guard, 0xAB, sizeof og.guard);
  static int runs = 0;
  auto lonce = fn<int (*)(int*, void (*)())>("pthread_once");
  lonce(&og.once, [] { ++runs; });
  lonce(&og.once, [] { ++runs; });
  check(runs == 1 && og.guard[0] == 0xAB && og.guard[15] == 0xAB, "pthread_once on a 4-byte pthread_once_t");
  struct { uint64_t w[5]; } mx = {{0x4000, 0, 0, 0, 0}};  // bionic PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
  auto llock = fn<int (*)(void*)>("pthread_mutex_lock");
  auto lunlock = fn<int (*)(void*)>("pthread_mutex_unlock");
  check(llock(&mx) == 0 && llock(&mx) == 0, "bionic's static recursive mutex locks twice");
  lunlock(&mx);
  lunlock(&mx);
  long attr = 0;
  struct { uint64_t w[5]; } m2{};
  fn<int (*)(void*)>("pthread_mutexattr_init")(&attr);
  check(fn<int (*)(void*, int)>("pthread_mutexattr_settype")(&attr, 1 /* bionic RECURSIVE */) == 0, "mutexattr_settype(1)");
  fn<int (*)(void*, void*)>("pthread_mutex_init")(&m2, &attr);
  check(llock(&m2) == 0 && llock(&m2) == 0, "type 1 is recursive (FreeBSD's 1 is ERRORCHECK)");
  lunlock(&m2);
  lunlock(&m2);
  auto lsyscall = fn<long (*)(long, ...)>("syscall");
  check(lsyscall(186) == fn<long (*)()>("gettid")(), "syscall(SYS_gettid = 186)");
  uint32_t fx = 1;
  check(lsyscall(202, &fx, 128 /* FUTEX_WAIT_PRIVATE */, 0, nullptr, nullptr, 0) == -1 && lx_errno() == L_EAGAIN,
        "futex wait on a changed value -> EAGAIN");
  check(lsyscall(202, &fx, 129 /* FUTEX_WAKE_PRIVATE */, 1, nullptr, nullptr, 0) == 0, "futex wake");
  check(lsyscall(99999) == -1 && lx_errno() == 38, "unknown syscall -> ENOSYS (38), not run");

  puts("sockets:");
  const int s = fn<int (*)(int, int, int)>("socket")(L_AF_INET, L_SOCK_DGRAM | L_SOCK_NONBLOCK, 0);
  check(s >= 0, "socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK)");
  const int one = 1;
  check(fn<int (*)(int, int, int, const void*, unsigned)>("setsockopt")(s, L_SOL_SOCKET, L_SO_REUSEADDR, &one, 4) == 0,
        "setsockopt(SOL_SOCKET = 1, SO_REUSEADDR = 2)");
  unsigned char sa[16] = {L_AF_INET, 0, 0, 0, 127, 0, 0, 1};  // Linux sockaddr_in, port 0
  check(fn<int (*)(int, const void*, unsigned)>("bind")(s, sa, 16) == 0, "bind with a Linux sockaddr_in");
  unsigned char me[16] = {};
  unsigned ml = sizeof me;
  check(fn<int (*)(int, void*, unsigned*)>("getsockname")(s, me, &ml) == 0 && me[0] == L_AF_INET && me[1] == 0 && ml == 16,
        "getsockname gives a Linux sockaddr_in");
  check(fn<long (*)(int, void*, size_t, int)>("recv")(s, &c, 1, 0) < 0 && lx_errno() == L_EAGAIN, "recv with nothing -> EAGAIN");
  check(fn<long (*)(int, const void*, size_t, int, const void*, unsigned)>("sendto")(s, "x", 1, 0x4000 /* MSG_NOSIGNAL */, me, 16) == 1,
        "sendto(MSG_NOSIGNAL) to a Linux sockaddr");
  usleep(20000);
  unsigned char from[16] = {};
  unsigned fl = sizeof from;
  check(fn<long (*)(int, void*, size_t, int, void*, unsigned*)>("recvfrom")(s, &c, 1, 0, from, &fl) == 1 && from[0] == L_AF_INET,
        "recvfrom gives a Linux sockaddr_in");
  int err = -1;
  unsigned el = 4;
  check(fn<int (*)(int, int, int, void*, unsigned*)>("getsockopt")(s, L_SOL_SOCKET, L_SO_ERROR, &err, &el) == 0 && err == 0,
        "getsockopt(SO_ERROR)");
  // UDP_SEGMENT (Linux UDP GSO): 250 bytes with a 100-byte segment size must
  // arrive as datagrams of 100, 100 and 50.
  const int gso = 100;
  check(fn<int (*)(int, int, int, const void*, unsigned)>("setsockopt")(s, 17 /* SOL_UDP */, 103 /* UDP_SEGMENT */, &gso, 4) == 0,
        "setsockopt(SOL_UDP, UDP_SEGMENT, 100)");
  char big[250];
  memset(big, 'g', sizeof big);
  check(fn<long (*)(int, const void*, size_t, int, const void*, unsigned)>("sendto")(s, big, sizeof big, 0, me, 16) == 250,
        "sendto of 250 bytes with GSO");
  usleep(20000);
  long sizes[4] = {0, 0, 0, 0};
  for (long& z : sizes) z = fn<long (*)(int, void*, size_t, int)>("recv")(s, big, sizeof big, 0);
  check(sizes[0] == 100 && sizes[1] == 100 && sizes[2] == 50 && sizes[3] < 0, "arrives as datagrams of 100, 100, 50");
  close(s);
  struct Ai { int flags, family, socktype, protocol; unsigned addrlen; char* canon; unsigned char* addr; Ai* next; };
  Ai hints = {0x4 /* AI_NUMERICHOST */, 0, 1, 0, 0, nullptr, nullptr, nullptr}, *res = nullptr;
  check(fn<int (*)(const char*, const char*, const void*, Ai**)>("getaddrinfo")("127.0.0.1", "443", &hints, &res) == 0 && res &&
            res->family == L_AF_INET && res->addr[0] == L_AF_INET && res->addr[1] == 0 && res->addr[2] == 1 && res->addr[3] == 0xbb,
        "getaddrinfo: Linux families and sockaddrs");
  if (res) fn<void (*)(Ai*)>("freeaddrinfo")(res);

  puts("text and system:");
  struct { uint64_t state; unsigned char guard[16]; } ms{};
  memset(ms.guard, 0xAB, sizeof ms.guard);
  const char* text = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";  // é € 😀
  auto lmbrtowc = fn<size_t (*)(wchar_t*, const char*, size_t, void*)>("mbrtowc");
  wchar_t w1 = 0, w2 = 0, w3 = 0;
  const size_t r1 = lmbrtowc(&w1, text, 9, &ms.state);
  const size_t r2 = lmbrtowc(&w2, text + 2, 1, &ms.state);  // one byte at a time: -2 twice, then 3
  const size_t r2b = lmbrtowc(&w2, text + 3, 1, &ms.state);
  const size_t r2c = lmbrtowc(&w2, text + 4, 1, &ms.state);
  const size_t r3 = lmbrtowc(&w3, text + 5, 4, &ms.state);
  check(r1 == 2 && w1 == 0xE9 && r2 == static_cast<size_t>(-2) && r2b == static_cast<size_t>(-2) && r2c == 1 &&
            w2 == 0x20AC && r3 == 4 && w3 == 0x1F600,
        "mbrtowc: UTF-8, partial sequences kept in the state");
  check(ms.guard[0] == 0xAB && ms.guard[15] == 0xAB, "mbstate_t: only 8 bytes used");
  char out[8] = {};
  check(fn<size_t (*)(char*, wchar_t, void*)>("wcrtomb")(out, 0x1F600, nullptr) == 4 && !memcmp(out, text + 5, 4),
        "wcrtomb back to UTF-8");
  struct { char f[6][65]; } u{};
  check(fn<int (*)(void*)>("uname")(&u) == 0 && !strcmp(u.f[0], "FreeBSD") && !strcmp(u.f[4], "x86_64"),
        "uname: FreeBSD, x86_64 (Linux's name for amd64), Linux layout");
  check(fn<void* (*)(void*, const char*)>("dlsym")(nullptr, "printf") != nullptr, "dlsym(bionic RTLD_DEFAULT = 0)");
  FILE** out_var = reinterpret_cast<FILE**>(tonic::resolve_symbol("stdout", false));
  check(out_var && *out_var == stdout, "stdout is FreeBSD's __stdoutp");

  unlink(f.c_str());
  rmdir(dir.c_str());
  printf("%s: %d failed\n", g_fail ? "FAIL" : "bsdabi", g_fail);
  return g_fail ? 1 : 0;
}
