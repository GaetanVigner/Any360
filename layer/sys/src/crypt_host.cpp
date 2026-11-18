#include "crypt_host.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace any360::sys::crypt_host {

#if defined(_WIN32)
namespace {

// The hash object lives in the caller's stack buffer (no allocation of ours); CNG's objects for SHA-1 and SHA-256,
// HMAC or not, are a few hundred bytes.
constexpr ULONG kObjectBytes = 1024;

struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ULONG object_bytes = 0;
  Algorithm(const wchar_t* id, ULONG flags) {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, id, nullptr, flags))) {
      handle = nullptr;
      return;
    }
    ULONG written = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes),
                                          sizeof object_bytes, &written, 0)) ||
        object_bytes > kObjectBytes) {
      BCryptCloseAlgorithmProvider(handle, 0);
      handle = nullptr;
    }
  }
};

// Opened once for the process (and left open: the host closes them with it).
struct Algorithms {
  Algorithm sha1{BCRYPT_SHA1_ALGORITHM, 0}, sha256{BCRYPT_SHA256_ALGORITHM, 0};
  Algorithm hmac_sha1{BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG};
  bool all() const { return sha1.handle && sha256.handle && hmac_sha1.handle; }
};

const Algorithms& Opened() {
  static const Algorithms algorithms;
  return algorithms;
}

}  // namespace

bool Available() {
  static const bool available = Opened().all();
  return available;
}

bool Digest(Hash hash, bool hmac, const uint8_t* key, uint32_t key_size, const Part* parts, int count, uint8_t* out) {
  if (!Available() || (hmac && hash != Hash::kSha1)) return false;
  const Algorithms& opened = Opened();
  const Algorithm& algorithm = hash == Hash::kSha256 ? opened.sha256 : hmac ? opened.hmac_sha1 : opened.sha1;
  alignas(16) UCHAR object[kObjectBytes];
  // CNG refuses an empty HMAC secret; one zero byte is the same key (the standard pads keys with zeros).
  static const UCHAR kZero = 0;
  const UCHAR* secret = hmac ? (key_size ? key : &kZero) : nullptr;
  const ULONG secret_size = hmac ? (key_size ? key_size : 1) : 0;
  BCRYPT_HASH_HANDLE handle = nullptr;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.handle, &handle, object, algorithm.object_bytes, const_cast<PUCHAR>(secret),
                                       secret_size, 0))) {
    return false;
  }
  bool ok = true;
  for (int i = 0; i < count && ok; ++i) {
    if (parts[i].size) ok = BCRYPT_SUCCESS(BCryptHashData(handle, const_cast<PUCHAR>(parts[i].data), parts[i].size, 0));
  }
  UCHAR digest[32];
  const ULONG digest_size = hash == Hash::kSha1 ? 20 : 32;
  ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(handle, digest, digest_size, 0));
  BCryptDestroyHash(handle);
  if (ok) {
    for (ULONG i = 0; i < digest_size; ++i) out[i] = digest[i];
  }
  return ok;
}

#else

bool Available() { return false; }

bool Digest(Hash, bool, const uint8_t*, uint32_t, const Part*, int, uint8_t*) { return false; }

#endif

}  // namespace any360::sys::crypt_host
