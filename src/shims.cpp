#include "shims.hpp"
#include "netping.hpp"
#include "privacy.hpp"

#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace tonic {

namespace {

std::unordered_map<std::string, void*>& table() {
  static std::unordered_map<std::string, void*> t;
  return t;
}

std::vector<std::string> g_stubbed;
std::vector<HostSym> g_host;

struct Image {
  std::string name;
  uint64_t base, span;
  const Elf64_Phdr* phdrs;
  size_t phnum;
};
std::vector<Image> g_images;
ShimStats g_stats;
bool g_abort_on_stub = false;

// ---------------------------------------------------------------------------
// Logging stubs. Each unimplemented function gets a tiny thunk that passes its
// own name to stub_trap, so the first thing we learn about a missing function
// is its name and call order.
// ---------------------------------------------------------------------------

uint8_t* g_thunks = nullptr;
size_t g_thunk_used = 0;
constexpr size_t kThunkCap = 1 << 20;
constexpr size_t kThunkSize = 32;

extern "C" uint64_t tonic_stub_trap(const char* name) {
  fprintf(stderr, "[stub] %s called\n", name);
  if (g_abort_on_stub) abort();
  return 0;
}

uint64_t make_stub(const std::string& name) {
  if (!g_thunks) {
    void* m = mmap(nullptr, kThunkCap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
      perror("mmap thunks");
      abort();
    }
    g_thunks = static_cast<uint8_t*>(m);
  }
  if (g_thunk_used + kThunkSize > kThunkCap) {
    fprintf(stderr, "out of stub space\n");
    abort();
  }
  const char* nm = strdup(name.c_str());
  const uint64_t handler = reinterpret_cast<uint64_t>(&tonic_stub_trap);
  uint8_t* p = g_thunks + g_thunk_used;
  g_thunk_used += kThunkSize;
  size_t i = 0;
  p[i++] = 0xF3; p[i++] = 0x0F; p[i++] = 0x1E; p[i++] = 0xFA;  // endbr64
  p[i++] = 0x48; p[i++] = 0xBF;                                // mov rdi, imm64 (name)
  memcpy(p + i, &nm, 8); i += 8;
  p[i++] = 0x48; p[i++] = 0xB8;                                // mov rax, imm64 (handler)
  memcpy(p + i, &handler, 8); i += 8;
  p[i++] = 0xFF; p[i++] = 0xE0;                                // jmp rax
  return reinterpret_cast<uint64_t>(p);
}

// ---------------------------------------------------------------------------
// bionic-specific functions and data.
// ---------------------------------------------------------------------------

// bionic's stdin/stdout/stderr (for older API levels) are elements of __sF.
// sizeof(FILE) on bionic x86_64 is 0x98: libroblox.so passes __sF+0x98 and
// __sF+0x130 for stdout and stderr. If a pointer into __sF is not a multiple of
// this we warn. Override with TONIC_BIONIC_FILE_SIZE.
alignas(16) unsigned char g_sF[3 * 0x200];
size_t g_file_size = 0x98;

FILE* map_file(FILE* f) {
  const uintptr_t p = reinterpret_cast<uintptr_t>(f), b = reinterpret_cast<uintptr_t>(g_sF);
  if (p < b || p >= b + sizeof g_sF) return f;
  const size_t off = p - b;
  if (off % g_file_size) fprintf(stderr, "[warn] __sF offset %#zx is not a multiple of assumed FILE size %#zx\n", off, g_file_size);
  switch (off / g_file_size) {
    case 0: return stdin;
    case 1: return stdout;
    default: return stderr;
  }
}

uintptr_t g_stack_chk_guard;

const char* g_amf_bitrate = "bitrate";
const char* g_amf_channels = "channel-count";
const char* g_amf_color = "color-format";
const char* g_amf_framerate = "frame-rate";
const char* g_amf_height = "height";
const char* g_amf_iframe = "i-frame-interval";
const char* g_amf_mime = "mime";
const char* g_amf_samplerate = "sample-rate";
const char* g_amf_stride = "stride";
const char* g_amf_width = "width";

int* sh_errno() { return &errno; }

// We report no Android system properties rather than inventing device values.
int sh_property_get(const char*, char* value) {
  if (value) value[0] = '\0';
  return 0;
}

void sh_set_abort_message(const char* msg) { fprintf(stderr, "[abort-message] %s\n", msg ? msg : "(null)"); }

void sh_assert2(const char* file, int line, const char* func, const char* msg) {
  fprintf(stderr, "[assert] %s:%d: %s: %s\n", file, line, func ? func : "", msg);
  abort();
}

#if defined(__FreeBSD__) || defined(__OpenBSD__)
char* sh_gnu_strerror_r(int e, char* buf, size_t n) {  // FreeBSD's is the XSI one (returns int)
  if (n) strerror_r(e, buf, n);
  return buf;
}
#else
char* sh_gnu_strerror_r(int e, char* buf, size_t n) { return strerror_r(e, buf, n); }
#endif
size_t sh_strlen_chk(const char* s, size_t) { return strlen(s); }
char* sh_strchr_chk(const char* s, int c, size_t) { return const_cast<char*>(strchr(s, c)); }
char* sh_strncpy_chk2(char* d, const char* s, size_t n, size_t, size_t) { return strncpy(d, s, n); }
void sh_fd_set_chk(int fd, fd_set* s, size_t) { FD_SET(fd, s); }
void sh_fd_clr_chk(int fd, fd_set* s, size_t) { FD_CLR(fd, s); }
int sh_fd_isset_chk(int fd, const fd_set* s, size_t) { return FD_ISSET(fd, s); }
ssize_t sh_write_chk(int fd, const void* b, size_t n, size_t) { return write(fd, b, n); }
mode_t sh_umask_chk(mode_t m) { return umask(m); }
ssize_t sh_sendto_chk(int fd, const void* b, size_t n, size_t, int flags, const sockaddr* a, socklen_t l) {
  netping_note(a, l);
  return sendto(fd, b, n, flags, a, l);
}
// Where the game's traffic goes (the address only), for the stats overlay's
// ping (netping.hpp). On FreeBSD bsdabi.cpp's translations do the same.
ssize_t sh_sendto(int fd, const void* b, size_t n, int flags, const sockaddr* a, socklen_t l) {
  netping_note(a, l);
  return sendto(fd, b, n, flags, a, l);
}
ssize_t sh_sendmsg(int fd, const msghdr* m, int flags) {
  if (m) netping_note(static_cast<const sockaddr*>(m->msg_name), m->msg_namelen);
  return sendmsg(fd, m, flags);
}
#ifdef __linux__
int sh_sendmmsg(int fd, mmsghdr* v, unsigned n, int flags) {
  if (v && n) netping_note(static_cast<const sockaddr*>(v[0].msg_hdr.msg_name), v[0].msg_hdr.msg_namelen);
  return sendmmsg(fd, v, n, flags);
}
#endif
int sh_connect(int fd, const sockaddr* a, socklen_t l) {
  netping_note(a, l);
  return connect(fd, a, l);
}
size_t sh_fwrite_chk(const void* p, size_t sz, size_t n, FILE* f, size_t) { return fwrite(p, sz, n, map_file(f)); }

int sh_fprintf(FILE* f, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_file(f), fmt, ap);
  va_end(ap);
  return r;
}
int sh_vfprintf(FILE* f, const char* fmt, va_list ap) { return vfprintf(map_file(f), fmt, ap); }
int sh_fputs(const char* s, FILE* f) { return fputs(s, map_file(f)); }
int sh_fputc(int c, FILE* f) { return fputc(c, map_file(f)); }
size_t sh_fwrite(const void* p, size_t sz, size_t n, FILE* f) { return fwrite(p, sz, n, map_file(f)); }
int sh_fflush(FILE* f) { return fflush(f ? map_file(f) : nullptr); }

