// assets.cpp - the AAssetManager / AAsset API (Android's read-only view of the
// APK's assets/ folder), served from a directory on disk: the APK's assets/
// unpacked (default data/apk/assets, or --assets DIR).
//
// The library asks by path relative to assets/ ("shaders/x.pack"), and
// sometimes by an absolute path built from the asset folder we gave it; both
// are accepted.

#include "assets.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "shims.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[assets] " fmt, ##__VA_ARGS__)

std::string g_root = "data/apk/assets";
struct Manager {
  uint32_t magic = 0x5f6d6772;  // "_mgr"
} g_manager;

struct Asset {
  int fd = -1;
  off_t length = 0;
  off_t pos = 0;
  void* map = nullptr;
  std::string path;
};

std::atomic<long> g_opened{0}, g_missing{0};
std::mutex g_mu;
std::set<std::string> g_missing_names;

std::string resolve(const char* name) {
  if (!name) return {};
  if (name[0] == '/') return name;
  std::string n = name;
  while (n.rfind("./", 0) == 0) n.erase(0, 2);
  return g_root + "/" + n;
}

void* sh_AAssetManager_fromJava(void*, void*) { return &g_manager; }

void* sh_AAssetManager_open(void* mgr, const char* name, int mode) {
  (void)mgr;
  (void)mode;
  const std::string path = resolve(name);
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat st {};
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (fd >= 0) close(fd);
    ++g_missing;
    std::lock_guard<std::mutex> l(g_mu);
    if (g_missing_names.insert(name ? name : "").second) LOG("open %s: not found (%s)\n", name ? name : "(null)", path.c_str());
    return nullptr;
  }
  ++g_opened;
  auto* a = new Asset;
  a->fd = fd;
  a->length = st.st_size;
  a->path = path;
  return a;
}

void sh_AAsset_close(void* p) {
  auto* a = static_cast<Asset*>(p);
  if (!a) return;
  if (a->map) munmap(a->map, a->length ? a->length : 1);
  if (a->fd >= 0) close(a->fd);
  delete a;
}

const void* sh_AAsset_getBuffer(void* p) {
  auto* a = static_cast<Asset*>(p);
  if (!a) return nullptr;
  if (!a->map) {
    void* m = mmap(nullptr, a->length ? a->length : 1, PROT_READ, MAP_PRIVATE, a->fd, 0);
    if (m == MAP_FAILED) return nullptr;
    a->map = m;
  }
  return a->map;
}

off_t sh_AAsset_getLength(void* p) { return p ? static_cast<Asset*>(p)->length : 0; }
off_t sh_AAsset_getRemainingLength(void* p) {
  auto* a = static_cast<Asset*>(p);
  return a ? a->length - a->pos : 0;
}

int sh_AAsset_read(void* p, void* buf, size_t n) {
  auto* a = static_cast<Asset*>(p);
  if (!a) return -1;
  const ssize_t r = pread(a->fd, buf, n, a->pos);
  if (r > 0) a->pos += r;
  return static_cast<int>(r);
}

off_t sh_AAsset_seek(void* p, off_t off, int whence) {
  auto* a = static_cast<Asset*>(p);
  if (!a) return -1;
  off_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? a->pos + off : a->length + off;
  if (np < 0 || np > a->length) return -1;
  a->pos = np;
  return np;
}

// The whole file is the asset (nothing is compressed inside an APK here).
int sh_AAsset_openFileDescriptor(void* p, off_t* start, off_t* length) {
  auto* a = static_cast<Asset*>(p);
  if (!a) return -1;
  if (start) *start = 0;
  if (length) *length = a->length;
  return dup(a->fd);
}

int sh_AAsset_isAllocated(void*) { return 0; }

}  // namespace

void set_asset_root(const std::string& dir) {
  g_root = dir;
  while (g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
}

const std::string& asset_root() { return g_root; }

void register_asset_shims() {
  register_shim("AAssetManager_fromJava", reinterpret_cast<void*>(&sh_AAssetManager_fromJava));
  register_shim("AAssetManager_open", reinterpret_cast<void*>(&sh_AAssetManager_open));
  register_shim("AAsset_close", reinterpret_cast<void*>(&sh_AAsset_close));
  register_shim("AAsset_getBuffer", reinterpret_cast<void*>(&sh_AAsset_getBuffer));
  register_shim("AAsset_getLength", reinterpret_cast<void*>(&sh_AAsset_getLength));
  register_shim("AAsset_getLength64", reinterpret_cast<void*>(&sh_AAsset_getLength));
  register_shim("AAsset_getRemainingLength", reinterpret_cast<void*>(&sh_AAsset_getRemainingLength));
  register_shim("AAsset_getRemainingLength64", reinterpret_cast<void*>(&sh_AAsset_getRemainingLength));
  register_shim("AAsset_read", reinterpret_cast<void*>(&sh_AAsset_read));
  register_shim("AAsset_seek", reinterpret_cast<void*>(&sh_AAsset_seek));
  register_shim("AAsset_seek64", reinterpret_cast<void*>(&sh_AAsset_seek));
  register_shim("AAsset_openFileDescriptor", reinterpret_cast<void*>(&sh_AAsset_openFileDescriptor));
  register_shim("AAsset_openFileDescriptor64", reinterpret_cast<void*>(&sh_AAsset_openFileDescriptor));
  register_shim("AAsset_isAllocated", reinterpret_cast<void*>(&sh_AAsset_isAllocated));
}

void print_asset_summary() {
  printf("  assets (%s): %ld opened, %ld not found\n", g_root.c_str(), g_opened.load(), g_missing.load());
}

}  // namespace tonic
