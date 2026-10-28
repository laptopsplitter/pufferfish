#include "elfimg.hpp"
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include "bsdsys.hpp"
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>

// FreeBSD's names and flags (Linux's are used as they are).
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0  // (FreeBSD reserves nothing up front anyway)
#endif
#if !defined(R_X86_64_JUMP_SLOT) && defined(R_X86_64_JMP_SLOT)
#define R_X86_64_JUMP_SLOT R_X86_64_JMP_SLOT
#endif

namespace tonic {

namespace {

constexpr uint64_t kPage = 4096;
inline uint64_t page_down(uint64_t v) { return v & ~(kPage - 1); }
inline uint64_t page_up(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

constexpr int64_t kDtAndroidRel = 0x6000000f;
constexpr int64_t kDtAndroidRela = 0x60000011;
constexpr int64_t kDtAndroidRelaSz = 0x60000012;
constexpr int64_t kDtAndroidRelr = 0x6fffe000;
constexpr int64_t kDtRelr = 36;

struct SlebReader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;

  int64_t next() {
    uint64_t result = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
      if (p >= end) {
        ok = false;
        return 0;
      }
      b = *p++;
      if (shift < 64) result |= static_cast<uint64_t>(b & 0x7f) << shift;
      shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) result |= ~0ULL << shift;
    return static_cast<int64_t>(result);
  }
};

}  // namespace

bool decode_aps2(const uint8_t* data, size_t size, std::vector<Rela>& out, std::string& err) {
  if (size < 4 || memcmp(data, "APS2", 4) != 0) {
    err = "packed relocations: missing APS2 magic";
    return false;
  }
  SlebReader r{data + 4, data + size};
  const uint64_t total = static_cast<uint64_t>(r.next());
  Rela cur{static_cast<uint64_t>(r.next()), 0, 0};
  out.reserve(total);

  uint64_t done = 0;
  while (done < total) {
    const uint64_t group_size = static_cast<uint64_t>(r.next());
    const uint64_t flags = static_cast<uint64_t>(r.next());
    const bool by_info = flags & 1, by_delta = flags & 2, by_addend = flags & 4, has_addend = flags & 8;

    uint64_t delta = 0;
    if (by_delta) delta = static_cast<uint64_t>(r.next());
    if (by_info) cur.info = static_cast<uint64_t>(r.next());
    if (has_addend && by_addend) {
      cur.addend += r.next();
    } else if (!has_addend) {
      cur.addend = 0;
    }

    for (uint64_t i = 0; i < group_size; ++i) {
      cur.offset += by_delta ? delta : static_cast<uint64_t>(r.next());
      if (!by_info) cur.info = static_cast<uint64_t>(r.next());
      if (has_addend && !by_addend) cur.addend += r.next();
      out.push_back(cur);
    }
    done += group_size;
    if (!r.ok) {
      err = "packed relocations: truncated data";
      return false;
    }
  }
  return true;
}

bool ElfImage::load(const std::string& path, std::string& err) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    err = "cannot open " + path;
    return false;
  }

  Elf64_Ehdr eh;
  if (pread(fd, &eh, sizeof eh, 0) != static_cast<ssize_t>(sizeof eh) ||
      memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_X86_64 || eh.e_type != ET_DYN) {
    err = "not a 64-bit x86_64 shared object";
    close(fd);
    return false;
  }

  phdrs_.resize(eh.e_phnum);
  if (pread(fd, phdrs_.data(), eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff) !=
      static_cast<ssize_t>(eh.e_phnum * sizeof(Elf64_Phdr))) {
    err = "cannot read program headers";
    close(fd);
    return false;
  }

  uint64_t lo = ~0ULL, hi = 0, align = kPage;
  for (const auto& p : phdrs_) {
    if (p.p_type != PT_LOAD) continue;
    lo = std::min<uint64_t>(lo, page_down(p.p_vaddr));
    hi = std::max<uint64_t>(hi, page_up(p.p_vaddr + p.p_memsz));
    align = std::max<uint64_t>(align, p.p_align);
  }
  if (lo == ~0ULL) {
    err = "no PT_LOAD segments";
    close(fd);
    return false;
  }
  span_ = hi - lo;

  // Reserve an address range aligned to the library's own alignment (16 KB for
  // recent Android builds), then trim the slack.
  void* raw = mmap(nullptr, span_ + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) {
    err = "cannot reserve address space";
    close(fd);
    return false;
  }
  const uint64_t raw_addr = reinterpret_cast<uint64_t>(raw);
  const uint64_t start = (raw_addr + align - 1) & ~(align - 1);
  if (start > raw_addr) munmap(raw, start - raw_addr);
  const uint64_t tail = raw_addr + span_ + align - (start + span_);
  if (tail) munmap(reinterpret_cast<void*>(start + span_), tail);
  base_ = start - lo;

  for (const auto& p : phdrs_) {
    if (p.p_type != PT_LOAD) continue;
    int prot = 0;
    if (p.p_flags & PF_R) prot |= PROT_READ;
    if (p.p_flags & PF_W) prot |= PROT_WRITE;
    if (p.p_flags & PF_X) prot |= PROT_EXEC;

    const uint64_t seg_start = page_down(p.p_vaddr);
    const uint64_t file_end = p.p_vaddr + p.p_filesz;
    const uint64_t map_len = page_up(file_end) - seg_start;
    if (map_len) {
      void* m = mmap(reinterpret_cast<void*>(base_ + seg_start), map_len, prot, MAP_PRIVATE | MAP_FIXED, fd,
                     page_down(p.p_offset));
      if (m == MAP_FAILED) {
        err = "mmap of a segment failed";
        close(fd);
        return false;
      }
    }
    if (p.p_memsz > p.p_filesz) {
      // Zero the tail of the last file-backed page, then add anonymous pages.
      if ((p.p_flags & PF_W) && (file_end & (kPage - 1)))
        memset(reinterpret_cast<void*>(base_ + file_end), 0, page_up(file_end) - file_end);
      const uint64_t bss_start = page_up(file_end);
      const uint64_t bss_end = page_up(p.p_vaddr + p.p_memsz);
      if (bss_end > bss_start) {
        void* m = mmap(reinterpret_cast<void*>(base_ + bss_start), bss_end - bss_start, prot,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) {
          err = "mmap of bss failed";
          close(fd);
          return false;
        }
      }
    }
  }
  close(fd);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  bsdsys_add_range(base_ + lo, base_ + hi);  // Android code: its direct syscalls are Linux's
