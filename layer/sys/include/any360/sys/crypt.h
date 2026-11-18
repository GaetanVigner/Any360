// xboxkrnl.exe cryptography a title calls directly: SHA-1 and SHA-256 (one call or streamed through a guest state),
// HMAC-SHA-1, RC4, AES-128 and triple DES (ECB and CBC), byte-for-byte as the reference lays its states out. The
// console-key operations answer as a machine without its keys: signatures are accepted, keys are not handed out, the
// key-obfuscation key is the reference's (zero).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "guest.h"

namespace any360::sys {

class Sha1 {
 public:
  Sha1();
  // Resumed from a guest XECRYPT_SHA_STATE's digest, buffered bytes and byte count.
  Sha1(const uint32_t h[5], const uint8_t* buffer, uint32_t count);
  const uint32_t* digest() const { return h_; }
  const uint8_t* buffer() const { return buffer_; }
  uint64_t total() const { return total_; }
  void Update(const uint8_t* data, size_t size);
  std::array<uint8_t, 20> Final();

 private:
  void Block(const uint8_t* block);
  uint32_t h_[5];
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// XeCryptSha: the digest of the three buffers (null or empty ones skipped), its first `output_size` bytes written.
void XeCryptSha(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3, uint32_t output,
                uint32_t output_size);
// Streamed SHA-1 on an XECRYPT_SHA_STATE (0x58 bytes: count, five digest words, 64 buffered bytes). Final writes the
// first `out_size` digest bytes and leaves the state's digest words as the padded blocks left them.
void XeCryptShaInit(Guest g, uint32_t state);
void XeCryptShaUpdate(Guest g, uint32_t state, uint32_t input, uint32_t size);
void XeCryptShaFinal(Guest g, uint32_t state, uint32_t out, uint32_t out_size);
// Streamed SHA-256 on an XECRYPT_SHA256_STATE (count, eight hash words, 64 buffered bytes). Final writes the first
// `out_size` hash bytes and, as the reference, the hash over the first 32 buffered bytes.
void XeCryptSha256Init(Guest g, uint32_t state);
void XeCryptSha256Update(Guest g, uint32_t state, uint32_t input, uint32_t size);
void XeCryptSha256Final(Guest g, uint32_t state, uint32_t out, uint32_t out_size);
// MD5 (RFC 1321), as Sha1 above.
class Md5 {
 public:
  Md5();
  // Resumed from a guest XECRYPT_MD5_STATE's words A-D, buffered bytes and byte count.
  Md5(const uint32_t abcd[4], const uint8_t* buffer, uint32_t count);
  const uint32_t* words() const { return abcd_; }
  const uint8_t* buffer() const { return buffer_; }
  uint64_t total() const { return total_; }
  void Update(const uint8_t* data, size_t size);
  std::array<uint8_t, 16> Final();