// Android logging goes to stderr.
int sh_log_write(int prio, const char* tag, const char* text) {
  fprintf(stderr, "[log %d] %s: %s\n", prio, tag ? tag : "", text ? text : "");
  return 1;
}
int sh_log_buf_write(int, int prio, const char* tag, const char* text) { return sh_log_write(prio, tag, text); }
int sh_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  fprintf(stderr, "[log %d] %s: ", prio, tag ? tag : "");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  return 1;
}
int sh_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = sh_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}
void sh_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  fprintf(stderr, "[log assert] %s: cond=%s ", tag ? tag : "", cond ? cond : "");
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
  abort();
}

// abort(): say who called it before dying. A silent SIGABRT otherwise tells us
// nothing about which part of the library gave up.
[[noreturn]] void sh_abort() {
  char who[300];
  describe_addr(reinterpret_cast<uint64_t>(__builtin_return_address(0)), who, sizeof who);
  fprintf(stderr, "\n[abort] abort() called from %s\n", who);
  abort();
}

// ---------------------------------------------------------------------------
// sysconf: bionic and glibc number the _SC_* names differently. Passing a
// bionic name straight to glibc asks a different question: bionic
// _SC_PAGESIZE (0x27) is glibc _SC_BC_STRING_MAX, so the page size comes back
// as 1000. Values are from bionic's <bits/sysconf.h>; anything not listed is
// logged and fails with EINVAL rather than returning a wrong answer.
// ---------------------------------------------------------------------------
long sh_sysconf(int bionic_name) {
  struct Map { int bionic; int glibc; const char* name; };
  static const Map kMap[] = {
      {0x00, _SC_ARG_MAX, "ARG_MAX"},          {0x05, _SC_CHILD_MAX, "CHILD_MAX"},
      {0x06, _SC_CLK_TCK, "CLK_TCK"},          {0x09, _SC_LINE_MAX, "LINE_MAX"},
      {0x0a, _SC_NGROUPS_MAX, "NGROUPS_MAX"},  {0x0b, _SC_OPEN_MAX, "OPEN_MAX"},
      {0x1b, _SC_STREAM_MAX, "STREAM_MAX"},    {0x26, _SC_IOV_MAX, "IOV_MAX"},
      {0x27, _SC_PAGESIZE, "PAGESIZE"},        {0x28, _SC_PAGESIZE, "PAGE_SIZE"},
      {0x47, _SC_GETGR_R_SIZE_MAX, "GETGR_R_SIZE_MAX"},
      {0x48, _SC_GETPW_R_SIZE_MAX, "GETPW_R_SIZE_MAX"},
      {0x4c, _SC_THREAD_STACK_MIN, "THREAD_STACK_MIN"},
      {0x60, _SC_NPROCESSORS_CONF, "NPROCESSORS_CONF"},
      {0x61, _SC_NPROCESSORS_ONLN, "NPROCESSORS_ONLN"},
      {0x62, _SC_PHYS_PAGES, "PHYS_PAGES"},
#ifdef _SC_AVPHYS_PAGES
      {0x63, _SC_AVPHYS_PAGES, "AVPHYS_PAGES"},
#else
      {0x63, _SC_PHYS_PAGES, "AVPHYS_PAGES (as PHYS_PAGES)"},  // (FreeBSD has no "available" count)
#endif
  };
  static bool seen[256];
  for (const Map& m : kMap) {
    if (m.bionic != bionic_name) continue;
    const long r = sysconf(m.glibc);
    if (bionic_name < 256 && !seen[bionic_name]) {
      seen[bionic_name] = true;
      fprintf(stderr, "[sysconf] %s (bionic %#x) -> %ld\n", m.name, bionic_name, r);
    }
    return r;
  }
  fprintf(stderr, "[sysconf] UNMAPPED bionic name %#x: returning -1/EINVAL (add it to sh_sysconf)\n", bionic_name);
  errno = EINVAL;
  return -1;
}

