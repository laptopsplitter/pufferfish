#include "cookies.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include "fakejni.hpp"
#include "script.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[cookies] " fmt, ##__VA_ARGS__)

constexpr const char* kUrl = "https://www.roblox.com";

struct Cookie {
  std::string value;
  long long expires = 0;  // unix time; 0 = session cookie (kept anyway, as Android does across restarts)
};
std::mutex g_mu;
std::map<std::string, Cookie> g_jar;

std::string path() {
  if (const char* p = getenv("TONIC_COOKIES")) return p;
  // Saved by an older build under its earlier name: keep the login.
  struct stat st{};
  if (stat("data/files/tonic-cookies.txt", &st) != 0 && stat("data/files/tumbler-cookies.txt", &st) == 0)
    rename("data/files/tumbler-cookies.txt", "data/files/tonic-cookies.txt");
  return "data/files/tonic-cookies.txt";
}

std::string trim(std::string s) {
  const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void load() {
  std::ifstream f(path());
  std::string line;
  while (std::getline(f, line)) {  // NAME \t EXPIRES \t VALUE
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos) continue;
    Cookie c;
    c.expires = strtoll(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    c.value = line.substr(t2 + 1);
    if (c.expires && c.expires < time(nullptr)) continue;
    g_jar[line.substr(0, t1)] = c;
  }
}

void save() {  // g_mu held
  const std::string p = path(), tmp = p + ".new";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    LOG("cannot write %s: %s\n", tmp.c_str(), strerror(errno));
    return;
  }
  std::string out;
  for (auto& [name, c] : g_jar) out += name + "\t" + std::to_string(c.expires) + "\t" + c.value + "\n";
  if (write(fd, out.data(), out.size()) < 0) LOG("write failed\n");
  fchmod(fd, 0600);
  close(fd);
  rename(tmp.c_str(), p.c_str());
}

bool g_logged_out = false;  // log out: nothing is saved again

// One Set-Cookie header: NAME=VALUE; Expires=...; Max-Age=...; ...
void apply(const std::string& header) {
  const size_t semi = header.find(';');
  const std::string nv = header.substr(0, semi);
  const size_t eq = nv.find('=');
  if (eq == std::string::npos) return;
  const std::string name = trim(nv.substr(0, eq)), value = trim(nv.substr(eq + 1));
  if (name.empty()) return;
  long long expires = 0;
  bool gone = value.empty();
  std::stringstream attrs(semi == std::string::npos ? "" : header.substr(semi + 1));
  std::string a;
  while (std::getline(attrs, a, ';')) {
    a = trim(a);
    const size_t e = a.find('=');
    std::string k = a.substr(0, e), v = e == std::string::npos ? "" : trim(a.substr(e + 1));
    for (char& ch : k) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    if (k == "max-age") {
      const long long s = strtoll(v.c_str(), nullptr, 10);
      if (s <= 0) gone = true;
      else expires = time(nullptr) + s;
    } else if (k == "expires" && !expires) {
      tm t{};
      if (strptime(v.c_str(), "%a, %d %b %Y %H:%M:%S", &t) || strptime(v.c_str(), "%a, %d-%b-%Y %H:%M:%S", &t)) {
        expires = timegm(&t);
        if (expires < time(nullptr)) gone = true;
      }
    }
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (g_logged_out) return;
  if (gone) {
    if (g_jar.erase(name)) LOG("removed %s\n", name.c_str());
  } else {
    const bool had = g_jar.count(name);
    g_jar[name] = Cookie{value, expires};
    if (!had) LOG("saved %s\n", name.c_str());
  }
  save();
}

}  // namespace

void cookies_init() {
  add_jni_call_listener([](const std::string& label, const std::string& method, const std::vector<uint64_t>& raw) {
    if (label != "cookieHandler" || method != "onSetCookie" || raw.empty()) return;
    for (const std::string& h : jni_string_array(reinterpret_cast<void*>(raw[0]))) apply(h);
  });
}

void cookies_save_login(const std::string& value, long long expires) {
  std::lock_guard<std::mutex> l(g_mu);
  g_logged_out = false;
  g_jar.clear();  // a new sign-in: nothing of the old session stays
  g_jar[".ROBLOSECURITY"] = Cookie{value, expires};
  save();
  LOG("saved the login from roblox.com\n");  // (never the value)
}

bool cookies_have_login() {
  std::ifstream f(path());
  std::string line;
  while (std::getline(f, line)) {  // NAME \t EXPIRES \t VALUE (the value is not kept here)
    if (line.rfind(".ROBLOSECURITY\t", 0) != 0) continue;
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos || t2 + 1 >= line.size()) continue;
    const long long expires = strtoll(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    if (!expires || expires > time(nullptr)) return true;
  }
  return false;
}

std::string cookies_session() {
  std::ifstream f(path());
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind(".ROBLOSECURITY\t", 0) != 0) continue;
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    const long long expires = strtoll(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    if (!expires || expires > time(nullptr)) return line.substr(t2 + 1);
  }
  return {};
}

void cookies_logout() {
  std::lock_guard<std::mutex> l(g_mu);
  g_logged_out = true;
  g_jar.clear();
  if (unlink(path().c_str()) == 0) LOG("saved login deleted\n");
}

void cookies_storage_init() {
  // StatFs(Environment.getDataDirectory()).getAvailableBytes(): free space
  // where the library keeps its files.
  register_java_method("com/roblox/client/LocalStorageManager.getAllocatableBytes()J",
                       [](void*, const std::vector<uint64_t>&) -> uint64_t {
                         struct statvfs st{};
                         if (statvfs("data/files", &st) != 0 && statvfs(".", &st) != 0) return 0;
                         return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
                       });
}

void cookies_enable(const ElfImage& img) {
  std::string header;
  {
    std::lock_guard<std::mutex> l(g_mu);
    load();
    for (auto& [name, c] : g_jar) header += (header.empty() ? "" : "; ") + name + "=" + c.value;
    LOG("%zu saved cookie(s) from %s%s\n", g_jar.size(), path().c_str(),
        g_jar.count(".ROBLOSECURITY") ? " (includes the session: you stay logged in)" : "");
  }
  void* env = jni_env();
  if (auto set = reinterpret_cast<void (*)(void*, void*, void*, void*)>(
          find_native(img, "com/roblox/engine/jni/NativeSettingsInterface", "nativeSetMultipleCookies",
                      "(Ljava/lang/String;Ljava/lang/String;)V")))
    set(env, jni_class("com/roblox/engine/jni/NativeSettingsInterface"), jni_new_string(kUrl), jni_new_string(header));
  else
    LOG("nativeSetMultipleCookies not found\n");
  constexpr const char* kProto = "com/roblox/universalapp/cookie/JNICookieProtocol";
  if (auto reg = reinterpret_cast<void (*)(void*, void*, void*)>(
          find_native(img, kProto, "updateOnSetCookieHandler",
                      "(Lcom/roblox/universalapp/cookie/JNICookieProtocol$OnSetCookieHandler;)V")))
    reg(env, jni_new_object(kProto),
        jni_new_object("com/roblox/universalapp/cookie/CookieProtocol$OnSetCookieHandlerImpl", "cookieHandler"));
  else
    LOG("updateOnSetCookieHandler not found: new cookies will not be saved\n");
}

}  // namespace tonic
