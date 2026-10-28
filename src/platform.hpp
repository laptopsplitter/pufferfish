#pragma once
// The few things tonic itself needs from the OS that Linux and FreeBSD do
// differently. On Linux each one is exactly what tonic did before.
// (This is tonic's own side; what Roblox's library expects from Android is
// shims.cpp's job.)
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tonic {

std::string self_exe();                  // this program's path (Linux: /proc/self/exe)
// What to exec to start this program again. Linux: "/proc/self/exe" itself,
// which still works after the file was rebuilt; FreeBSD: self_exe().
std::string self_exec_path();
std::vector<std::string> self_cmdline();  // its arguments (Linux: /proc/self/cmdline)
// One of tonic's own files (assets/, scripts/, bsdkmod/): from the installed
// copy (TONIC_SHARE, set by the installed launcher), else the folder it runs in.
std::string share_file(const std::string& rel);
// What starts tonic from outside: the installed launcher (TONIC_LAUNCHER), else ./run.sh here.
std::string launcher_path();
int platform_tid();                       // this thread's id, for logs
bool thread_stack(uint64_t& lo, uint64_t& hi);  // this thread's stack
// Reads N bytes at ADDR without faulting (false: not readable).
bool safe_read(uint64_t addr, void* dst, size_t n);
// From a signal handler's context: the instruction and stack pointers.
void signal_regs(const void* ucontext, uint64_t& ip, uint64_t& sp);

}  // namespace tonic