// ---------------------------------------------------------------------------
// Signals. bionic LP64 sigset_t is 8 bytes and its struct sigaction is
// { int sa_flags; handler; sigset_t sa_mask; restorer } = 32 bytes. glibc's are
// 128 and 152 bytes, so calling glibc directly writes far past the library's
// buffers. Signal numbers and SA_* flags are the kernel's and match.
// ---------------------------------------------------------------------------
using bsigset_t = uint64_t;
struct bsigaction {
  int sa_flags;
  void* handler;
  bsigset_t sa_mask;
  void (*sa_restorer)();
};
static_assert(sizeof(bsigaction) == 32, "bionic LP64 struct sigaction is 32 bytes");

void to_glibc(const bsigset_t* in, sigset_t* out) {
  sigemptyset(out);
  for (int s = 1; s <= 64; ++s)
    if (*in & (1ULL << (s - 1))) sigaddset(out, s);
}
void to_bionic(const sigset_t* in, bsigset_t* out) {
  *out = 0;
  for (int s = 1; s <= 64; ++s)
    if (sigismember(in, s) == 1) *out |= 1ULL << (s - 1);
}

int sh_sigemptyset(bsigset_t* set) {
  if (!set) return errno = EINVAL, -1;
  *set = 0;
  return 0;
}
int sh_sigfillset(bsigset_t* set) {
  if (!set) return errno = EINVAL, -1;
  *set = ~0ULL;
  return 0;
}

