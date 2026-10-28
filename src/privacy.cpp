#include "privacy.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "config.hpp"

namespace tonic {

namespace {

// The reporting hosts named in libroblox.so: each one, and anything under it.
const char* const kHosts[] = {
    "ecsv2.roblox.com",             // analytics event stream
    "ecs.roblox.com",               //   (older name of the same)
    "client-telemetry.roblox.com",  // client telemetry
    "tracing.roblox.com",           // performance tracing
    "tracing-grpc.roblox.com",
    "uploads.backtrace.rbx.com",  // crash reports
    "sentry-relay.rbx.com",       // error reports
};

std::mutex g_mu;
std::set<std::string> g_logged;

bool under(const std::string& h, const std::string& base) {
  return h == base || (h.size() > base.size() && h.compare(h.size() - base.size(), base.size(), base) == 0 &&
                       h[h.size() - base.size() - 1] == '.');
}

}  // namespace

bool privacy_blocked(const char* host) {
  if (!host || !config_bool("block_analytics", false)) return false;
  std::string h = host;
  for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!h.empty() && h.back() == '.') h.pop_back();
  for (const char* b : kHosts)
    if (under(h, b)) {
      std::lock_guard<std::mutex> l(g_mu);
      if (g_logged.insert(h).second) fprintf(stderr, "[privacy] blocked %s (Roblox analytics; Settings > Performance)\n", h.c_str());
      return true;
    }
  return false;
}

}  // namespace tonic
