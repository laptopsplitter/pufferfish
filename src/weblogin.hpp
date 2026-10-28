// weblogin.hpp - "Sign in on roblox.com": Roblox's website in tonic-webview
// --login; the session is checked and saved (cookies.hpp). The approach comes
// from Mocktail Plus.
#pragma once

#include <string>

namespace tonic {

bool weblogin_start();           // opens the window; false if it is already open or cannot start
bool weblogin_running();
std::string weblogin_status();   // for the Account row: "" or what happened last
void weblogin_forget();          // (logging out)
void weblogin_apply_identity();  // before Roblox starts: the signed-in user for NativeUserJavaInterface

// Roblox's own app storage (files/appData/LocalStorage/appStorage.json, what
// the app keeps between starts), for this instance. String values.
void app_storage_set(const std::string& key, const std::string& value);
void app_storage_erase(const std::string& key);
std::string app_storage_get(const std::string& key);

}  // namespace tonic
