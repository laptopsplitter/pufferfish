// make test-privacy: which lookups "Block Roblox analytics" answers "no such host".
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "../src/privacy.hpp"

int main() {
  int fails = 0;
  auto expect = [&](const char* h, bool want) {
    if (tonic::privacy_blocked(h) != want) printf("FAIL %s: expected %s\n", h, want ? "blocked" : "allowed"), ++fails;
  };
  // (config.cpp reads the file once: a fresh process for the "on" checks)
  if (!getenv("PRIVACY_ON")) {
    std::ofstream("tests/privacy-off.conf") << "\n";
    setenv("TONIC_CONFIG", "tests/privacy-off.conf", 1);
    expect("ecsv2.roblox.com", false);  // off by default
    std::ofstream("tests/privacy.conf") << "block_analytics=1\n";
    setenv("PRIVACY_ON", "1", 1);
    setenv("TONIC_CONFIG", "tests/privacy.conf", 1);
    return system("./tests/privacy-test") != 0 || fails;
  }
  for (const char* h : {"ecsv2.roblox.com", "ECSV2.Roblox.com.", "client-telemetry.roblox.com", "tracing.roblox.com",
                        "tracing-grpc.roblox.com", "uploads.backtrace.rbx.com", "sentry-relay.rbx.com", "x.ecsv2.roblox.com"})
    expect(h, true);
  for (const char* h : {"www.roblox.com", "apis.roblox.com", "assetdelivery.roblox.com", "auth.roblox.com", "notecsv2.roblox.com",
                        "ecsv2.roblox.com.evil.example", "users.roblox.com"})
    expect(h, false);
  printf(fails ? "privacy: %d failed\n" : "privacy: ok\n", fails);
  return fails != 0;
}
