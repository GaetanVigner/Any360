// xboxkrnl.exe cryptography a title calls directly: SHA-1 over up to three buffers. The console-key operations answer
// as a machine without its keys: signatures are accepted, keys are not handed out.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "guest.h"

namespace any360::sys {

class Sha1 {
 public:
  Sha1();
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
// The checksum of a console-signed blob: not computed (no known title depends on its value); the digest is zeroed.
void XeCryptRotSumSha(Guest g, uint32_t digest, uint32_t digest_size);
// Signatures made with the console's keys: accepted (1), as there is no key to check them against.
uint32_t XeCryptBnQwBeSigVerify();
// The console's keys are not available: STATUS_UNSUCCESSFUL, the buffer zeroed.
uint32_t XeKeysGetKey(Guest g, uint32_t buffer, uint32_t size_ptr);

}  // namespace any360::sys
