// apkfiles.cpp - see apkfiles.hpp.
#include "apkfiles.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <vector>

namespace tonic {
namespace {

namespace fs = std::filesystem;

uint16_t le16(const uint8_t* b) { return static_cast<uint16_t>(b[0] | b[1] << 8); }
uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24; }

struct Zip {
  const uint8_t* p = nullptr;
  size_t n = 0;
  ~Zip() {
    if (p) munmap(const_cast<uint8_t*>(p), n);
  }
  bool open_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 22) {
      void* m = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
      if (m != MAP_FAILED) p = static_cast<const uint8_t*>(m), n = static_cast<size_t>(st.st_size);
    }
    ::close(fd);
    return p != nullptr;
  }
};

struct Entry {
  std::string name;
  uint16_t method;
  uint32_t csize, usize, crc;
  size_t data;  // where its (compressed) bytes start
};

// The entries, from the central directory (the part the APK signature covers).
bool entries(const Zip& z, std::vector<Entry>& out, std::string& err) {
  size_t eocd = SIZE_MAX;
  for (size_t i = z.n - 22 + 1; i-- > (z.n > 22 + 65535 ? z.n - 22 - 65535 : 0);)
    if (le32(z.p + i) == 0x06054b50 && i + 22 + le16(z.p + i + 20) == z.n) {
      eocd = i;
      break;
    }
  if (eocd == SIZE_MAX) return err = "not a zip file", false;
  const size_t count = le16(z.p + eocd + 10), cd = le32(z.p + eocd + 16), cd_size = le32(z.p + eocd + 12);
  if (count == 0xffff || cd == 0xffffffff || cd + cd_size > eocd) return err = "unsupported zip (zip64)", false;
  size_t at = cd;
  for (size_t k = 0; k < count; ++k) {
    if (at + 46 > eocd || le32(z.p + at) != 0x02014b50) return err = "damaged zip directory", false;
    Entry e;
    const uint16_t flags = le16(z.p + at + 8);
    e.method = le16(z.p + at + 10);
    e.crc = le32(z.p + at + 16), e.csize = le32(z.p + at + 20), e.usize = le32(z.p + at + 24);
    const size_t nl = le16(z.p + at + 28), xl = le16(z.p + at + 30), cl = le16(z.p + at + 32);
    const size_t local = le32(z.p + at + 42);
    if (at + 46 + nl > eocd) return err = "damaged zip directory", false;
    e.name.assign(reinterpret_cast<const char*>(z.p + at + 46), nl);
    at += 46 + nl + xl + cl;
    if (flags & 1) return err = "encrypted zip entry", false;
    if (local + 30 > cd || le32(z.p + local) != 0x04034b50) return err = "damaged zip entry", false;
    e.data = local + 30 + le16(z.p + local + 26) + le16(z.p + local + 28);
    if (e.data + e.csize > cd) return err = "damaged zip entry", false;
    out.push_back(e);
  }
  return true;
}

// The entry's contents, in pieces, to SINK (false from SINK stops it); CRC checked.
bool read_entry(const Zip& z, const Entry& e, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err) {
  uLong crc = crc32(0, nullptr, 0);
  if (e.method == 0) {
    if (e.csize != e.usize) return err = "damaged zip entry " + e.name, false;
    for (size_t off = 0; off < e.csize; off += 1 << 20) {
      const size_t len = std::min<size_t>(1 << 20, e.csize - off);
      crc = crc32(crc, z.p + e.data + off, static_cast<uInt>(len));
      if (!sink(z.p + e.data + off, len)) return false;
    }
  } else if (e.method == 8) {
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK) return err = "zlib", false;
    std::vector<uint8_t> buf(1 << 18);
    s.next_in = const_cast<Bytef*>(z.p + e.data);
    s.avail_in = e.csize;
    size_t total = 0;
    int r = Z_OK;
    while (r != Z_STREAM_END) {
      s.next_out = buf.data();
      s.avail_out = static_cast<uInt>(buf.size());
      r = inflate(&s, Z_NO_FLUSH);
      if (r != Z_OK && r != Z_STREAM_END) {
        inflateEnd(&s);
        return err = "damaged zip entry " + e.name, false;
      }
      const size_t got = buf.size() - s.avail_out;
      total += got;
      crc = crc32(crc, buf.data(), static_cast<uInt>(got));
      if (got && !sink(buf.data(), got)) {
        inflateEnd(&s);
        return false;
      }
      if (r != Z_STREAM_END && got == 0 && s.avail_in == 0) {
        inflateEnd(&s);
        return err = "damaged zip entry " + e.name, false;
      }
    }
    inflateEnd(&s);
    if (total != e.usize) return err = "damaged zip entry " + e.name, false;
  } else {
    return err = "unsupported zip compression in " + e.name, false;
  }
  if (crc != e.crc) return err = "damaged zip entry " + e.name, false;
  return true;
}

