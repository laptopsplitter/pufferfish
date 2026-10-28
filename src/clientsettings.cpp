// clientsettings.cpp - see clientsettings.hpp.
#include "clientsettings.hpp"
#include "platform.hpp"
#include "spawn.hpp"

#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace tonic {
namespace {

#define LOG(fmt, ...) fprintf(stderr, "[settings] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

constexpr const char* kUrl = "https://clientsettingscdn.roblox.com/v2/settings/application/AndroidApp";
constexpr const char* kDir = "data/settings";

std::mutex g_mu;
std::condition_variable g_cv;
bool g_started = false, g_done = false;

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A JSON string at S[I] (I at the opening quote), as written (escapes kept); I moves past it.
bool json_string(const std::string& s, size_t& i, std::string& raw) {
  if (i >= s.size() || s[i] != '"') return false;
  const size_t start = i++;
  while (i < s.size() && s[i] != '"') i += s[i] == '\\' ? 2 : 1;
  if (i >= s.size()) return false;
  raw = s.substr(start, ++i - start);
  return true;
}

// {"applicationSettings": {"Name": "value", ...}}: the pairs, as written.
bool settings_of(const std::string& json, std::map<std::string, std::string>& out) {
  size_t i = json.find("\"applicationSettings\"");
  if (i == std::string::npos) return false;
  i = json.find('{', i);
  if (i == std::string::npos) return false;
  ++i;
  for (;;) {
    while (i < json.size() && (isspace(static_cast<unsigned char>(json[i])) || json[i] == ',')) ++i;
    if (i >= json.size()) return false;
    if (json[i] == '}') return true;
    std::string k, v;
    if (!json_string(json, i, k)) return false;
    while (i < json.size() && (isspace(static_cast<unsigned char>(json[i])) || json[i] == ':')) ++i;
    if (i < json.size() && json[i] == '"') {
      if (!json_string(json, i, v)) return false;
    } else {  // (a number or true/false: kept as written)
      const size_t s = i;
      while (i < json.size() && json[i] != ',' && json[i] != '}' && !isspace(static_cast<unsigned char>(json[i]))) ++i;
      v = json.substr(s, i - s);
    }
    out[k] = v;
  }
}

std::string json_of(const std::map<std::string, std::string>& m, bool wrapped) {
  std::string s = wrapped ? "{\"applicationSettings\": {" : "{";
  bool first = true;
  for (const auto& [k, v] : m) {
    s += (first ? "" : ", ") + k + ": " + v;
    first = false;
  }
  return s + (wrapped ? "}}" : "}");
}

void write_file(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".new";
  { std::ofstream(tmp, std::ios::trunc | std::ios::binary) << text; }
  std::error_code ec;
  fs::rename(tmp, path, ec);
}

void refresh() {
  std::error_code ec;
  fs::create_directories(kDir, ec);
  std::map<std::string, std::string> overrides;
  if (!settings_of(slurp(share_file("scripts/settings-overrides.json")), overrides))
    LOG("scripts/settings-overrides.json not found or not readable\n");
  const std::string server_path = std::string(kDir) + "/AndroidApp.json";
  int st = -1;
  const std::string got = spawn_read({"curl", "-sS", "--fail", "--proto", "=https", "--proto-redir", "=https", "-m", "15", kUrl}, 20, &st);
  std::map<std::string, std::string> server;
  if (st == 0 && settings_of(got, server)) {
    write_file(server_path, got);
  } else if (settings_of(slurp(server_path), server)) {
    LOG("could not download Roblox's settings: using the last ones\n");
  } else {
    LOG("could not download Roblox's settings: using tonic's overrides alone\n");
  }
  std::map<std::string, std::string> merged = server;
  for (const auto& [k, v] : overrides) merged[k] = v;
  write_file(std::string(kDir) + "/client-settings.json", json_of(merged, true));
  // The overrides alone, flat, for nativePreloadFlagOverrides: they stay in
  // force when Roblox fetches its flags again while running.
  write_file(std::string(kDir) + "/preload-overrides.json", json_of(overrides, false));
  LOG("%zu of Roblox's settings, %zu of tonic's overrides\n", server.size(), overrides.size());
}

}  // namespace

void clientsettings_start() {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_started) return;
    g_started = true;
  }
  std::thread([] {
    refresh();
    std::lock_guard<std::mutex> l(g_mu);
    g_done = true;
    g_cv.notify_all();
  }).detach();
}

void clientsettings_wait() {
  clientsettings_start();
  std::unique_lock<std::mutex> l(g_mu);
  g_cv.wait(l, [] { return g_done; });
}

}  // namespace tonic