#endif

  // Dynamic section. Values are link-time addresses; add base_ ourselves.
  const Elf64_Phdr* dynp = nullptr;
  for (const auto& p : phdrs_)
    if (p.p_type == PT_DYNAMIC) dynp = &p;
  if (!dynp) {
    err = "no PT_DYNAMIC";
    return false;
  }

  uint64_t jmprel = 0, jmprel_sz = 0, rela = 0, rela_sz = 0, aps2 = 0, aps2_sz = 0;
  uint64_t init_array = 0, init_array_sz = 0, init = 0, strtab = 0, symtab = 0, gnuhash = 0;
  std::vector<uint64_t> needed_offsets;
  for (auto* d = reinterpret_cast<const Elf64_Dyn*>(base_ + dynp->p_vaddr); d->d_tag != DT_NULL; ++d) {
    const uint64_t v = d->d_un.d_ptr;
    switch (d->d_tag) {
      case DT_NEEDED: needed_offsets.push_back(d->d_un.d_val); break;
      case DT_STRTAB: strtab = v; break;
      case DT_SYMTAB: symtab = v; break;
      case DT_GNU_HASH: gnuhash = v; break;
      case DT_JMPREL: jmprel = v; break;
      case DT_PLTRELSZ: jmprel_sz = v; break;
      case DT_RELA: rela = v; break;
      case DT_RELASZ: rela_sz = v; break;
      case kDtAndroidRela: aps2 = v; break;
      case kDtAndroidRelaSz: aps2_sz = v; break;
      case DT_INIT: init = v; break;
      case DT_INIT_ARRAY: init_array = v; break;
      case DT_INIT_ARRAYSZ: init_array_sz = v; break;
      case DT_TEXTREL:
      case DT_REL:
      case kDtAndroidRel:
      case kDtRelr:
      case kDtAndroidRelr:
        err = "unsupported relocation feature in dynamic section (tag " + std::to_string(d->d_tag) + ")";
        return false;
      default: break;
    }
  }

  strtab_ = reinterpret_cast<const char*>(base_ + strtab);
  for (uint64_t off : needed_offsets) needed_.emplace_back(strtab_ + off);
  symtab_ = reinterpret_cast<const Elf64_Sym*>(base_ + symtab);
  if (gnuhash) gnu_hash_ = reinterpret_cast<const uint32_t*>(base_ + gnuhash);
  if (rela) rela_ = reinterpret_cast<const Elf64_Rela*>(base_ + rela), rela_bytes_ = rela_sz;
  if (jmprel) jmprel_ = reinterpret_cast<const Elf64_Rela*>(base_ + jmprel), jmprel_bytes_ = jmprel_sz;
  if (aps2) aps2_ = reinterpret_cast<const uint8_t*>(base_ + aps2), aps2_bytes_ = aps2_sz;
  if (init) init_fn_ = reinterpret_cast<void (*)(int, char**, char**)>(base_ + init);
  if (init_array) {
    init_array_ = reinterpret_cast<const uint64_t*>(base_ + init_array);
    init_array_count_ = init_array_sz / sizeof(uint64_t);
  }
  return true;
}

