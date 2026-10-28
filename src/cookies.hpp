// cookies.hpp - the session cookies Android keeps in CookieManager, kept in
// data/files/tonic-cookies.txt (0600, or $TONIC_COOKIES). Only names are logged.
#pragma once
#include <string>

#include "elfimg.hpp"

namespace tonic {

void cookies_init();                        // before the library runs
void cookies_enable(const ElfImage& img);   // "tonic cookies" in the script: restore + register
void cookies_storage_init();                // LocalStorageManager.getAllocatableBytes
bool cookies_have_login();
// A session from tonic's roblox.com sign-in window (weblogin.hpp): the saved
// login, replacing any other; Roblox starts signed in with it.
void cookies_save_login(const std::string& value, long long expires);                 // a saved Roblox login (this instance)
void cookies_logout();
// The saved session value, for tonic's own requests to Roblox as you
// (account.hpp). Never log it or put it on a command line.
std::string cookies_session();                      // deletes the saved login; nothing is saved after

}  // namespace tonic
