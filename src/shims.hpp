// shims.hpp - the symbols an Android-built library expects from bionic and the
// Android platform, provided on top of glibc.
#pragma once

#include <elf.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tonic {

struct ShimStats {
  size_t shim = 0;       // implemented here (bionic differences, logging, data)
  size_t host = 0;       // taken from glibc as-is
  size_t weak_null = 0;  // weak and unavailable: resolved to null
  size_t stubbed = 0;    // not implemented: logging stub
};

void init_shims(bool abort_on_stub);

// Adds or replaces a symbol the library will be given instead of the host's.
// Call before relocation (window.cpp uses it for ANativeWindow_* and EGL).
void register_shim(const char* name, void* fn);
// What is registered for NAME so far (null if nothing), so a later layer can
// wrap it (bsdabi.cpp).
void* shim_lookup(const char* name);

// Resolution policy: shim table, then glibc, then null if weak, else a stub
// that logs its name when called.
uint64_t resolve_symbol(const std::string& name, bool weak);

const std::vector<std::string>& stubbed_symbols();

// A thunk that logs "[stub] <name> called" and returns 0 (or aborts with
// --abort-on-stub). Used for unresolved imports and unimplemented JNI slots.
uint64_t make_logging_stub(const std::string& name);
const ShimStats& shim_stats();

// A symbol we took from the host, and the host library it came from.
struct HostSym {
  std::string name;
  std::string lib;
};
const std::vector<HostSym>& host_symbols();

// Images we mapped ourselves. They are invisible to glibc's dl_iterate_phdr and
// dladdr, so we keep our own list: the dl_iterate_phdr shim reports them (the
// library's own unwinder needs that to find .eh_frame), and crash reports use
// it to turn addresses into image offsets.
void register_image(const std::string& name, uint64_t base, uint64_t span, const Elf64_Phdr* phdrs, size_t phnum);
// Writes "name+0xOFF" for an address inside a registered image, else the host
// symbol from dladdr, else the raw address. Not async-signal-safe (dladdr), but
// good enough for a crash report.
void describe_addr(uint64_t addr, char* buf, size_t n);
bool in_image_code(uint64_t addr);

}  // namespace tonic
