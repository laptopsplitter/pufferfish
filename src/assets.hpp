// assets.hpp - AAssetManager/AAsset served from the APK's assets/ folder
// unpacked on disk (see assets.cpp).
#pragma once

#include <string>

namespace tonic {

void set_asset_root(const std::string& dir);  // default data/apk/assets
const std::string& asset_root();
void register_asset_shims();  // before relocation
void print_asset_summary();

}  // namespace tonic
