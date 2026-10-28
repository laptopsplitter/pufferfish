#pragma once
// "Block Roblox analytics" (config block_analytics, off by default): Roblox's
// own reporting hosts are answered "no such host", like a DNS blocklist
// (Pi-hole) would. Only lookups change; Roblox's code is not touched. Only
// reporting: analytics events, telemetry, tracing, crash and error reports.
// Never the API, moderation, safety or anti-cheat hosts.
// (The Android app's third-party trackers, AppsFlyer, Firebase and the like,
// are Java and never run in tonic at all.)
namespace tonic {

bool privacy_blocked(const char* host);  // true: answer "no such host" (counted, logged once per host)

}  // namespace tonic
