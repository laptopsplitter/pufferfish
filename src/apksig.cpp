// apksig.cpp - see apksig.hpp. APK Signature Scheme v2/v3, as specified at
// source.android.com/docs/security/features/apksigning, verified with OpenSSL.
#include "apksig.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace tonic {
namespace {

// Roblox's signing certificates (com.roblox.client), SHA-256 of the DER
// certificate. Two: Roblox's key and the one it rotated to. The same list as
// Mocktail's config/roblox_signing_certificates.json; TONIC_TRUSTED_CERT adds
// one more (should Roblox ever rotate again before tonic is updated).
const char* const kRobloxCerts[] = {
    "2bebd189e8d3106401347056c93d045b61e20e22d0c3cbed85474aeb00a3d12a",
    "44932ea35a17a267372d71b54d1a0cb3da0dca5113e94406ae2fe18090ba1477",
};

constexpr uint32_t kV2 = 0x7109871a, kV3 = 0xf05368c0;
constexpr size_t kChunk = 1 << 20;

struct Map {
  const uint8_t* p = nullptr;
  size_t n = 0;
  ~Map() {
    if (p) munmap(const_cast<uint8_t*>(p), n);
  }
};

uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24; }
uint64_t le64(const uint8_t* b) { return le32(b) | static_cast<uint64_t>(le32(b + 4)) << 32; }

// A view, read as a sequence of length-prefixed (u32) parts.
struct View {
  const uint8_t* p;
  size_t n;
  bool take(View& out) {  // the next length-prefixed part
    if (n < 4) return false;
    const uint32_t len = le32(p);
    if (len > n - 4) return false;
    out = {p + 4, len};
    p += 4 + len, n -= 4 + len;
    return true;
  }
  bool u32(uint32_t& v) {
    if (n < 4) return false;
    v = le32(p);
    p += 4, n -= 4;
    return true;
  }
};

std::string hex(const uint8_t* d, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += k[d[i] >> 4], s += k[d[i] & 15];
  return s;
}

// The signature algorithms (IDs from the spec): digest and padding.
struct Algo {
  uint32_t id;
  const EVP_MD* (*md)();
  int kind;  // 0 RSA PKCS#1 v1.5, 1 RSA-PSS, 2 ECDSA, 3 DSA
  int strength;
};
const Algo kAlgos[] = {
    {0x0101, EVP_sha256, 1, 2}, {0x0102, EVP_sha512, 1, 4}, {0x0103, EVP_sha256, 0, 1}, {0x0104, EVP_sha512, 0, 3},
    {0x0201, EVP_sha256, 2, 2}, {0x0202, EVP_sha512, 2, 4}, {0x0301, EVP_sha256, 3, 1},
};
const Algo* algo(uint32_t id) {
  for (const Algo& a : kAlgos)
    if (a.id == id) return &a;
  return nullptr;  // (verity-based ones: not used without a v2/v3 plain one alongside)
}

bool verify(const Algo& a, const View& key, const View& sig, const View& data) {
  const uint8_t* kp = key.p;
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pk(d2i_PUBKEY(nullptr, &kp, static_cast<long>(key.n)), EVP_PKEY_free);
  if (!pk) return false;
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  EVP_PKEY_CTX* pctx = nullptr;
  if (!ctx || EVP_DigestVerifyInit(ctx.get(), &pctx, a.md(), nullptr, pk.get()) != 1) return false;
  if (a.kind == 1) {
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, EVP_MD_size(a.md())) != 1 || EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, a.md()) != 1)
      return false;
  }
  return EVP_DigestVerify(ctx.get(), sig.p, sig.n, data.p, data.n) == 1;
}

