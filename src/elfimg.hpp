// elfimg.hpp - minimal x86_64 ELF shared-object loader for Android-built libraries.
//
// Maps PT_LOAD segments, applies relocations (RELA, JMPREL and Android packed
// APS2), protects RELRO, and runs constructors. It does not use the host's
// dynamic linker, so the library never sees glibc's loader.
#pragma once

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tonic {

struct Rela {
  uint64_t offset;
  uint64_t info;
  int64_t addend;
};

// Decode Android packed relocations ("APS2"). Returns false on malformed input.
bool decode_aps2(const uint8_t* data, size_t size, std::vector<Rela>& out, std::string& err);

// Index of the constructor currently running, for fault reports.
inline volatile int64_t g_current_init = -1;

struct RelocStats {
  size_t relative = 0, glob_dat = 0, jump_slot = 0, abs64 = 0;
};

class ElfImage {
 public:
  using Resolver = std::function<uint64_t(const std::string& name, bool weak)>;

  bool load(const std::string& path, std::string& err);
  bool relocate(const Resolver& resolve, std::string& err);
  bool protect_relro(std::string& err);
  // Runs DT_INIT then DT_INIT_ARRAY. argc/argv/envp are passed like bionic does.
  void run_init(int argc, char** argv, char** envp, bool verbose);

  void* find_symbol(const char* name) const;
  uint64_t base() const { return base_; }
  uint64_t mapped_size() const { return span_; }
  size_t init_count() const { return init_array_count_; }
  const RelocStats& stats() const { return stats_; }
  const std::vector<std::string>& needed() const { return needed_; }
  const std::vector<Elf64_Phdr>& phdrs() const { return phdrs_; }

 private:
  const Elf64_Sym* gnu_lookup(const char* name) const;

  uint64_t base_ = 0;
  uint64_t span_ = 0;
  std::vector<Elf64_Phdr> phdrs_;

  const char* strtab_ = nullptr;
  const Elf64_Sym* symtab_ = nullptr;
  const uint32_t* gnu_hash_ = nullptr;

  const Elf64_Rela* rela_ = nullptr;
  size_t rela_bytes_ = 0;
  const Elf64_Rela* jmprel_ = nullptr;
  size_t jmprel_bytes_ = 0;
  const uint8_t* aps2_ = nullptr;
  size_t aps2_bytes_ = 0;

  void (*init_fn_)(int, char**, char**) = nullptr;
  const uint64_t* init_array_ = nullptr;
  size_t init_array_count_ = 0;

  RelocStats stats_;
  std::vector<std::string> needed_;
};

}  // namespace tonic