int sh_sigaction(int sig, const bsigaction* act, bsigaction* old) {
  struct sigaction gact{}, gold{};
  if (act) {
    gact.sa_handler = reinterpret_cast<void (*)(int)>(act->handler);
    gact.sa_flags = act->sa_flags & ~0x04000000;  // SA_RESTORER: glibc supplies its own
    to_glibc(&act->sa_mask, &gact.sa_mask);
    fprintf(stderr, "[sigaction] library installs handler for signal %d (%s)\n", sig, strsignal(sig));
  }
  const int r = sigaction(sig, act ? &gact : nullptr, old ? &gold : nullptr);
  if (r == 0 && old) {
    old->handler = reinterpret_cast<void*>(gold.sa_handler);
    old->sa_flags = gold.sa_flags;
    to_bionic(&gold.sa_mask, &old->sa_mask);
    old->sa_restorer = nullptr;
  }
  return r;
}

int sh_pthread_sigmask(int how, const bsigset_t* set, bsigset_t* old) {
  sigset_t gs, go;
  if (set) to_glibc(set, &gs);
  const int r = pthread_sigmask(how, set ? &gs : nullptr, old ? &go : nullptr);
  if (r == 0 && old) to_bionic(&go, old);
  return r;
}

// dl_iterate_phdr(): our images only. The library's own LLVM unwinder must not
// walk into tonic's libstdc++ frames (it crashes there); hidden, an uncaught
// exception ends in std::terminate, as at ART frames on Android.
int sh_dl_iterate_phdr(int (*cb)(dl_phdr_info*, size_t, void*), void* data) {
  for (const Image& im : g_images) {
    dl_phdr_info info{};
    info.dlpi_addr = im.base;
    info.dlpi_name = im.name.c_str();
    info.dlpi_phdr = im.phdrs;
    info.dlpi_phnum = static_cast<ElfW(Half)>(im.phnum);
    info.dlpi_adds = 1;
    if (int r = cb(&info, sizeof info, data)) return r;
  }
  return 0;
}

template <typename T>
void reg(const char* name, T* p) {
  table()[name] = reinterpret_cast<void*>(p);
}

}  // namespace

