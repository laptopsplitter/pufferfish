// openbsd_abi.cpp - Pufferfish OpenBSD Bionic/Linux-to-OpenBSD ABI translation layer.
//
// Translates Linux/Bionic errno numbers, mmap flags (including OpenBSD's
// mandatory MAP_STACK for stack allocations and W^X transition rules),
// native OpenBSD futex(2) syscalls (<sys/futex.h>), /proc virtual files
// via OpenBSD sysctl(2), and statfs(2) f_type = 0x9fa0 (PROC_SUPER_MAGIC)
// so Roblox's integrity checks pass without disconnecting (Error 304).

#ifdef __OpenBSD__

#include "bsdabi.hpp"
#include "bsdsys.hpp"
#include "shims.hpp"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/futex.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace tonic {

namespace {

// Linux x86_64 syscall numbers invoked directly by libroblox.so
constexpr long kLinux_openat        = 257;
constexpr long kLinux_fstatfs       = 138;
constexpr long kLinux_statfs        = 137;
constexpr long kLinux_futex         = 202;
constexpr long kLinux_gettid        = 186;
constexpr long kLinux_tgkill        = 234;
constexpr long kLinux_getrandom     = 318;
constexpr long kLinux_clock_gettime = 228;

// Linux procfs magic expected by Roblox's anti-tamper check on /proc
constexpr uint64_t kLinuxProcSuperMagic = 0x9fa0;

struct LinuxStatfs64 {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  int32_t  f_fsid[2];
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
};

int bsd_to_linux_errno(int e) {
  switch (e) {
    case EAGAIN:        return 11;
    case EDEADLK:       return 35;
    case ENAMETOOLONG:  return 36;
    case ENOSYS:        return 38;
    case ENOTEMPTY:     return 39;
    case ELOOP:         return 40;
    case ENOTSOCK:      return 88;
    case EMSGSIZE:      return 90;
    case EOPNOTSUPP:    return 95;
    case EADDRINUSE:    return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN:      return 100;
    case ENETUNREACH:   return 101;
    case ECONNABORTED:  return 103;
    case ECONNRESET:    return 104;
    case ENOBUFS:       return 105;
    case EISCONN:       return 106;
    case ENOTCONN:      return 107;
    case ETIMEDOUT:     return 110;
    case ECONNREFUSED:  return 111;
    case EHOSTUNREACH:  return 113;
    case EALREADY:      return 114;
    case EINPROGRESS:   return 115;
    default:            return e;
  }
}

// Synthesizes /proc/meminfo, /proc/cpuinfo, /proc/self/maps, /proc/self/status
// in memory using mkstemp for libroblox.so reads.
int open_virtual_proc(const char* path) {
  char content[4096];
  int len = 0;

  if (strcmp(path, "/proc/meminfo") == 0) {
    int mib[2] = {CTL_HW, HW_PHYSMEM64};
    int64_t physmem = 8ULL * 1024 * 1024 * 1024;
    size_t sz = sizeof(physmem);
    sysctl(mib, 2, &physmem, &sz, nullptr, 0);
    unsigned long long kb = static_cast<unsigned long long>(physmem) / 1024ULL;
    len = snprintf(content, sizeof(content),
                   "MemTotal:       %llu kB\n"
                   "MemFree:        %llu kB\n"
                   "MemAvailable:   %llu kB\n"
                   "Buffers:        0 kB\n"
                   "Cached:         %llu kB\n",
                   kb, kb / 2, (kb * 3) / 4, kb / 4);
  } else if (strcmp(path, "/proc/cpuinfo") == 0) {
    int mib_ncpu[2] = {CTL_HW, HW_NCPUONLINE};
    int ncpu = 4;
    size_t sz = sizeof(ncpu);
    sysctl(mib_ncpu, 2, &ncpu, &sz, nullptr, 0);

    char model[128] = "OpenBSD amd64 Processor";
    int mib_model[2] = {CTL_HW, HW_MODEL};
    size_t msz = sizeof(model);
    sysctl(mib_model, 2, model, &msz, nullptr, 0);

    for (int i = 0; i < ncpu && len < static_cast<int>(sizeof(content)) - 200; ++i) {
      len += snprintf(content + len, sizeof(content) - len,
                      "processor\t: %d\nvendor_id\t: GenuineIntel\nmodel name\t: %s\n\n",
                      i, model);
    }
  } else if (strcmp(path, "/proc/self/status") == 0) {
    len = snprintf(content, sizeof(content),
                   "Name:\tpufferfish\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nTracerPid:\t0\n",
                   getpid(), getpid());
  } else if (strcmp(path, "/proc/self/cmdline") == 0) {
    len = snprintf(content, sizeof(content), "com.roblox.client");
  } else {
    return -1;
  }

  char tmp[] = "/tmp/pufferfish-proc-XXXXXX";
  int fd = mkstemp(tmp);
  if (fd < 0) return -1;
  unlink(tmp);
  if (write(fd, content, len) != len) {
    close(fd);
    return -1;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

// OpenBSD native futex(2) translation from Linux futex opcodes
long openbsd_futex_shim(uint32_t* uaddr, int op, uint32_t val, const struct timespec* timeout, uint32_t* uaddr2) {
  const int cmd = op & 0x7f;
  int obsd_op = 0;
  if (cmd == 0 || cmd == 9) { // FUTEX_WAIT / FUTEX_WAIT_BITSET
    obsd_op = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
  } else if (cmd == 1 || cmd == 10) { // FUTEX_WAKE / FUTEX_WAKE_BITSET
    obsd_op = FUTEX_WAKE | FUTEX_PRIVATE_FLAG;
  } else if (cmd == 3) { // FUTEX_REQUEUE
    obsd_op = FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG;
  } else {
    errno = ENOSYS;
    return -1;
  }
  return futex(uaddr, obsd_op, static_cast<int>(val), timeout, uaddr2);
}

// Linux mmap wrapper translating Linux MAP_ANONYMOUS (0x20) and MAP_STACK (0x20000)
// to OpenBSD's MAP_ANON (0x1000) and MAP_STACK (0x4000).
void* sh_openbsd_mmap(void* addr, size_t len, int prot, int linux_flags, int fd, off_t offset) {
  int bsd_flags = 0;
  if (linux_flags & 0x01) bsd_flags |= MAP_SHARED;
  if (linux_flags & 0x02) bsd_flags |= MAP_PRIVATE;
  if (linux_flags & 0x10) bsd_flags |= MAP_FIXED;
  if (linux_flags & 0x20) bsd_flags |= MAP_ANON;
  if (linux_flags & 0x20000) bsd_flags |= MAP_STACK;

  void* res = mmap(addr, len, prot, bsd_flags, fd, offset);
  return res;
}

} // namespace

long bsd_raw_syscall(long nr, const long a[6]) {
  switch (nr) {
    case kLinux_openat: {
      const int dirfd = static_cast<int>(a[0]);
      const char* path = reinterpret_cast<const char*>(a[1]);
      const int linux_flags = static_cast<int>(a[2]);
      const mode_t mode = static_cast<mode_t>(a[3]);

      if (path && strncmp(path, "/proc/", 6) == 0) {
        int vfd = open_virtual_proc(path);
        if (vfd >= 0) return vfd;
        if (strcmp(path, "/proc") == 0 || strcmp(path, "/proc/self") == 0) {
          int dfd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          return dfd >= 0 ? dfd : -bsd_to_linux_errno(errno);
        }
        return -2; // -ENOENT
      }

      int bsd_flags = linux_flags & (O_RDONLY | O_WRONLY | O_RDWR);
      if (linux_flags & 0100)     bsd_flags |= O_CREAT;
      if (linux_flags & 0200)     bsd_flags |= O_EXCL;
      if (linux_flags & 01000)    bsd_flags |= O_TRUNC;
      if (linux_flags & 02000)    bsd_flags |= O_APPEND;
      if (linux_flags & 04000)    bsd_flags |= O_NONBLOCK;
      if (linux_flags & 0200000)  bsd_flags |= O_DIRECTORY;
      if (linux_flags & 02000000) bsd_flags |= O_CLOEXEC;

      const int BionicAtFdcwd = -100;
      int fd = openat(dirfd == BionicAtFdcwd ? AT_FDCWD : dirfd, path, bsd_flags, mode);
      return fd >= 0 ? fd : -bsd_to_linux_errno(errno);
    }

    case kLinux_fstatfs:
    case kLinux_statfs: {
      auto* out = reinterpret_cast<LinuxStatfs64*>(a[1]);
      if (!out) return -14; // -EFAULT
      memset(out, 0, sizeof(*out));
      // Report Linux PROC_SUPER_MAGIC (0x9fa0) so Roblox's /proc verification passes
      out->f_type    = kLinuxProcSuperMagic;
      out->f_bsize   = 4096;
      out->f_frsize  = 4096;
      out->f_namelen = 255;
      return 0;
    }

    case kLinux_futex: {
      long r = openbsd_futex_shim(
          reinterpret_cast<uint32_t*>(a[0]),
          static_cast<int>(a[1]),
          static_cast<uint32_t>(a[2]),
          reinterpret_cast<const struct timespec*>(a[3]),
          reinterpret_cast<uint32_t*>(a[4]));
      return r == 0 ? 0 : -bsd_to_linux_errno(errno);
    }

    case kLinux_gettid:
      return static_cast<long>(getthrid());

    case kLinux_getrandom: {
      void* buf = reinterpret_cast<void*>(a[0]);
      size_t buflen = static_cast<size_t>(a[1]);
      if (!buf) return -14;
      arc4random_buf(buf, buflen);
      return static_cast<long>(buflen);
    }

    case kLinux_clock_gettime: {
      clockid_t clk = (a[0] == 0) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
      auto* tp = reinterpret_cast<struct timespec*>(a[1]);
      return clock_gettime(clk, tp) == 0 ? 0 : -bsd_to_linux_errno(errno);
    }

    default:
      return -38; // -ENOSYS
  }
}

void init_bsd_abi() {
  register_shim("mmap", reinterpret_cast<void*>(sh_openbsd_mmap));
  register_shim("mmap64", reinterpret_cast<void*>(sh_openbsd_mmap));
  fprintf(stderr, "[pufferfish_abi] registered OpenBSD Bionic/Linux ABI shims (futex, mmap/W^X, /proc, statfs 0x9fa0)\n");
}

} // namespace pufferfish

#endif // __OpenBSD__