// The whole file's digest as v2/v3 define it: 1 MiB chunks of the entries,
// the central directory and the end record (its directory offset pointing at
// the signing block), each hashed with a prefix, then all of those together.
bool content_digest(const Map& m, size_t block_at, size_t cd_at, size_t eocd_at, const EVP_MD* md, std::vector<uint8_t>& out) {
  std::vector<uint8_t> eocd(m.p + eocd_at, m.p + m.n);
  for (int i = 0; i < 4; ++i) eocd[16 + i] = static_cast<uint8_t>(block_at >> (8 * i));
  struct Part { const uint8_t* p; size_t n; } parts[3] = {{m.p, block_at}, {m.p + cd_at, eocd_at - cd_at}, {eocd.data(), eocd.size()}};
  size_t chunks = 0;
  for (const Part& s : parts) chunks += (s.n + kChunk - 1) / kChunk;
  const int dl = EVP_MD_size(md);
  std::vector<uint8_t> all(5 + chunks * static_cast<size_t>(dl));
  all[0] = 0x5a;
  for (int i = 0; i < 4; ++i) all[1 + i] = static_cast<uint8_t>(chunks >> (8 * i));
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> c(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  size_t at = 5;
  for (const Part& s : parts)
    for (size_t off = 0; off < s.n; off += kChunk) {
      const size_t len = std::min(kChunk, s.n - off);
      uint8_t pre[5] = {0xa5, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len >> 16),
                        static_cast<uint8_t>(len >> 24)};
      unsigned got = 0;
      if (EVP_DigestInit_ex(c.get(), md, nullptr) != 1 || EVP_DigestUpdate(c.get(), pre, 5) != 1 ||
          EVP_DigestUpdate(c.get(), s.p + off, len) != 1 || EVP_DigestFinal_ex(c.get(), all.data() + at, &got) != 1)
        return false;
      at += got;
    }
  out.resize(static_cast<size_t>(dl));
  unsigned got = 0;
  return EVP_Digest(all.data(), all.size(), out.data(), &got, md, nullptr) == 1;
}

}  // namespace

