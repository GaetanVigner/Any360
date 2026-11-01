#include "any360/sys/crypt.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace any360::sys {

Sha1::Sha1() : h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::Block(const uint8_t* block) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
  }
  for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = std::rotl(b, 30);
    b = a;
    a = t;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
}

void Sha1::Update(const uint8_t* data, size_t size) {
  total_ += size;
  while (size) {
    const size_t n = std::min(size, sizeof buffer_ - buffered_);
    std::memcpy(buffer_ + buffered_, data, n);
    buffered_ += n;
    data += n;
    size -= n;
    if (buffered_ == sizeof buffer_) {
      Block(buffer_);
      buffered_ = 0;
    }
  }
}

std::array<uint8_t, 20> Sha1::Final() {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  Update(&one, 1);
  while (buffered_ != 56) Update(&zero, 1);
  uint8_t length[8];
  for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
  Update(length, 8);
  std::array<uint8_t, 20> digest;
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 4; ++j) digest[4 * i + j] = uint8_t(h_[i] >> (24 - 8 * j));
  }
  return digest;
}

void XeCryptSha(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3, uint32_t output,
                uint32_t output_size) {
  Sha1 sha;
  if (in1 && size1) sha.Update(g.host(in1), size1);
  if (in2 && size2) sha.Update(g.host(in2), size2);
  if (in3 && size3) sha.Update(g.host(in3), size3);
  const auto digest = sha.Final();
  std::memcpy(g.host(output), digest.data(), std::min<uint32_t>(20, output_size));
}

void XeCryptRotSumSha(Guest g, uint32_t digest, uint32_t digest_size) {
  if (digest && digest_size) std::memset(g.host(digest), 0, digest_size);
}

uint32_t XeCryptBnQwBeSigVerify() { return 1; }

uint32_t XeKeysGetKey(Guest g, uint32_t buffer, uint32_t size_ptr) {
  if (buffer && size_ptr) std::memset(g.host(buffer), 0, g.Load32(size_ptr));
  return 0xC0000001;
}

}  // namespace any360::sys
