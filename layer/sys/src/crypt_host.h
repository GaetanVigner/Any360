// The host's own hash library behind crypt.cpp's one-call SHA-1 and SHA-256 forms (crypt.h: ShaPath::kHost):
// Windows' CNG; no host library elsewhere.
#pragma once

#include <cstdint>

namespace any360::sys::crypt_host {

enum class Hash { kSha1, kSha256 };

struct Part {
  const uint8_t* data;
  uint32_t size;
};

// True when the host has a library to hash with (opened once, on first use).
bool Available();

// The digest of `parts` in order (20 bytes for SHA-1, 32 for SHA-256) into `out`, an HMAC-SHA-1 under `key` when
// `hmac` (the key as the standard takes it: hashed when longer than a block). False, `out` untouched, when the host
// could not.
bool Digest(Hash hash, bool hmac, const uint8_t* key, uint32_t key_size, const Part* parts, int count, uint8_t* out);

}  // namespace any360::sys::crypt_host