ApkSignature apk_signature(const std::string& path) {
  ApkSignature r;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return r.error = "cannot open it", r;
  struct stat st{};
  Map m;
  if (fstat(fd, &st) == 0 && st.st_size > 22) {
    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) m.p = static_cast<const uint8_t*>(p), m.n = static_cast<size_t>(st.st_size);
  }
  close(fd);
  if (!m.p) return r.error = "cannot read it", r;

  // The zip's end record, the central directory before it, the signing block before that.
  size_t eocd_at = SIZE_MAX;
  for (size_t i = m.n - 22 + 1; i-- > (m.n > 22 + 65535 ? m.n - 22 - 65535 : 0);)
    if (le32(m.p + i) == 0x06054b50 && i + 22 + (m.p[i + 20] | m.p[i + 21] << 8) == m.n) {
      eocd_at = i;
      break;
    }
  if (eocd_at == SIZE_MAX) return r.error = "not a zip file", r;
  const size_t cd_at = le32(m.p + eocd_at + 16), cd_size = le32(m.p + eocd_at + 12);
  if (cd_at + cd_size != eocd_at || cd_at < 32) return r.error = "damaged zip directory", r;
  if (memcmp(m.p + cd_at - 16, "APK Sig Block 42", 16) != 0) return r.error = "not signed the current way (no APK signing block)", r;
  const uint64_t bsize = le64(m.p + cd_at - 24);
  if (bsize < 24 || bsize > cd_at - 8) return r.error = "damaged signing block", r;
  const size_t block_at = cd_at - static_cast<size_t>(bsize) - 8;
  if (le64(m.p + block_at) != bsize) return r.error = "damaged signing block", r;

  View v2{}, v3{};
  for (size_t p = block_at + 8; p + 12 <= cd_at - 24;) {
    const uint64_t len = le64(m.p + p);
    if (len < 4 || len > cd_at - 24 - p - 8) return r.error = "damaged signing block", r;
    const uint32_t id = le32(m.p + p + 8);
    if (id == kV2) v2 = {m.p + p + 12, static_cast<size_t>(len - 4)};
    if (id == kV3) v3 = {m.p + p + 12, static_cast<size_t>(len - 4)};
    p += 8 + static_cast<size_t>(len);
  }
  const bool use_v3 = v3.p != nullptr;  // (v3 is what current Android goes by when present)
  View signers;
  if (!(use_v3 ? v3 : v2).p || !View(use_v3 ? v3 : v2).take(signers)) return r.error = "no v2/v3 signature", r;

  int checked = 0;
  View signer;
  while (signers.take(signer)) {
    View signed_data, sigs, key;
    uint32_t min_sdk = 0, max_sdk = 0;
    if (!signer.take(signed_data)) return r.error = "damaged signer", r;
    if (use_v3 && (!signer.u32(min_sdk) || !signer.u32(max_sdk))) return r.error = "damaged signer", r;
    if (!signer.take(sigs) || !signer.take(key)) return r.error = "damaged signer", r;
    // The strongest signature this signer has that we know.
    const Algo* best = nullptr;
    View best_sig{}, s;
    while (sigs.take(s)) {
      uint32_t id;
      View sig;
      if (!s.u32(id) || !s.take(sig)) return r.error = "damaged signature", r;
      const Algo* a = algo(id);
      if (a && (!best || a->strength > best->strength)) best = a, best_sig = sig;
    }
    if (!best) return r.error = "no signature algorithm tonic knows", r;
    if (!verify(*best, key, best_sig, signed_data)) return r.error = "the signature does not match", r;
    // Inside what was signed: the file's digest and the certificates.
    View digests, certs, d, digest_val{};
    View sd = signed_data;
    if (!sd.take(digests) || !sd.take(certs)) return r.error = "damaged signed data", r;
    while (digests.take(d)) {
      uint32_t id;
      View val;
      if (!d.u32(id) || !d.take(val)) return r.error = "damaged digest", r;
      if (id == best->id) digest_val = val;
    }
    if (!digest_val.p) return r.error = "no digest for its signature", r;
    std::vector<uint8_t> mine;
    if (!content_digest(m, block_at, cd_at, eocd_at, best->md(), mine)) return r.error = "could not hash it", r;
    if (mine.size() != digest_val.n || memcmp(mine.data(), digest_val.p, mine.size()) != 0)
      return r.error = "the file was changed after it was signed", r;
    View cert;
    if (!certs.take(cert)) return r.error = "no certificate", r;
    // The certificate's key must be the key the signature was checked with.
    const uint8_t* cp = cert.p;
    std::unique_ptr<X509, decltype(&X509_free)> x(d2i_X509(nullptr, &cp, static_cast<long>(cert.n)), X509_free);
    if (!x) return r.error = "unreadable certificate", r;
    unsigned char* der = nullptr;
    const int dlen = i2d_PUBKEY(X509_get0_pubkey(x.get()), &der);
    const bool same = dlen > 0 && static_cast<size_t>(dlen) == key.n && memcmp(der, key.p, key.n) == 0;
    OPENSSL_free(der);
    if (!same) return r.error = "the certificate is not the signer's", r;
    uint8_t fp[32];
    unsigned fl = 0;
    EVP_Digest(cert.p, cert.n, fp, &fl, EVP_sha256(), nullptr);
    r.certs.push_back(hex(fp, fl));
    ++checked;
  }
  if (!checked) return r.error = "no signer", r;
  r.valid = true;
  return r;
}

bool apk_signed_by_roblox(const std::string& path, std::string& why) {
  const ApkSignature s = apk_signature(path);
  if (!s.valid) {
    why = "not a genuine Roblox APK: " + s.error;
    return false;
  }
  const char* extra = getenv("TONIC_TRUSTED_CERT");
  for (const std::string& c : s.certs) {
    for (const char* k : kRobloxCerts)
      if (c == k) return true;
    if (extra && c == extra) {
      fprintf(stderr, "[apk] signed with %s, trusted by TONIC_TRUSTED_CERT\n", c.c_str());
      return true;
    }
  }
  why = "not signed by Roblox (certificate " + (s.certs.empty() ? std::string("none") : s.certs[0].substr(0, 16) + "...") + ")";
  return false;
}

}  // namespace tonic