void register_shim(const char* name, void* fn) { table()[name] = fn; }
void* shim_lookup(const char* name) {
  auto it = table().find(name);
  return it == table().end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// getaddrinfo & co. bionic's struct addrinfo has the BSD layout, where
// ai_canonname comes BEFORE ai_addr; glibc has them the other way round. The
// AI_*, NI_* and EAI_* numbers differ too. Passed straight through, every
// lookup "succeeds" with ai_addr pointing at the canonical name (usually
// null), and the library reports DnsResolve. These convert both ways.
// ---------------------------------------------------------------------------
namespace {

struct BionicAddrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  socklen_t ai_addrlen;
  char* ai_canonname;
  sockaddr* ai_addr;
  BionicAddrinfo* ai_next;
};

struct FlagPair {
  int bionic, glibc;
};
constexpr FlagPair kAiFlags[] = {{0x1, AI_PASSIVE},     {0x2, AI_CANONNAME}, {0x4, AI_NUMERICHOST},
                                 {0x8, AI_NUMERICSERV}, {0x100, AI_ALL},     {0x400, AI_ADDRCONFIG},
                                 {0x800, AI_V4MAPPED}};
constexpr FlagPair kNiFlags[] = {{0x1, NI_NOFQDN},       {0x2, NI_NUMERICHOST}, {0x4, NI_NAMEREQD},
                                 {0x8, NI_NUMERICSERV},  {0x10, NI_DGRAM}};
constexpr FlagPair kEai[] = {{1, EAI_ADDRFAMILY}, {2, EAI_AGAIN},   {3, EAI_BADFLAGS}, {4, EAI_FAIL},
                             {5, EAI_FAMILY},     {6, EAI_MEMORY},  {7, EAI_NODATA},   {8, EAI_NONAME},
                             {9, EAI_SERVICE},    {10, EAI_SOCKTYPE}, {11, EAI_SYSTEM}, {14, EAI_OVERFLOW}};

int map_flags(int in, const FlagPair* t, size_t n, bool to_glibc) {
  int out = 0;
  for (size_t i = 0; i < n; ++i)
    if (in & (to_glibc ? t[i].bionic : t[i].glibc)) out |= to_glibc ? t[i].glibc : t[i].bionic;
  return out;
}
int eai_to_bionic(int e) {
  if (e == 0) return 0;
  for (const auto& p : kEai)
    if (p.glibc == e) return p.bionic;
  return 4;  // EAI_FAIL
}
int eai_to_glibc(int e) {
  for (const auto& p : kEai)
    if (p.bionic == e) return p.glibc;
  return EAI_FAIL;
}

// The same struct hostent as glibc's; only the analytics block (privacy.hpp).
hostent* sh_gethostbyname(const char* name) {
  if (privacy_blocked(name)) {
    h_errno = HOST_NOT_FOUND;
    return nullptr;
  }
  return gethostbyname(name);
}

int sh_getaddrinfo(const char* node, const char* service, const BionicAddrinfo* bh, BionicAddrinfo** res) {
  addrinfo h{};
  if (bh) {
    h.ai_flags = map_flags(bh->ai_flags, kAiFlags, sizeof kAiFlags / sizeof *kAiFlags, true);
    h.ai_family = bh->ai_family;
    h.ai_socktype = bh->ai_socktype;
    h.ai_protocol = bh->ai_protocol;
  }
  *res = nullptr;
  if (privacy_blocked(node)) return 8;  // bionic EAI_NONAME: "no such host" (privacy.hpp)
  addrinfo* g = nullptr;
  const int r = getaddrinfo(node, service, bh ? &h : nullptr, &g);
  *res = nullptr;
  if (r != 0) {
    fprintf(stderr, "[dns] getaddrinfo(%s, %s) failed: %s\n", node ? node : "(null)", service ? service : "(null)",
            gai_strerror(r));
    return eai_to_bionic(r);
  }
  // One allocation per entry: the entry, then its address, then its name.
  // sh_freeaddrinfo frees entry by entry.
  BionicAddrinfo** tail = res;
  for (addrinfo* p = g; p; p = p->ai_next) {
    const size_t name_len = p->ai_canonname ? strlen(p->ai_canonname) + 1 : 0;
    auto* b = static_cast<BionicAddrinfo*>(calloc(1, sizeof(BionicAddrinfo) + p->ai_addrlen + name_len));
    if (!b) break;
    b->ai_flags = map_flags(p->ai_flags, kAiFlags, sizeof kAiFlags / sizeof *kAiFlags, false);
    b->ai_family = p->ai_family;
    b->ai_socktype = p->ai_socktype;
    b->ai_protocol = p->ai_protocol;
    b->ai_addrlen = p->ai_addrlen;
    b->ai_addr = reinterpret_cast<sockaddr*>(b + 1);
    memcpy(b->ai_addr, p->ai_addr, p->ai_addrlen);
    if (name_len) {
      b->ai_canonname = reinterpret_cast<char*>(b + 1) + p->ai_addrlen;
      memcpy(b->ai_canonname, p->ai_canonname, name_len);
    }
    *tail = b;
    tail = &b->ai_next;
  }
  if (getenv("TONIC_LOG_DNS") && g) {
    char ip[64] = "?";
    if (g->ai_family == AF_INET)
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(g->ai_addr)->sin_addr, ip, sizeof ip);
    else if (g->ai_family == AF_INET6)
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(g->ai_addr)->sin6_addr, ip, sizeof ip);
    fprintf(stderr, "[dns] %s:%s -> %s\n", node ? node : "(null)", service ? service : "", ip);
  }
  freeaddrinfo(g);
  return 0;
}

