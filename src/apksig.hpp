#pragma once
// apksig.hpp - is an APK Roblox's own, unmodified? Checked the way Android
// checks an app before installing it: APK Signature Scheme v3 (or v2), the
// signature over the whole file's contents verified with the signer's public
// key, and that signer's certificate one of Roblox's (pinned SHA-256
// fingerprints). Any changed byte, or another signer, fails.

#include <string>
#include <vector>

namespace tonic {

struct ApkSignature {
  bool valid = false;                 // the signature checks out (whoever signed it)
  std::vector<std::string> certs;     // the signers' certificates, SHA-256, lowercase hex
  std::string error;                  // why not valid
};

ApkSignature apk_signature(const std::string& path);

// Valid, and signed with one of Roblox's certificates. WHY says what failed.
bool apk_signed_by_roblox(const std::string& path, std::string& why);

}  // namespace tonic
