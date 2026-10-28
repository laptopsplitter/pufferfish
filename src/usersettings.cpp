#include "platform.hpp"
#include "usersettings.hpp"

#include "config.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace tonic {

#define LOG(fmt, ...) fprintf(stderr, "[settings] " fmt, ##__VA_ARGS__)

// The engine's own frame rate target (DFIntTaskSchedulerTargetFps) limits
// it too: 240 unless raised. Set it to CAP in the overrides tonic hands
// Roblox at startup (data/settings/preload-overrides.json, written by
// clientsettings.cpp), as editing the flags file does for other clients.
void target_fps(int cap) {
  const std::string path = "data/settings/preload-overrides.json";
  std::ifstream in(path);
  if (!in) {
    LOG("%s missing: frame rate target not raised\n", path.c_str());
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string json = ss.str();
  in.close();
  static const std::regex key(R"("DFIntTaskSchedulerTargetFps"\s*:\s*"?\d*"?)");
  const std::string entry = "\"DFIntTaskSchedulerTargetFps\": \"" + std::to_string(cap) + "\"";
  if (std::regex_search(json, key)) {
    json = std::regex_replace(json, key, entry);
  } else {
    const size_t open = json.find('{');
    if (open == std::string::npos) return;
    const bool empty = json.find_first_not_of(" \t\r\n", open + 1) == json.find('}', open);
    json.insert(open + 1, entry + (empty ? "" : ", "));
  }
  std::ofstream(path, std::ios::trunc) << json;
  LOG("frame rate target %d\n", cap);
}

// tonic runs in its own systemd user scope (tonic-PID.scope), so its memory
// limit can be set and changed while it runs (systemctl set-property).
void memory_scope_enter(int argc, char** argv) {
  if (getenv("TONIC_SCOPE")) return;  // already in it
  // systemd-run needs a user session (systemd --user); try it once first, so
  // a failure leaves tonic running without a scope instead of not at all.
  if (system("systemd-run --user --scope --quiet --collect true >/dev/null 2>&1") != 0) {
    LOG("systemd-run --user not usable here: no RAM limit possible\n");
    return;
  }
  const std::string unit = "tonic-" + std::to_string(getpid());
  setenv("TONIC_SCOPE", (unit + ".scope").c_str(), 1);
  const std::string self = self_exe();
  if (self.empty()) return;
  std::vector<std::string> args = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--unit=" + unit, "--", self};
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  std::vector<char*> av;
  for (auto& a : args) av.push_back(a.data());
  av.push_back(nullptr);
  fflush(stderr);
  execvp("systemd-run", av.data());
  LOG("systemd-run failed (%s): no RAM limit possible\n", strerror(errno));
  unsetenv("TONIC_SCOPE");
}

void memory_limit_apply() {
  const char* unit = getenv("TONIC_SCOPE");
  if (!unit) return;
  const int mb = config_int("ram_limit_mb", 0);
  const std::string high = mb > 0 ? std::to_string(mb) + "M" : "infinity";
  const std::string cmd = std::string("systemctl --user set-property --runtime ") + unit + " MemoryHigh=" + high +
                          " >/dev/null 2>&1";
  static int applied = -1;
  if (mb == applied) return;
  if (system(cmd.c_str()) == 0) {
    const bool had = applied > 0;
    applied = mb;
    if (mb > 0) LOG("RAM limit %d MB (soft: slows down past it, does not crash)\n", mb);
    else if (had) LOG("RAM limit off\n");
  } else {
    LOG("could not set the RAM limit on %s\n", unit);
  }
}

void usersettings_apply() {
  // TONIC_FPS_CAP, else the menu's frame rate cap (0: leave Roblox's alone).
  const char* want = getenv("TONIC_FPS_CAP");
  const int cap = want && *want ? atoi(want) : config_int("fps_cap", 0);
  if (cap <= 0) return;
  const int capped = cap;
  target_fps(cap);
  namespace fs = std::filesystem;
  std::error_code ec;
  int found = 0;
  for (auto it = fs::recursive_directory_iterator("data", fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec || it->path().filename() != "GlobalBasicSettings_13.xml") continue;
    ++found;
    std::ifstream in(it->path());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string xml = ss.str();
    static const std::regex re(R"((<int name="FramerateCap">)(-?\d+)(</int>))");
    // An earlier tonic wrote "${1}N${3}" here instead of the element: repair it.
    static const std::regex broken(R"(\$\{1\}(-?\d+)\$\{3\})");
    std::smatch bm;
    if (std::regex_search(xml, bm, broken)) {
      xml.replace(bm.position(0), bm.length(0), "<int name=\"FramerateCap\">" + bm[1].str() + "</int>");
      LOG("repaired FramerateCap in %s\n", it->path().c_str());
    }
    std::smatch m;
    if (!std::regex_search(xml, m, re)) {
      LOG("%s has no FramerateCap yet; open Settings in a game once, close tonic, then try again\n",
          it->path().c_str());
      continue;
    }
    const std::string old = m[2];
    xml.replace(m.position(2), m.length(2), std::to_string(capped));
    std::ofstream(it->path(), std::ios::trunc) << xml;
    LOG("Maximum Frame Rate %s -> %d in %s\n", old.c_str(), capped, it->path().c_str());
  }
  if (!found) LOG("no GlobalBasicSettings_13.xml under data/ yet: play a game once, close tonic, then try again\n");
}

long long usersettings_mtime() {
  namespace fs = std::filesystem;
  std::error_code ec;
  long long newest = 0;
  for (auto it = fs::recursive_directory_iterator("data", fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const std::string n = it->path().filename().string();
    if (n != "GlobalBasicSettings_13.xml" && n != "GlobalSettings_13.xml") continue;
    const auto t = fs::last_write_time(it->path(), ec);
    if (!ec) newest = std::max<long long>(newest, t.time_since_epoch().count());
  }
  return newest;
}

}  // namespace tonic