bool ElfImage::relocate(const Resolver& resolve, std::string& err) {
  std::vector<Rela> all;
  if (rela_) {
    for (size_t i = 0; i < rela_bytes_ / sizeof(Elf64_Rela); ++i)
      all.push_back({rela_[i].r_offset, rela_[i].r_info, rela_[i].r_addend});
  }
  if (aps2_ && !decode_aps2(aps2_, aps2_bytes_, all, err)) return false;
  if (jmprel_) {
    for (size_t i = 0; i < jmprel_bytes_ / sizeof(Elf64_Rela); ++i)
      all.push_back({jmprel_[i].r_offset, jmprel_[i].r_info, jmprel_[i].r_addend});
  }

  std::unordered_map<uint32_t, uint64_t> cache;
  auto sym_addr = [&](uint32_t idx) -> uint64_t {
    auto it = cache.find(idx);
    if (it != cache.end()) return it->second;
    const Elf64_Sym& s = symtab_[idx];
    uint64_t addr;
    if (s.st_shndx != SHN_UNDEF) {
      addr = base_ + s.st_value;
    } else {
      addr = resolve(strtab_ + s.st_name, ELF64_ST_BIND(s.st_info) == STB_WEAK);
    }
    cache.emplace(idx, addr);
    return addr;
  };

  for (const Rela& r : all) {
    auto* where = reinterpret_cast<uint64_t*>(base_ + r.offset);
    const uint32_t type = ELF64_R_TYPE(r.info);
    const uint32_t sym = ELF64_R_SYM(r.info);
    switch (type) {
      case R_X86_64_RELATIVE: *where = base_ + r.addend; ++stats_.relative; break;
      case R_X86_64_GLOB_DAT: *where = sym_addr(sym); ++stats_.glob_dat; break;
      case R_X86_64_JUMP_SLOT: *where = sym_addr(sym); ++stats_.jump_slot; break;
      case R_X86_64_64: *where = sym_addr(sym) + r.addend; ++stats_.abs64; break;
      case R_X86_64_NONE: break;
      default:
        err = "unsupported relocation type " + std::to_string(type);
        return false;
    }
  }
  return true;
}

bool ElfImage::protect_relro(std::string& err) {
  for (const auto& p : phdrs_) {
    if (p.p_type != PT_GNU_RELRO) continue;
    const uint64_t s = page_down(base_ + p.p_vaddr);
    const uint64_t e = page_up(base_ + p.p_vaddr + p.p_memsz);
    if (mprotect(reinterpret_cast<void*>(s), e - s, PROT_READ) != 0) {
      err = "mprotect of RELRO failed";
      return false;
    }
  }
  return true;
}

void ElfImage::run_init(int argc, char** argv, char** envp, bool verbose) {
  if (init_fn_) {
    g_current_init = -2;
    init_fn_(argc, argv, envp);
  }
  for (size_t i = 0; i < init_array_count_; ++i) {
    const uint64_t fn = init_array_[i];
    if (fn == 0 || fn == ~0ULL) continue;
    g_current_init = static_cast<int64_t>(i);
    if (verbose && i % 500 == 0) fprintf(stderr, "[init] %zu / %zu\n", i, init_array_count_);
    reinterpret_cast<void (*)(int, char**, char**)>(fn)(argc, argv, envp);
  }
  g_current_init = -1;
}

const Elf64_Sym* ElfImage::gnu_lookup(const char* name) const {
  if (!gnu_hash_) return nullptr;
  const uint32_t nbuckets = gnu_hash_[0], symoffset = gnu_hash_[1], bloom_size = gnu_hash_[2],
                 bloom_shift = gnu_hash_[3];
  const uint64_t* bloom = reinterpret_cast<const uint64_t*>(gnu_hash_ + 4);
  const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_size);
  const uint32_t* chain = buckets + nbuckets;

  uint32_t h = 5381;
  for (const char* c = name; *c; ++c) h = h * 33 + static_cast<uint8_t>(*c);

  const uint64_t word = bloom[(h / 64) % bloom_size];
  const uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> bloom_shift) % 64));
  if ((word & mask) != mask) return nullptr;

  uint32_t idx = buckets[h % nbuckets];
  if (idx < symoffset) return nullptr;
  for (;; ++idx) {
    const uint32_t ch = chain[idx - symoffset];
    if (((ch ^ h) >> 1) == 0) {
      const Elf64_Sym& s = symtab_[idx];
      if (s.st_shndx != SHN_UNDEF && strcmp(strtab_ + s.st_name, name) == 0) return &s;
    }
    if (ch & 1) break;
  }
  return nullptr;
}

void* ElfImage::find_symbol(const char* name) const {
  const Elf64_Sym* s = gnu_lookup(name);
  return s ? reinterpret_cast<void*>(base_ + s->st_value) : nullptr;
}

}  // namespace tonic