void sh_freeaddrinfo(BionicAddrinfo* a) {
  while (a) {
    BionicAddrinfo* next = a->ai_next;
    free(a);
    a = next;
  }
}

const char* sh_gai_strerror(int e) { return gai_strerror(eai_to_glibc(e)); }

int sh_getnameinfo(const sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv, socklen_t servlen,
                   int flags) {
  const int r = getnameinfo(sa, salen, host, hostlen, serv, servlen,
                            map_flags(flags, kNiFlags, sizeof kNiFlags / sizeof *kNiFlags, true));
  return eai_to_bionic(r);
}

}  // namespace

void init_shims(bool abort_on_stub) {
  g_abort_on_stub = abort_on_stub;
  // The library imports EGL and GLES by name (libEGL.so, libGLESv2.so on
  // Android). Load the host's (libglvnd/Mesa) globally so those names resolve
  // to it; the few calls that need translating are shimmed in window.cpp.
  for (const char* lib : {"libEGL.so.1", "libGLESv2.so.2"})
    if (!dlopen(lib, RTLD_NOW | RTLD_GLOBAL)) fprintf(stderr, "[shims] %s not loaded: %s\n", lib, dlerror());
  if (const char* s = getenv("TONIC_BIONIC_FILE_SIZE")) g_file_size = strtoul(s, nullptr, 0);
  if (getrandom(&g_stack_chk_guard, sizeof g_stack_chk_guard, 0) < 0) g_stack_chk_guard = 0x5f3a9c71d2e4b608ULL;
  g_stack_chk_guard &= ~0xffULL;  // low byte zero, like a normal canary

  reg("abort", &sh_abort);
  reg("sysconf", &sh_sysconf);
  reg("sigemptyset", &sh_sigemptyset);
  reg("sigfillset", &sh_sigfillset);
  reg("sigaction", &sh_sigaction);
  reg("pthread_sigmask", &sh_pthread_sigmask);
  // bionic x86_64 jmp_buf is 11 longs (88 bytes); glibc setjmp() also saves the
  // signal mask and writes 200. glibc _setjmp() writes only registers plus a
  // "mask not saved" flag (68 bytes), which fits. setjmp cannot be wrapped in a
  // C function (its frame would be gone on return), so point straight at it.
  if (void* p = dlsym(RTLD_DEFAULT, "_setjmp")) table()["setjmp"] = p;
  reg("dl_iterate_phdr", &sh_dl_iterate_phdr);
  reg("__errno", &sh_errno);
  reg("__system_property_get", &sh_property_get);
  reg("android_set_abort_message", &sh_set_abort_message);
  reg("__assert2", &sh_assert2);
  reg("__gnu_strerror_r", &sh_gnu_strerror_r);
  reg("__strlen_chk", &sh_strlen_chk);
  reg("__strchr_chk", &sh_strchr_chk);
  reg("__strncpy_chk2", &sh_strncpy_chk2);
  reg("__FD_SET_chk", &sh_fd_set_chk);
  reg("__FD_CLR_chk", &sh_fd_clr_chk);
  reg("__FD_ISSET_chk", &sh_fd_isset_chk);
  reg("__write_chk", &sh_write_chk);
  reg("getaddrinfo", &sh_getaddrinfo);
  reg("gethostbyname", &sh_gethostbyname);
  reg("freeaddrinfo", &sh_freeaddrinfo);
  reg("gai_strerror", &sh_gai_strerror);
  reg("getnameinfo", &sh_getnameinfo);
  reg("__umask_chk", &sh_umask_chk);
  reg("__sendto_chk", &sh_sendto_chk);
  reg("sendto", &sh_sendto);
  reg("sendmsg", &sh_sendmsg);
#ifdef __linux__
  reg("sendmmsg", &sh_sendmmsg);
#endif
  reg("connect", &sh_connect);
  reg("__fwrite_chk", &sh_fwrite_chk);

  reg("fprintf", &sh_fprintf);
  reg("vfprintf", &sh_vfprintf);
  reg("fputs", &sh_fputs);
  reg("fputc", &sh_fputc);
  reg("fwrite", &sh_fwrite);
  reg("fflush", &sh_fflush);

  reg("__android_log_write", &sh_log_write);
  reg("__android_log_buf_write", &sh_log_buf_write);
  reg("__android_log_vprint", &sh_log_vprint);
  reg("__android_log_print", &sh_log_print);
  reg("__android_log_assert", &sh_log_assert);

  reg("__stack_chk_guard", &g_stack_chk_guard);
  reg("__sF", g_sF);

  reg("AMEDIAFORMAT_KEY_BIT_RATE", &g_amf_bitrate);
  reg("AMEDIAFORMAT_KEY_CHANNEL_COUNT", &g_amf_channels);
  reg("AMEDIAFORMAT_KEY_COLOR_FORMAT", &g_amf_color);
  reg("AMEDIAFORMAT_KEY_FRAME_RATE", &g_amf_framerate);
  reg("AMEDIAFORMAT_KEY_HEIGHT", &g_amf_height);
  reg("AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", &g_amf_iframe);
  reg("AMEDIAFORMAT_KEY_MIME", &g_amf_mime);
  reg("AMEDIAFORMAT_KEY_SAMPLE_RATE", &g_amf_samplerate);
  reg("AMEDIAFORMAT_KEY_STRIDE", &g_amf_stride);
  reg("AMEDIAFORMAT_KEY_WIDTH", &g_amf_width);
}