// Where an entry goes under DIR ("" if it is not one tonic installs): lib/x86_64/X -> lib/X, assets/... as is.
std::string target(const std::string& name, bool from_arch, bool from_base) {
  if (name.empty() || name.back() == '/' || name[0] == '/' || name.find('\\') != std::string::npos) return {};
  for (size_t i = 0; (i = name.find("..", i)) != std::string::npos; ++i)
    if ((i == 0 || name[i - 1] == '/') && (i + 2 == name.size() || name[i + 2] == '/')) return {};
  if (from_arch && name.rfind("lib/x86_64/", 0) == 0) {
    const std::string base = name.substr(11);
    return base.empty() || base.find('/') != std::string::npos ? "" : "lib/" + base;
  }
  if (from_base && name.rfind("assets/", 0) == 0) return name;
  return {};
}

// Walks what tonic installs from the APKs: FN(relative path, zip, entry).
bool walk(const std::string& base, const std::string& arch,
          const std::function<bool(const std::string&, const Zip&, const Entry&)>& fn, std::string& err) {
  for (int which = 0; which < (base == arch ? 1 : 2); ++which) {
    const std::string& path = which == 0 ? base : arch;
    Zip z;
    if (!z.open_file(path)) return err = "cannot read " + path, false;
    std::vector<Entry> list;
    if (!entries(z, list, err)) return false;
    const bool is_base = path == base, is_arch = path == arch;
    for (const Entry& e : list) {
      const std::string t = target(e.name, is_arch, is_base);
      if (!t.empty() && !fn(t, z, e)) return false;
    }
  }
  return true;
}

}  // namespace

bool apk_unpack(const std::string& base, const std::string& arch, const std::string& dir, std::string& err) {
  std::error_code ec;
  return walk(base, arch, [&](const std::string& rel, const Zip& z, const Entry& e) {
    const fs::path out = fs::path(dir) / rel;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) return err = "cannot write " + out.string(), false;
    if (!read_entry(z, e, [&](const uint8_t* d, size_t n) { f.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n)); return bool(f); }, err))
      return err.empty() ? (err = "cannot write " + out.string(), false) : false;
    return true;
  }, err);
}

bool apk_files_match(const std::string& base, const std::string& arch, const std::string& dir, std::string& err) {
  std::set<std::string> expected;
  std::vector<uint8_t> have(1 << 20);
  const bool ok = walk(base, arch, [&](const std::string& rel, const Zip& z, const Entry& e) {
    expected.insert(rel);
    const std::string path = dir + "/" + rel;
    std::ifstream f(path, std::ios::binary);
    struct stat st{};
    if (!f || stat(path.c_str(), &st) != 0 || static_cast<uint64_t>(st.st_size) != e.usize)
      return err = rel + (f ? " was changed" : " is missing"), false;
    bool same = true;
    if (!read_entry(z, e, [&](const uint8_t* d, size_t n) {  // compared as it is read, piece by piece
          if (have.size() < n) have.resize(n);
          f.read(reinterpret_cast<char*>(have.data()), static_cast<std::streamsize>(n));
          same = static_cast<size_t>(f.gcount()) == n && memcmp(have.data(), d, n) == 0;
          return same;
        }, err))
      return err = err.empty() ? rel + " was changed" : err, false;
    return true;
  }, err);
  if (!ok) return false;
  // Nothing added either.
  std::error_code ec;
  for (const char* sub : {"lib", "assets"})
    for (auto it = fs::recursive_directory_iterator(dir + "/" + sub, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_directory(ec)) continue;
      const std::string rel = fs::relative(it->path(), dir, ec).string();
      if (!expected.count(rel)) return err = rel + " was added", false;
    }
  if (!expected.count("lib/libroblox.so")) return err = "no libroblox.so in the APK", false;
  return true;
}

}  // namespace tonic
