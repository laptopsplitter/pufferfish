#include "recent.hpp"
#include "spawn.hpp"
#include "profile.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace tonic {

namespace {

// Per instance (profile.hpp): each account has its own recent games.
std::string path_of() { return profile_dir() + "/files/tonic-recent.txt"; }
constexpr size_t kMax = 8;
std::mutex g_mu;
std::vector<RecentGame> g_list;
std::string g_owner;  // the account the list belongs to ("" not known yet)
bool g_loaded = false;
std::atomic<bool> g_fetching{false};

void load() {  // g_mu held
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream f(path_of());
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("#owner\t", 0) == 0) {
      g_owner = line.substr(7);
      continue;
    }
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
    if (t1 == std::string::npos) continue;
    RecentGame g;
    g.place = strtoll(line.c_str(), nullptr, 10);
    g.when = strtoll(line.c_str() + t1 + 1, nullptr, 10);
    if (t2 != std::string::npos) g.name = line.substr(t2 + 1);
    if (g.place > 0) g_list.push_back(g);
  }
}

void save() {  // g_mu held
  std::error_code ec;
  std::filesystem::create_directories(profile_dir() + "/files", ec);
  std::ofstream f(path_of() + ".new", std::ios::trunc);
  if (!g_owner.empty()) f << "#owner\t" << g_owner << "\n";
  for (auto& g : g_list) f << g.place << "\t" << g.when << "\t" << g.name << "\n";
  f.close();
  rename((path_of() + ".new").c_str(), path_of().c_str());
}

// GET URL with curl; empty on failure. Public endpoints, no cookies sent;
// HTTPS only, also for redirects, and no shell in between (spawn.hpp).
std::string get(const std::string& url) {
  int st = 0;
  std::string out = spawn_read({"curl", "-fsS", "-m", "8", "--proto", "=https", "--proto-redir", "=https", "--max-filesize", "1000000", url}, 10, &st);
  return st == 0 ? out : std::string();
}

std::string json_string(const std::string& j, const std::string& key) {
  const size_t k = j.find("\"" + key + "\"");
  if (k == std::string::npos) return {};
  size_t q = j.find('"', j.find(':', k) + 1);
  if (q == std::string::npos) return {};
  std::string out;
  for (++q; q < j.size() && j[q] != '"'; ++q) {
    if (j[q] == '\\' && q + 1 < j.size()) ++q;
    if (j[q] != '\t' && j[q] != '\n' && j[q] != '\r') out += j[q];
  }
  return out;
}

std::string name_of(long long place) {
  const std::string u = get("https://apis.roblox.com/universes/v1/places/" + std::to_string(place) + "/universe");
  const size_t k = u.find("\"universeId\"");
  if (k == std::string::npos) return {};
  const long long universe = strtoll(u.c_str() + u.find(':', k) + 1, nullptr, 10);
  if (universe <= 0) return {};
  return json_string(get("https://games.roblox.com/v1/games?universeIds=" + std::to_string(universe)), "name");
}

}  // namespace

void recent_add(long long place) {
  if (place <= 0) return;
  {
    std::lock_guard<std::mutex> l(g_mu);
    load();
    std::string name;
    auto it = std::find_if(g_list.begin(), g_list.end(), [&](const RecentGame& g) { return g.place == place; });
    if (it != g_list.end()) name = it->name, g_list.erase(it);
    g_list.insert(g_list.begin(), RecentGame{place, static_cast<long long>(time(nullptr)), name});
    if (g_list.size() > kMax) g_list.resize(kMax);
    save();
  }
  recent_fetch_names();
}

void recent_set_owner(const std::string& account) {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  if (account == g_owner) return;
  // Another account (or none, after logging out): its own recent games.
  if (!g_owner.empty() || account.empty()) g_list.clear();
  g_owner = account;
  save();
}

std::vector<RecentGame> recent_list() {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  return g_list;
}

void recent_fetch_names() {
  if (g_fetching.exchange(true)) return;
  std::thread([] {
    for (const RecentGame& g : recent_list()) {
      if (!g.name.empty()) continue;
      const std::string n = name_of(g.place);
      if (n.empty()) continue;
      std::lock_guard<std::mutex> l(g_mu);
      for (auto& x : g_list)
        if (x.place == g.place) x.name = n;
      save();
    }
    g_fetching = false;
  }).detach();
}

}  // namespace tonic
