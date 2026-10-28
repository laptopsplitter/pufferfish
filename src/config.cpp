#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace tonic {

namespace {

// TONIC_CONFIG: another settings file (the tests use their own).
const std::string& path_() {
  static const std::string p = getenv("TONIC_CONFIG") && *getenv("TONIC_CONFIG") ? getenv("TONIC_CONFIG") : "data/files/tonic.conf";
  return p;
}
#define kPath path_().c_str()
std::mutex g_mu;
std::map<std::string, std::string> g_values;
bool g_loaded = false;

void load() {  // g_mu held
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream f(kPath);
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    g_values[line.substr(0, eq)] = line.substr(eq + 1);
  }
}

}  // namespace

std::string config_get(const std::string& name, const std::string& fallback) {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  auto it = g_values.find(name);
  return it == g_values.end() ? fallback : it->second;
}

bool config_bool(const std::string& name, bool fallback) {
  const std::string v = config_get(name);
  return v.empty() ? fallback : v == "1";
}

int config_int(const std::string& name, int fallback) {
  const std::string v = config_get(name);
  return v.empty() ? fallback : atoi(v.c_str());
}

void config_set(const std::string& name, const std::string& value) {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  g_values[name] = value;
  std::error_code ec;
  std::filesystem::create_directories("data/files", ec);
  std::ofstream f(std::string(kPath) + ".new", std::ios::trunc);
  f << "# tonic settings (the menu on the loading screen)\n";
  for (auto& [k, v] : g_values) f << k << "=" << v << "\n";
  f.close();
  rename((std::string(kPath) + ".new").c_str(), kPath);
}

}  // namespace tonic
