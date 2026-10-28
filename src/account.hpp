#pragma once
// Settings on your Roblox account, changed the way roblox.com's own settings
// page changes them (Roblox's web API, signed in with tonic's saved login).
#include <string>

namespace tonic {

// Your account's theme ("dark" | "light"): the Roblox app takes its theme from
// the account, so this is what switches Roblox's menus (and roblox.com's).
// In the background; account_status() says how it went.
void account_set_theme(const std::string& theme);
std::string account_status();

}  // namespace tonic
