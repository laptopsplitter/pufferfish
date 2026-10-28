#pragma once
// apkfiles.hpp - unpacks Roblox's signed APKs, and at every start checks each
// installed file byte for byte against them (nothing missing or added).

#include <string>

namespace tonic {

// From BASE (the APK with the assets) and ARCH (the one with lib/x86_64; may
// be the same file): lib/x86_64/* into DIR/lib, assets/* into DIR/assets.
bool apk_unpack(const std::string& base, const std::string& arch, const std::string& dir, std::string& err);

// DIR/lib and DIR/assets hold exactly what the APKs hold, byte for byte.
bool apk_files_match(const std::string& base, const std::string& arch, const std::string& dir, std::string& err);

}  // namespace tonic
