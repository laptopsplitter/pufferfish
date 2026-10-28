// apk.hpp - Roblox, the newest x86_64 build from APKPure. tonic installs it
// (a split .xapk bundle, or an .apk) into data/versions/VERSION/:
//   lib/      the APK's lib/x86_64/*.so (libroblox.so is what tonic loads)
//   assets/   the APK's assets/
//   apk/      the signed APKs themselves, to check the rest against at each start
// The version comes from the APK's AndroidManifest.xml (versionName).
// Several versions can be kept; config "apk_version" picks one.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tonic {

struct RobloxInstall {
  std::string version;  // e.g. 2.742.1059
  std::string lib;      // path of libroblox.so
  std::string assets;   // the unpacked assets/ folder
};

// The one to run (config "apk_version", else the newest installed). False:
// none at all.
bool apk_current(RobloxInstall& out);
std::vector<std::string> apk_versions();  // installed, oldest first

// The newest x86_64 Roblox from APKPure, downloaded and installed (in the
// background), only once its signature shows it is Roblox's own, unmodified
// APK (apksig.hpp). The signed APKs are kept with it (apkfiles.hpp). PROGRESS
// gets short status lines; DONE gets the installed version, or "" and an
// error. Thread-safe callbacks.
void apk_fetch_latest(std::function<void(const std::string&)> progress,
                      std::function<void(const std::string& version, const std::string& error)> done);
// How far the download is (0-1); -1 when unknown or not downloading. Any thread.
float apk_progress_fraction();

// Before each start: RI's APKs still carry Roblox's signature, and every
// installed file is exactly as in them (nothing changed, missing or added).
// False: WHY says what is not (for the log); OLD: it was installed by an
// older tonic, which did not keep the APKs to check against.
bool apk_check_install(const RobloxInstall& ri, std::string& why, bool* old = nullptr);

// Forget the current one: its files are deleted, and tonic downloads Roblox
// again.
void apk_forget();

// A file chooser (zenity or kdialog) is installed.
bool can_choose_file();
// A file, through it, in the background ("" if cancelled): PATTERNS like "*.png *.jpg".
void choose_file(const std::string& title, const std::string& label, const std::string& patterns,
                 std::function<void(const std::string& path)> done);

}  // namespace tonic
