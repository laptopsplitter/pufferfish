#pragma once
// bsdabi.hpp - FreeBSD only: bionic's view of libc on top of FreeBSD's libc,
// translating numbers and struct layouts both ways. Translates only.

namespace tonic {

// Registers the translations. Call once, after every other register_*_shims()
// (it wraps some of theirs: getaddrinfo, getnameinfo, dlopen, dlsym) and before
// the library is relocated.
void init_bsd_abi();

// A Linux system call the library made with its own `syscall` instruction
// (bsdsys.cpp, through the tonicsud kernel module), carried out with the same translations as
// everything else. Returns the Linux result: the value, or -(Linux errno).
long bsd_raw_syscall(long nr, const long args[6]);

}  // namespace tonic