uint64_t resolve_symbol(const std::string& name, bool weak) {
  auto it = table().find(name);
  if (it != table().end()) {
    ++g_stats.shim;
    return reinterpret_cast<uint64_t>(it->second);
  }
  if (void* p = dlsym(RTLD_DEFAULT, name.c_str())) {
    ++g_stats.host;
    Dl_info di{};
    const char* lib = (dladdr(p, &di) && di.dli_fname) ? di.dli_fname : "?";
    if (const char* slash = strrchr(lib, '/')) lib = slash + 1;
    g_host.push_back({name, lib});
    return reinterpret_cast<uint64_t>(p);
  }
  if (weak) {
    ++g_stats.weak_null;
    return 0;
  }
  ++g_stats.stubbed;
  g_stubbed.push_back(name);
  return make_stub(name);
}

uint64_t make_logging_stub(const std::string& name) { return make_stub(name); }

const std::vector<std::string>& stubbed_symbols() { return g_stubbed; }
const ShimStats& shim_stats() { return g_stats; }
const std::vector<HostSym>& host_symbols() { return g_host; }

void register_image(const std::string& name, uint64_t base, uint64_t span, const Elf64_Phdr* phdrs, size_t phnum) {
  g_images.push_back({name, base, span, phdrs, phnum});
}

static const Image* image_for(uint64_t a) {
  for (const Image& im : g_images)
    for (size_t i = 0; i < im.phnum; ++i) {
      const Elf64_Phdr& p = im.phdrs[i];
      if (p.p_type == PT_LOAD && a >= im.base + p.p_vaddr && a < im.base + p.p_vaddr + p.p_memsz) return &im;
    }
  return nullptr;
}

bool in_image_code(uint64_t a) {
  for (const Image& im : g_images)
    for (size_t i = 0; i < im.phnum; ++i) {
      const Elf64_Phdr& p = im.phdrs[i];
      if (p.p_type == PT_LOAD && (p.p_flags & PF_X) && a >= im.base + p.p_vaddr &&
          a < im.base + p.p_vaddr + p.p_memsz)
        return true;
    }
  return false;
}

void describe_addr(uint64_t a, char* buf, size_t n) {
  if (const Image* im = image_for(a)) {
    const char* nm = strrchr(im->name.c_str(), '/');
    snprintf(buf, n, "%s+%#lx", nm ? nm + 1 : im->name.c_str(), a - im->base);
    return;
  }
  Dl_info di{};
  if (dladdr(reinterpret_cast<void*>(a), &di) && di.dli_fname) {
    const char* lib = strrchr(di.dli_fname, '/');
    lib = lib ? lib + 1 : di.dli_fname;
    if (di.dli_sname)
      snprintf(buf, n, "%s!%s+%#lx", lib, di.dli_sname, a - reinterpret_cast<uint64_t>(di.dli_saddr));
    else
      snprintf(buf, n, "%s+%#lx", lib, a - reinterpret_cast<uint64_t>(di.dli_fbase));
    return;
  }
  snprintf(buf, n, "%#lx", a);
}

}  // namespace tonic
