#include "profile.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "platform.hpp"
#include "script.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[instances] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

constexpr const char* kProfiles = "data/profiles";
int g_lock_fd = -1;

std::string dir_of(const std::string& id) { return id.empty() ? std::string("data") : std::string(kProfiles) + "/" + id; }
std::string label_of(const std::string& id) { return id.empty() ? std::string("Main") : "Instance " + id; }

bool valid_id(const std::string& id) {
  return !id.empty() && id.size() < 16 && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// True when another process holds the instance's lock.
bool locked(const std::string& id) {
  const std::string p = dir_of(id) + "/instance.lock";
  const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
  close(fd);  // (releases it again when we got it)
  return busy;
}

// The account Roblox saved in its app storage, if it did.
std::string username_in(const std::string& files) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(files, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 3) it.disable_recursion_pending();
    if (it->path().filename() != "appStorage.json") continue;
    std::ifstream f(it->path());
    std::stringstream ss;
    ss << f.rdbuf();
    static const std::regex user(R"re("Username"\s*:\s*"([^"]{1,40})")re");
    std::smatch m;
    const std::string s = ss.str();
    if (std::regex_search(s, m, user)) return m[1];
  }
  return {};
}

std::vector<std::string> cmdline() { return self_cmdline(); }

}  // namespace

std::string profile_id() {
  const char* p = getenv("TONIC_PROFILE");
  return p && valid_id(p) ? p : "";
}

std::string profile_dir() { return dir_of(profile_id()); }

std::string profile_title() {
  const std::string id = profile_id();
  return id.empty() ? "tonic" : "tonic - " + label_of(id);
}

bool profile_setup(const std::string& here) {
  const std::string id = profile_id(), dir = dir_of(id);
  std::error_code ec;
  for (const char* sub : {"files", "cache", "obb", "external"}) fs::create_directories(dir + "/" + sub, ec);
  // Logins live in here (the cookie file, Roblox's app storage, the sign-in
  // window's web data): only you can open the folders. Fixes older installs too.
  for (const std::string& d : {std::string("data"), dir, dir + "/files"}) {
    struct stat ds{};
    if (stat(d.c_str(), &ds) == 0 && S_ISDIR(ds.st_mode) && (ds.st_mode & 077)) {
      chmod(d.c_str(), 0700);
      if (d == "data") LOG("made tonic's data folder private (was readable by other users)\n");
    }
  }
  if (!id.empty()) {
    // Roblox's HTTP stack finds its CA bundle in files/exe: Main's.
    const fs::path exe = dir + "/files/exe";
    if (!fs::exists(exe, ec)) fs::create_directory_symlink(here + "/data/files/exe", exe, ec);
    set_script_var("DATA", here + "/" + dir);
    setenv("TONIC_COOKIES", (dir + "/files/tonic-cookies.txt").c_str(), 1);
    setenv("TONIC_WEBVIEW_DATA", (here + "/" + dir + "/webview").c_str(), 1);  // its own web data too
  }
  g_lock_fd = open((dir + "/instance.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (g_lock_fd >= 0 && flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    fprintf(stderr, "%s is already running (close it first, or start another instance from the mod panel)\n",
            label_of(id).c_str());
    return false;
  }
  if (!id.empty()) {
    struct stat st{};
    const bool saved = stat((dir + "/files/tonic-cookies.txt").c_str(), &st) == 0 && st.st_size > 0;
    LOG("this is %s: its own Roblox files, cache, web data and login in %s (%s)\n", label_of(id).c_str(), dir.c_str(),
        saved ? "saved login found" : "no saved login: you sign in here");
  }
  return true;
}

std::vector<Instance> profile_list() {
  std::vector<std::string> ids{""};
  std::error_code ec;
  std::vector<std::string> more;
  for (auto& e : fs::directory_iterator(kProfiles, ec))
    if (e.is_directory() && valid_id(e.path().filename().string())) more.push_back(e.path().filename().string());
  std::sort(more.begin(), more.end(), [](const std::string& a, const std::string& b) { return std::stoi(a) < std::stoi(b); });
  ids.insert(ids.end(), more.begin(), more.end());
  const std::string me = profile_id();
  std::vector<Instance> out;
  for (auto& id : ids) {
    Instance in;
    in.id = id;
    in.label = label_of(id);
    const std::string dir = dir_of(id);
    in.self = id == me;
    in.running = in.self || locked(id);
    struct stat st{};
    in.signed_in = stat((dir + "/files/tonic-cookies.txt").c_str(), &st) == 0 && st.st_size > 0;
    in.username = username_in(dir + "/files");
    out.push_back(in);
  }
  return out;
}

std::string profile_new() {
  int n = 2;
  std::error_code ec;
  while (fs::exists(dir_of(std::to_string(n)), ec)) ++n;
  const std::string id = std::to_string(n);
  fs::create_directories(dir_of(id) + "/files", ec);
  LOG("new instance: %s\n", label_of(id).c_str());
  return id;
}

bool profile_launch(const std::string& id) {
  if (!id.empty() && !valid_id(id)) return false;
  if (id == profile_id() || locked(id)) return false;
  std::vector<std::string> args = cmdline();
  if (args.empty()) return false;
  const std::string log = dir_of(id) + "/tonic.log";
  std::error_code ec;
  fs::create_directories(dir_of(id), ec);
  // Everything the child needs, made before fork (only exec-safe calls after).
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) {
    const std::string v = *e;
    bool drop = false;
    for (const char* k : {"TONIC_PROFILE=", "TONIC_SCOPE=", "TONIC_AUTOPLAY=", "TONIC_COOKIES=", "TONIC_WEBVIEW_DATA="})
      if (v.rfind(k, 0) == 0) drop = true;
    if (!drop) env.push_back(v);
  }
  if (!id.empty()) env.push_back("TONIC_PROFILE=" + id);
  const std::string exe = self_exec_path();  // (made before fork)
  std::vector<char*> argv, envp;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);
  for (auto& s : env) envp.push_back(s.data());
  envp.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();  // its own: closing this tonic does not close it
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    for (int i = 3; i < 1024; ++i) close(i);
    execve(exe.c_str(), argv.data(), envp.data());
    _exit(127);
  }
  LOG("started %s (pid %d, log %s)\n", label_of(id).c_str(), pid, log.c_str());
  return true;
}

bool profile_delete(const std::string& id) {
  if (!valid_id(id) || id == profile_id() || locked(id)) return false;
  std::error_code ec;
  fs::remove_all(dir_of(id), ec);
  LOG("deleted %s%s\n", label_of(id).c_str(), ec ? " (some files could not be removed)" : "");
  return !ec;
}

}  // namespace tonic