 private:
  void Block(const uint8_t* block);
  uint32_t abcd_[4];
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// Step 12 (docs/SYS_STUBS.md: what Xenia Canary implements and the reference stubs; the source of each answer is
// Xenia's xboxkrnl_crypt.cc, the hashes and ciphers checked against their standards' test vectors).
// XeCryptMd5 / XeCryptSha256: the digest of up to three buffers (null or empty ones skipped), its first `output_size`
// bytes written.
void XeCryptMd5(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3, uint32_t output,
                uint32_t output_size);
void XeCryptSha256(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3,
                   uint32_t output, uint32_t output_size);
// Streamed MD5 on an XECRYPT_MD5_STATE (0x54 bytes: count, words A-D, 64 buffered bytes). Final writes the first
// `out_size` digest bytes and leaves the digest in the state's words.
void XeCryptMd5Init(Guest g, uint32_t state);
void XeCryptMd5Update(Guest g, uint32_t state, uint32_t input, uint32_t size);
void XeCryptMd5Final(Guest g, uint32_t state, uint32_t out, uint32_t out_size);
// HMAC-MD5 of up to three buffers; a key over 64 bytes is cut to its first 64 (as Xenia assumes the console does,
// as for HMAC-SHA-1: not checked on a console).
void XeCryptHmacMd5(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                    uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size);
// Streamed HMAC-SHA-1 on an XECRYPT_HMACSHA_STATE (0xB0 bytes: the inner then the outer XECRYPT_SHA_STATE). Init
// starts both on the key's pads, a key over 64 bytes cut to its first 64 as the console does; Final finishes the
// inner hash, feeds its digest to the outer one and writes the first `out_size` bytes of the result.
void XeCryptHmacShaInit(Guest g, uint32_t state, uint32_t key, uint32_t key_size);
void XeCryptHmacShaUpdate(Guest g, uint32_t state, uint32_t input, uint32_t size);
void XeCryptHmacShaFinal(Guest g, uint32_t state, uint32_t out, uint32_t out_size);
// Whether XeCryptHmacSha's digest of the buffers starts with the first `verify_size` (at most 20) bytes given.
uint32_t XeCryptHmacShaVerify(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2,
                              uint32_t size2, uint32_t in3, uint32_t size3, uint32_t verify, uint32_t verify_size);
// Single DES on an XECRYPT_DES_STATE (one schedule of 16 subkeys, laid out as one third of XECRYPT_DES3_STATE).
void XeCryptDesKey(Guest g, uint32_t state, uint32_t key);
void XeCryptDesEcb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt);
void XeCryptDesCbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt);
// HMAC-SHA-1 of up to three buffers (empty ones skipped); a key over 64 bytes is hashed first.
void XeCryptHmacSha(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                    uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size);
// RC4 on an XECRYPT_RC4_STATE (S, i, j). As the reference, the key schedule cycles over 16 key bytes whatever the size.
void XeCryptRc4Key(Guest g, uint32_t state, uint32_t key, uint32_t key_size);
void XeCryptRc4Ecb(Guest g, uint32_t state, uint32_t data, uint32_t size);
void XeCryptRc4(Guest g, uint32_t key, uint32_t key_size, uint32_t data, uint32_t size);
// AES-128 on an XECRYPT_AES_STATE (0x160 bytes: the 11 encryption round keys, then the decryption ones). CBC
// works in 16-byte blocks and leaves the last ciphertext block in `feed`.
void XeCryptAesKey(Guest g, uint32_t state, uint32_t key);
void XeCryptAesEcb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt);
void XeCryptAesCbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt);
// Triple DES (encrypt-decrypt-encrypt with three keys) on an XECRYPT_DES3_STATE: three schedules of 16 48-bit
// subkeys, each a little-endian 64-bit word as the reference stores them. Blocks are big-endian 64-bit words.
void XeCryptDes3Key(Guest g, uint32_t state, uint32_t keys);
void XeCryptDes3Ecb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt);
void XeCryptDes3Cbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt);
// Odd parity in each byte's low bit.
void XeCryptDesParity(Guest g, uint32_t in, uint32_t size, uint32_t out);
// Each 8-byte word byte-reversed, `count` words.
void XeCryptBnQwSwapDwQwLeBe(Guest g, uint32_t in, uint32_t out, uint32_t count);
// Random bytes from the host (the reference fills 0xFD).
void XeCryptRandom(Guest g, uint32_t buffer, uint32_t size);
// HMAC-SHA-1 with a console key: only key 0x19 (the retail key the reference knows); STATUS_UNSUCCESSFUL otherwise.
uint32_t XeKeysHmacSha(Guest g, uint32_t key_number, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3,
                       uint32_t size3, uint32_t out, uint32_t out_size);
// Keys obscured with the key-obfuscation key (AES-ECB): obscuring, and CBC or HMAC with an obscured key.
uint32_t XeKeysObscureKey(Guest g, uint32_t in, uint32_t out);
uint32_t XeKeysAesCbcUsingKey(Guest g, uint32_t obscured_key, uint32_t in, uint32_t size, uint32_t out, uint32_t feed,
                              uint32_t encrypt);
uint32_t XeKeysHmacShaUsingKey(Guest g, uint32_t obscured_key, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                               uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size);

// The checksum of a console-signed blob: not computed (no known title depends on its value); the digest is zeroed.
void XeCryptRotSumSha(Guest g, uint32_t digest, uint32_t digest_size);
// Signatures made with the console's keys: accepted (1), as there is no key to check them against.
uint32_t XeCryptBnQwBeSigVerify();
// XeCryptBnQwNeRsaPubCrypt: out = in ^ e mod n with the public key at `key` (XECRYPT_RSA: the size in 64-bit words,
// the exponent, 8 bytes of padding, then the modulus). Numbers are arrays of big-endian 64-bit words, least significant
// word first. Returns 1, or 0 for a key of no size or an input not below the modulus (which the reference's
// BCrypt refuses).
uint32_t XeCryptBnQwNeRsaPubCrypt(Guest g, uint32_t in, uint32_t out, uint32_t key);
// The same for a PKCS #1 signature (1, as the reference).
uint32_t XeCryptBnDwLePkcs1Verify();
// The console's keys are not available: STATUS_UNSUCCESSFUL, the buffer zeroed.
uint32_t XeKeysGetKey(Guest g, uint32_t buffer, uint32_t size_ptr);

}  // namespace any360::sys
