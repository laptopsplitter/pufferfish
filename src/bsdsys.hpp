#pragma once
// bsdsys.hpp - FreeBSD only: Linux `syscall` instructions from Roblox's code,
// caught by the tonicsud kernel module (SIGEMT) and run through bsdabi's
// translations. TONIC_SYSCALLS=off leaves the module unused.

#include <cstdint>
#include <string>

namespace tonic {

// Call early in main. Finds the module and turns dispatch on for this process.
void bsdsys_start();

enum class BsdModule { on, off, missing, old, failed };  // off: TONIC_SYSCALLS=off
BsdModule bsdsys_state();
// Looks for the module again (once it is loaded while tonic runs). Cheap
// enough to call every second.
BsdModule bsdsys_retry();

// For the launcher's setup page (menu.cpp): where the module is (or will be,
// once built), and the one command that loads it, to run as root: it builds
// it first when that has not been done, and unloads an older one.
std::string bsdsys_module_path();
std::string bsdsys_load_command();
bool bsdsys_sources_missing();  // not built, and no kernel sources to build it from

// Code at [lo, hi) is the library's: its direct syscalls are Linux's.
void bsdsys_add_range(uint64_t lo, uint64_t hi);

// [lo, hi) is no longer the library's code (unmapped, or not executable now).
void bsdsys_remove_range(uint64_t lo, uint64_t hi);

}  // namespace tonic
