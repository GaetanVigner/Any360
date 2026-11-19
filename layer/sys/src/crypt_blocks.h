// The compression functions of the hashes in crypt.cpp: whole 64-byte blocks, SHA-1 and SHA-256 on the processor's
// SHA instructions (x86-64 SHA-NI) when crypt.h's ShaPath is kProcessor, on the portable C++ otherwise.
#pragma once

#include <cstddef>
#include <cstdint>

namespace any360::sys::crypt_blocks {

// `blocks` 64-byte blocks of `data` into the state (SHA-1: five words, SHA-256: eight; host-order words).
void Sha1(uint32_t h[5], const uint8_t* data, size_t blocks);
void Sha256(uint32_t h[8], const uint8_t* data, size_t blocks);
// The portable paths, whatever the processor (the tests' reference for the accelerated ones).
void Sha1Portable(uint32_t h[5], const uint8_t* data, size_t blocks);
void Sha256Portable(uint32_t h[8], const uint8_t* data, size_t blocks);
// SHA-512 (FIPS 180-4): `blocks` 128-byte blocks into eight host-order 64-bit words. x86-64's SHA-NI has no SHA-512:
// on a processor with BMI2 the schedule runs in SSE registers and the rounds on rorx, the portable C++ elsewhere.
void Sha512(uint64_t h[8], const uint8_t* data, size_t blocks);
void Sha512Portable(uint64_t h[8], const uint8_t* data, size_t blocks);
// MD5 (little-endian words).
void Md5(uint32_t abcd[4], const uint8_t* data, size_t blocks);

}  // namespace any360::sys::crypt_blocks
