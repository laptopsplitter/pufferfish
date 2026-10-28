#include "platform.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#ifdef __FreeBSD__
#include <fcntl.h>
#include <pthread_np.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#else
#include <sys/syscall.h>
#endif

namespace tonic {

std::string self_exe() {
#ifdef __FreeBSD__
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  char buf[4096];
  size_t len = sizeof buf;
  if (sysctl(mib, 4, buf, &len, nullptr, 0) != 0 || len == 0) return {};
  return std::string(buf, strnlen(buf, len));
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

std::string share_file(const std::string& rel) {
  const char* s = getenv("PUFFERFISH_SHARE");
  return s && *s ? std::string(s) + "/" + rel : rel;
}

std::string launcher_path() {
  if (const char* l = getenv("PUFFERFISH_LAUNCHER"); l && *l) return l;
  char cwd[4096];
  return std::string(getcwd(cwd, sizeof cwd) ? cwd : ".") + "/run.sh";
}

std::string self_exec_path() {
#ifdef __FreeBSD__
  return self_exe();
#else
  return "/proc/self/exe";
#endif
}

std::vector<std::string> self_cmdline() {
  std::string all;
#ifdef __FreeBSD__
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, getpid()};
  size_t len = 0;
  if (sysctl(mib, 4, nullptr, &len, nullptr, 0) == 0 && len) {
    all.resize(len);
    if (sysctl(mib, 4, all.data(), &len, nullptr, 0) == 0) all.resize(len);
    else all.clear();
  }
#else
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  all.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
#endif
  std::vector<std::string> parts;
  for (size_t p = 0; p < all.size();) {
    const size_t z = all.find('\0', p);
    parts.push_back(all.substr(p, z - p));
    if (z == std::string::npos) break;
    p = z + 1;
  }
  return parts;
}

int platform_tid() {
#ifdef __FreeBSD__
  return pthread_getthreadid_np();
#else
  return static_cast<int>(gettid());
#endif
}

bool thread_stack(uint64_t& lo, uint64_t& hi) {
  pthread_attr_t at;
#ifdef __FreeBSD__
  pthread_attr_init(&at);
  if (pthread_attr_get_np(pthread_self(), &at) != 0) {
    pthread_attr_destroy(&at);
    return false;
  }
#else
  if (pthread_getattr_np(pthread_self(), &at) != 0) return false;
#endif
  void* p = nullptr;
  size_t sz = 0;
  const bool ok = pthread_attr_getstack(&at, &p, &sz) == 0;
  pthread_attr_destroy(&at);
  if (!ok) return false;
  lo = reinterpret_cast<uint64_t>(p);
  hi = lo + sz;
  return true;
}

bool safe_read(uint64_t addr, void* dst, size_t n) {
#ifdef __FreeBSD__
  // No process_vm_readv: write() into a pipe fails with EFAULT on an
  // unreadable address instead of faulting. (Signal-safe: only syscalls.)
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) return false;
  bool ok = write(p[1], reinterpret_cast<const void*>(addr), n) == static_cast<ssize_t>(n) &&
            read(p[0], dst, n) == static_cast<ssize_t>(n);
  close(p[0]);
  close(p[1]);
  return ok;
#else
  iovec local{dst, n}, remote{reinterpret_cast<void*>(addr), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
#endif
}

void signal_regs(const void* ctx, uint64_t& ip, uint64_t& sp) {
  const auto* uc = static_cast<const ucontext_t*>(ctx);
#ifdef __FreeBSD__
  ip = static_cast<uint64_t>(uc->uc_mcontext.mc_rip);
  sp = static_cast<uint64_t>(uc->uc_mcontext.mc_rsp);
#else
  ip = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  sp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
#endif
}

}  // namespace tonic
