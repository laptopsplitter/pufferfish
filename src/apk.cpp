#include "apk.hpp"

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "apkfiles.hpp"
#include "apksig.hpp"
#include "config.hpp"
#include "spawn.hpp"

#include <atomic>
#include <chrono>

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[apk] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

constexpr const char* kVersions = "data/versions";

// Shell-quotes S for the commands below.
std::string q(const std::string& s) {
  std::string o = "'";
  for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return o + "'";
}

std::string run(const std::string& cmd, int* status = nullptr) {
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
  const int st = pclose(p);
  if (status) *status = st;
  return out;
}

bool has_tool(const char* name) { return system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0; }

std::vector<std::string> entries(const std::string& zip) {  // file names inside a zip
  std::vector<std::string> out;
  std::string line;
  std::stringstream ss(run("unzip -Z1 " + q(zip) + " 2>/dev/null"));
  while (std::getline(ss, line)) out.push_back(line);
  return out;
}

// --- AndroidManifest.xml (Android's binary XML): versionName of <manifest> ---

uint16_t u16(const std::string& b, size_t o) { return o + 2 <= b.size() ? static_cast<uint8_t>(b[o]) | static_cast<uint8_t>(b[o + 1]) << 8 : 0; }
uint32_t u32(const std::string& b, size_t o) { return o + 4 <= b.size() ? u16(b, o) | static_cast<uint32_t>(u16(b, o + 2)) << 16 : 0; }

std::string manifest_version(const std::string& x) {
  if (u16(x, 0) != 0x0003) return {};
  std::vector<std::string> strings;
  for (size_t p = u16(x, 2); p + 8 <= x.size();) {
    const uint16_t type = u16(x, p);
    const uint32_t size = u32(x, p + 4);
    if (size < 8) break;
    if (type == 0x0001) {  // string pool
      const uint32_t count = u32(x, p + 8), flags = u32(x, p + 16), start = u32(x, p + 20);
      const bool utf8 = flags & (1u << 8);
      for (uint32_t i = 0; i < count; ++i) {
        size_t s = p + start + u32(x, p + 28 + 4 * i);
        std::string str;
        if (utf8) {
          s += (static_cast<uint8_t>(x[s]) & 0x80) ? 2 : 1;  // length in UTF-16 units
          size_t len = static_cast<uint8_t>(x[s]);
          if (len & 0x80) len = ((len & 0x7f) << 8) | static_cast<uint8_t>(x[s + 1]), ++s;
          ++s;
          if (s + len <= x.size()) str = x.substr(s, len);
        } else {
          size_t len = u16(x, s);
          if (len & 0x8000) len = ((len & 0x7fff) << 16) | u16(x, s + 2), s += 2;
          s += 2;
          for (size_t k = 0; k < len && s + 2 * k + 1 < x.size(); ++k) {
            const uint16_t c = u16(x, s + 2 * k);
            str += c < 0x80 ? static_cast<char>(c) : '?';
          }
        }
        strings.push_back(str);
      }
    } else if (type == 0x0102) {  // start element
      const size_t ext = p + 16;
      const uint32_t name = u32(x, ext + 4);
      if (name < strings.size() && strings[name] == "manifest") {
        const uint16_t astart = u16(x, ext + 8), asize = u16(x, ext + 10), acount = u16(x, ext + 12);
        for (uint16_t a = 0; a < acount; ++a) {
          const size_t at = ext + astart + a * asize;
          const uint32_t an = u32(x, at + 4), raw = u32(x, at + 8), data = u32(x, at + 16);
          const uint8_t dtype = static_cast<uint8_t>(x[at + 15]);
          if (an >= strings.size() || strings[an] != "versionName") continue;
          if (raw < strings.size()) return strings[raw];
          if (dtype == 0x03 && data < strings.size()) return strings[data];
        }
        return {};
      }
    }
    p += size;
  }
  return {};
}

bool safe_version(const std::string& v) {
  return !v.empty() && v.size() < 40 &&
         std::all_of(v.begin(), v.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; });
}

std::string install(const std::string& path, const std::function<void(const std::string&)>& progress, std::string& err) {
  if (!has_tool("unzip")) {
    err = "unzip is not installed (pacman -S unzip)";
    return {};
  }
  std::error_code ec;
  const std::string tmp = "data/versions/.unpacking";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  // A bundle (.apks / .xapk) holds several APKs: the base one (manifest and
  // assets) and per-architecture ones (lib/x86_64).
  std::vector<std::string> apks;
  const auto top = entries(path);
  if (top.empty()) {
    err = "not a zip file: is it an APK?";
    return {};
  }
  const bool bundle = std::none_of(top.begin(), top.end(), [](const std::string& e) { return e == "AndroidManifest.xml"; });
  if (bundle) {
    progress("Opening the bundle");
    run("unzip -o -q " + q(path) + " '*.apk' -d " + q(tmp + "/split") + " 2>&1");
    for (auto& f : fs::recursive_directory_iterator(tmp + "/split", ec))
      if (f.path().extension() == ".apk") apks.push_back(f.path().string());
  } else {
    apks.push_back(path);
  }
  std::string base, arch;
  for (const auto& a : apks) {
    const auto list = entries(a);
    const bool manifest = std::find(list.begin(), list.end(), "AndroidManifest.xml") != list.end();
    const bool assets = std::any_of(list.begin(), list.end(), [](const std::string& e) { return e.rfind("assets/", 0) == 0; });
    const bool libs = std::any_of(list.begin(), list.end(), [](const std::string& e) { return e.rfind("lib/x86_64/", 0) == 0; });
    if (manifest && assets && base.empty()) base = a;
    if (libs && arch.empty()) arch = a;
  }
  if (arch.empty()) {
    err = "this APK has no x86_64 libraries (it is an ARM build): tonic needs the x86_64 one";
    fs::remove_all(tmp, ec);
    return {};
  }
  if (base.empty()) {
    err = "no base APK with assets found in this file";
    fs::remove_all(tmp, ec);
    return {};
  }
  // Only Roblox's own, unmodified APK: its signature checked as Android checks
  // it (apksig.hpp).
  progress("Checking that it is Roblox's own, unmodified");
  for (const std::string& a : {base, arch}) {
    std::string why;
    if (!apk_signed_by_roblox(a, why)) {
      err = why;
      LOG("refused %s: %s\n", path.c_str(), why.c_str());
      fs::remove_all(tmp, ec);
      return {};
    }
  }
  LOG("signature checked: Roblox's own APK, unmodified\n");
  run("unzip -o -q " + q(base) + " AndroidManifest.xml -d " + q(tmp) + " 2>&1");
  std::ifstream mf(tmp + "/AndroidManifest.xml", std::ios::binary);
  const std::string manifest((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
  std::string version = manifest_version(manifest);
  if (!safe_version(version)) version = "unknown-" + std::to_string(time(nullptr));
  const std::string dir = std::string(kVersions) + "/" + version;
  progress("Unpacking Roblox " + version);
  fs::remove_all(dir, ec);
  // The signed APKs are kept with it: each start checks the installed files
  // against them (apk_check_install).
  fs::create_directories(dir + "/apk", ec);
  const std::string kept_base = dir + "/apk/base.apk", kept_arch = arch == base ? kept_base : dir + "/apk/x86_64.apk";
  fs::copy_file(base, kept_base, fs::copy_options::overwrite_existing, ec);
  if (!ec && kept_arch != kept_base) fs::copy_file(arch, kept_arch, fs::copy_options::overwrite_existing, ec);
  fs::remove_all(tmp, ec);
  std::string why;
  if (ec || !apk_unpack(kept_base, kept_arch, dir, why) || !fs::exists(dir + "/lib/libroblox.so")) {
    err = !why.empty() ? why : ec ? "could not copy the APK: " + ec.message() : "no libroblox.so in this APK";
    fs::remove_all(dir, ec);
    return {};
  }
  // The app's folders, as Android's Java side makes them on first launch, and
  // Roblox's CA bundle where its HTTP stack looks for it.
  for (const char* d : {"data/files/exe", "data/cache", "data/android", "data/obb", "data/external"})
    fs::create_directories(d, ec);
  fs::copy_file(dir + "/assets/ssl/cacert.pem", "data/files/exe/cacert.pem", fs::copy_options::overwrite_existing, ec);
  for (auto& d : fs::directory_iterator(kVersions, ec))  // one at a time: the new one replaces the rest
    if (d.path().filename() != version && d.path().filename().string()[0] != '.') fs::remove_all(d.path(), ec);
  size_t libs = 0;
  for (auto& f : fs::directory_iterator(dir + "/lib", ec)) libs += f.path().extension() == ".so";
  LOG("installed Roblox %s: %zu libraries, assets in %s/assets\n", version.c_str(), libs, dir.c_str());
  return version;
}

// --- the newest Roblox from APKPure ------------------------------------------------
//
// Its app API, as Mocktail and apkeep (EFF) use it: one request with the
// processor type (x86_64) in a header; the answer lists the versions, newest
// first, each with its download link. Only links on APKPure's own hosts are
// followed. Whatever arrives is installed only if Roblox's signature checks out.

constexpr const char* kApkPureApi = "https://api.pureapk.com/m/v3/cms/app_version?hl=en-US&package_name=com.roblox.client";
constexpr const char* kDownload = "data/cache/roblox-download";
std::atomic<float> g_fraction{-1};

bool version_char(unsigned char c) { return isalnum(c) || c == '.' || c == '+' || c == '_' || c == '-'; }

// The answer's version markers: "2.700.123:" (a version name and a colon).
struct Marker { size_t begin, colon; std::string name; };
std::vector<Marker> markers(const std::string& b) {
  std::vector<Marker> out;
  for (size_t i = 0; i < b.size(); ++i) {
    if (!isdigit(static_cast<unsigned char>(b[i])) || (i && version_char(static_cast<unsigned char>(b[i - 1])))) continue;
    size_t e = i;
    bool dot = false;
    while (e < b.size() && version_char(static_cast<unsigned char>(b[e]))) dot = dot || b[e] == '.', ++e;
    if (dot && e < b.size() && b[e] == ':') out.push_back({i, e, b.substr(i, e - i)}), i = e;
  }
  return out;
}

bool apkpure_host(const std::string& url) {
  if (url.rfind("https://", 0) != 0) return false;
  const size_t end = url.find_first_of("/?#", 8);
  std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
  if (host.find_first_of("@:") != std::string::npos) return false;
  for (const char* d : {"pureapk.com", "apkpure.com", "winudf.com"}) {
    const std::string s = d;
    if (host == s || (host.size() > s.size() && host.compare(host.size() - s.size() - 1, s.size() + 1, "." + s) == 0)) return true;
  }
  return false;
}

// The newest version and its download link, from the answer.
bool parse_apkpure(const std::string& b, std::string& version, std::string& url, std::string& err) {
  const auto m = markers(b);
  if (m.empty()) return err = "APKPure's answer lists no version", false;
  const size_t end = m.size() > 1 ? m[1].begin : b.size();
  const std::string rec = b.substr(m[0].colon + 1, end - m[0].colon - 1);
  for (size_t i = 0; i < rec.size(); ++i) {
    size_t at = std::string::npos;
    if (rec.compare(i, 5, "XAPKJ") == 0) at = i + 7;
    else if (rec.compare(i, 4, "APKJ") == 0) at = i + 6;
    if (at == std::string::npos || rec.compare(at, 8, "https://") != 0) continue;
    size_t e = at;
    while (e < rec.size() && (isalnum(static_cast<unsigned char>(rec[e])) ||
                              std::string("-@:%._+~#=?&/()").find(rec[e]) != std::string::npos))
      ++e;
    const std::string u = rec.substr(at, e - at);
    if (apkpure_host(u)) {
      version = m[0].name, url = u;
      return true;
    }
    i = e;
  }
  return err = "APKPure has no download link for Roblox " + m[0].name, false;
}

std::vector<std::string> curl_https() {
  return {"curl", "-sS", "-L", "--fail", "--proto", "=https", "--proto-redir", "=https"};
}

std::string fetch(const std::function<void(const std::string&)>& progress, std::string& err) {
  if (!has_tool("curl")) return err = "curl is not installed", "";
  progress("Asking APKPure for the newest Roblox (x86_64)");
  auto meta = curl_https();
  meta.insert(meta.end(), {"-m", "60", "--max-filesize", "4000000", "-H", "x-cv: 3172501", "-H", "x-sv: 29", "-H", "x-gp: 1",
                           "-H", "x-abis: x86_64", kApkPureApi});
  int st = -1;
  const std::string answer = spawn_read(meta, 70, &st);
  if (st != 0 || answer.empty()) return err = "could not reach APKPure (check your internet connection)", "";
  std::string version, url;
  if (!parse_apkpure(answer, version, url, err)) return {};
  LOG("APKPure: newest Roblox is %s\n", version.c_str());

  std::error_code ec;
  fs::create_directories("data/cache", ec);
  const std::string out = std::string(kDownload) + (url.find(".apk") != std::string::npos && url.find(".xapk") == std::string::npos ? ".apk" : ".xapk");
  fs::remove(out, ec);
  // Its size first, for the progress bar (if the server says).
  auto head = curl_https();
  head.insert(head.end(), {"-I", "-m", "30", url});
  long long total = 0;
  {
    std::stringstream hs(spawn_read(head, 35));
    std::string line;
    while (std::getline(hs, line))
      if (line.size() > 15 && strncasecmp(line.c_str(), "content-length:", 15) == 0) total = atoll(line.c_str() + 15);
  }
  g_fraction = total > 0 ? 0.f : -1.f;
  std::atomic<bool> done{false};
  std::thread watch([&] {  // the progress, from the file's size as it grows
    std::error_code ec;
    while (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      const long long have = static_cast<long long>(fs::exists(out, ec) ? fs::file_size(out, ec) : 0);
      char buf[160];
      if (total > 0) {
        g_fraction = std::min(1.f, static_cast<float>(have) / total);
        snprintf(buf, sizeof buf, "Downloading Roblox %s: %lld of %lld MB", version.c_str(), have >> 20, total >> 20);
      } else {
        snprintf(buf, sizeof buf, "Downloading Roblox %s: %lld MB", version.c_str(), have >> 20);
      }
      progress(buf);
    }
  });
  auto get = curl_https();
  get.insert(get.end(), {"-m", "3600", "--max-filesize", "2000000000", "-o", out, url});
  spawn_read(get, 3610, &st);
  done = true;
  watch.join();
  g_fraction = -1;
  if (st != 0 || !fs::exists(out, ec) || fs::file_size(out, ec) < 1000000) {
    fs::remove(out, ec);
    return err = "the download from APKPure failed (check your internet connection)", "";
  }
  return out;
}

}  // namespace

bool apk_check_install(const RobloxInstall& ri, std::string& why, bool* old) {
  if (old) *old = false;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string dir = std::string(kVersions) + "/" + ri.version;
  const std::string base = dir + "/apk/base.apk";
  const std::string arch = fs::exists(dir + "/apk/x86_64.apk") ? dir + "/apk/x86_64.apk" : base;
  if (!fs::exists(base)) {
    if (old) *old = true;
    return why = "installed by an older tonic, without its APK to check against", false;
  }
  for (const std::string& a : {base, arch}) {
    std::string w;
    if (!apk_signed_by_roblox(a, w)) return why = "the kept APK is " + w, false;
  }
  if (!apk_files_match(base, arch, dir, why)) return false;
  // Roblox's trust store for its connections, outside that folder: put back
  // from the checked assets every time.
  std::error_code ec;
  fs::create_directories("data/files/exe", ec);
  fs::copy_file(dir + "/assets/ssl/cacert.pem", "data/files/exe/cacert.pem", fs::copy_options::overwrite_existing, ec);
  LOG("Roblox %s checked: signed by Roblox, every file as signed (%.1f s)\n", ri.version.c_str(),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

float apk_progress_fraction() { return g_fraction; }

void apk_fetch_latest(std::function<void(const std::string&)> progress,
                      std::function<void(const std::string&, const std::string&)> done) {
  std::thread([progress, done] {
    std::string err;
    const std::string path = fetch(progress, err);
    std::string v;
    if (!path.empty()) {
      v = install(path, progress, err);
      std::error_code ec;
      fs::remove(path, ec);  // (installed or refused: the download is not kept)
    }
    if (v.empty()) LOG("no Roblox from APKPure: %s\n", err.c_str());
    done(v, err);
  }).detach();
}

std::vector<std::string> apk_versions() {
  std::vector<std::pair<long long, std::string>> found;
  std::error_code ec;
  for (auto& d : fs::directory_iterator(kVersions, ec)) {
    const std::string name = d.path().filename().string();
    if (name[0] == '.' || !fs::exists(d.path() / "lib" / "libroblox.so")) continue;
    found.push_back({fs::last_write_time(d.path(), ec).time_since_epoch().count(), name});
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  for (auto& f : found) out.push_back(f.second);
  return out;
}

bool apk_current(RobloxInstall& out) {
  const auto versions = apk_versions();
  std::string pick = config_get("apk_version", "");
  if (std::find(versions.begin(), versions.end(), pick) == versions.end()) pick = versions.empty() ? "" : versions.back();
  if (pick.empty()) return false;
  const std::string dir = std::string(kVersions) + "/" + pick;
  out = {pick, dir + "/lib/libroblox.so", dir + "/assets"};
  return true;
}

void apk_forget() {
  RobloxInstall cur;
  if (!apk_current(cur)) return;
  std::error_code ec;
  fs::remove_all(std::string(kVersions) + "/" + cur.version, ec);
  config_set("apk_version", "");
  LOG("Roblox %s removed\n", cur.version.c_str());
}

bool can_choose_file() { return has_tool("zenity") || has_tool("kdialog"); }

void choose_file(const std::string& title, const std::string& label, const std::string& patterns,
                 std::function<void(const std::string& path)> done) {
  std::thread([title, label, patterns, done] {
    std::string out;
    if (has_tool("zenity"))
      out = run("zenity --file-selection --title=" + q(title) + " --file-filter=" + q(label + " | " + patterns) + " 2>/dev/null");
    else if (has_tool("kdialog"))
      out = run("kdialog --title " + q(title) + " --getopenfilename ~ " + q(patterns) + " 2>/dev/null");
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    done(out);
  }).detach();
}

}  // namespace tonic
