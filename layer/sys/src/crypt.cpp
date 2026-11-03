#include "any360/sys/crypt.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <random>

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
  // Only the low 32 bits of the bit count, as the reference's SHA-1 writes it (the same below 512 MB).
  uint8_t length[8] = {};
  for (int i = 4; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
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

// --- streamed hashes, HMAC, RC4, AES-128, triple DES ------------------------------------------------------------

Sha1::Sha1(const uint32_t h[5], const uint8_t* buffer, uint32_t count) : buffered_(count % 64), total_(count) {
  std::memcpy(h_, h, sizeof h_);
  std::memcpy(buffer_, buffer, buffered_);
}

namespace {

constexpr uint32_t kShaCount = 0, kShaDigest = 4, kShaBuffer = 0x18, kShaStateSize = 0x58;
constexpr uint32_t kSha256Count = 0, kSha256Hash = 4, kSha256Buffer = 0x24;

Sha1 LoadSha1(Guest g, uint32_t state) {
  uint32_t h[5];
  for (int i = 0; i < 5; ++i) h[i] = g.Load32(state + kShaDigest + 4 * i);
  return Sha1(h, g.host(state + kShaBuffer), g.Load32(state + kShaCount));
}

void StoreDigest(Guest g, uint32_t state, const Sha1& sha) {
  for (int i = 0; i < 5; ++i) g.Store32(state + kShaDigest + 4 * i, sha.digest()[i]);
}

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Sha256Block(uint32_t h[8], const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], gg = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & gg)) + kSha256K[i] + w[i];
    const uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = gg, gg = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += gg, h[7] += hh;
}

// The digest of an HMAC-SHA-1 with a host key.
std::array<uint8_t, 20> HmacSha(Guest g, const uint8_t* key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2,
                                uint32_t size2, uint32_t in3, uint32_t size3) {
  uint8_t inner[64], outer[64], k[64];
  std::memset(inner, 0x36, sizeof inner);
  std::memset(outer, 0x5C, sizeof outer);
  if (key_size > 64) {
    Sha1 hashed;
    hashed.Update(key, key_size);
    const auto digest = hashed.Final();
    std::memcpy(k, digest.data(), 20);
    key_size = 20;
  } else {
    std::memcpy(k, key, key_size);
  }
  for (uint32_t i = 0; i < key_size; ++i) inner[i] = k[i] ^ 0x36, outer[i] = k[i] ^ 0x5C;
  Sha1 sha;
  sha.Update(inner, 64);
  if (size1) sha.Update(g.host(in1), size1);
  if (size2) sha.Update(g.host(in2), size2);
  if (size3) sha.Update(g.host(in3), size3);
  const auto digest = sha.Final();
  Sha1 second;
  second.Update(outer, 64);
  second.Update(digest.data(), 20);
  return second.Final();
}

// --- RC4 on a state of S, i and j; the key schedule cycles over 16 key bytes whatever the key's size, as the
// reference ---

void Rc4Schedule(Guest g, uint8_t* s, uint32_t key) {
  s[0x100] = s[0x101] = 0;
  for (int x = 0; x < 256; ++x) s[x] = uint8_t(x);
  uint8_t j = 0;
  for (int x = 0; x < 256; ++x) {
    j = uint8_t(j + s[x] + g.Load8(key + x % 16));
    std::swap(s[x], s[j]);
  }
}

void Rc4Crypt(Guest g, uint8_t* s, uint32_t data, uint32_t size) {
  uint8_t& i = s[0x100];
  uint8_t& j = s[0x101];
  for (uint32_t n = 0; n < size; ++n) {
    i = uint8_t(i + 1);
    j = uint8_t(j + s[i]);
    std::swap(s[i], s[j]);
    g.Store8(data + n, g.Load8(data + n) ^ s[uint8_t(s[i] + s[j])]);
  }
}

// --- AES-128 ---

struct AesTables {
  uint8_t s[256], inv[256];
  AesTables() {
    // The S-box from the multiplicative inverse in GF(2^8) and the affine map.
    uint8_t p = 1, q = 1;
    do {
      p = uint8_t(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
      q ^= q << 1, q ^= q << 2, q ^= q << 4;
      if (q & 0x80) q ^= 0x09;
      const uint8_t x = uint8_t(q ^ std::rotl(q, 1) ^ std::rotl(q, 2) ^ std::rotl(q, 3) ^ std::rotl(q, 4) ^ 0x63);
      s[p] = x;
    } while (p != 1);
    s[0] = 0x63;
    for (int i = 0; i < 256; ++i) inv[s[i]] = uint8_t(i);
  }
};
const AesTables& Aes() {
  static const AesTables tables;
  return tables;
}

uint8_t Mul2(uint8_t a) { return uint8_t((a & 0x80) ? (a << 1) ^ 0x1B : a << 1); }
uint8_t Mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (; b; b >>= 1, a = Mul2(a)) {
    if (b & 1) r ^= a;
  }
  return r;
}

void AesSchedule(const uint8_t* key, uint8_t* round_keys) {
  static constexpr uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
  std::memcpy(round_keys, key, 16);
  for (int i = 4; i < 44; ++i) {
    uint8_t t[4];
    std::memcpy(t, round_keys + 4 * (i - 1), 4);
    if (i % 4 == 0) {
      const uint8_t first = t[0];
      t[0] = uint8_t(Aes().s[t[1]] ^ kRcon[i / 4 - 1]), t[1] = Aes().s[t[2]], t[2] = Aes().s[t[3]], t[3] = Aes().s[first];
    }
    for (int j = 0; j < 4; ++j) round_keys[4 * i + j] = round_keys[4 * (i - 4) + j] ^ t[j];
  }
}

// One block; the state is column-major as the bytes come (byte 4c + r is row r of column c).
void AesEncrypt(const uint8_t* round_keys, const uint8_t* in, uint8_t* out) {
  uint8_t st[16];
  for (int i = 0; i < 16; ++i) st[i] = in[i] ^ round_keys[i];
  for (int round = 1; round <= 10; ++round) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[4 * c + r] = Aes().s[st[4 * ((c + r) % 4) + r]];  // SubBytes, ShiftRows
    }
    if (round < 10) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t* a = t + 4 * c;
        const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        st[4 * c] = uint8_t(Mul2(a0) ^ Mul2(a1) ^ a1 ^ a2 ^ a3);
        st[4 * c + 1] = uint8_t(a0 ^ Mul2(a1) ^ Mul2(a2) ^ a2 ^ a3);
        st[4 * c + 2] = uint8_t(a0 ^ a1 ^ Mul2(a2) ^ Mul2(a3) ^ a3);
        st[4 * c + 3] = uint8_t(Mul2(a0) ^ a0 ^ a1 ^ a2 ^ Mul2(a3));
      }
    } else {
      std::memcpy(st, t, 16);
    }
    for (int i = 0; i < 16; ++i) st[i] ^= round_keys[16 * round + i];
  }
  std::memcpy(out, st, 16);
}

void AesDecrypt(const uint8_t* round_keys, const uint8_t* in, uint8_t* out) {
  uint8_t st[16];
  for (int i = 0; i < 16; ++i) st[i] = in[i] ^ round_keys[160 + i];
  for (int round = 9; round >= 0; --round) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[4 * ((c + r) % 4) + r] = Aes().inv[st[4 * c + r]];  // InvShiftRows, InvSubBytes
    }
    for (int i = 0; i < 16; ++i) t[i] ^= round_keys[16 * round + i];
    if (round > 0) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t* a = t + 4 * c;
        st[4 * c] = uint8_t(Mul(a[0], 14) ^ Mul(a[1], 11) ^ Mul(a[2], 13) ^ Mul(a[3], 9));
        st[4 * c + 1] = uint8_t(Mul(a[0], 9) ^ Mul(a[1], 14) ^ Mul(a[2], 11) ^ Mul(a[3], 13));
        st[4 * c + 2] = uint8_t(Mul(a[0], 13) ^ Mul(a[1], 9) ^ Mul(a[2], 14) ^ Mul(a[3], 11));
        st[4 * c + 3] = uint8_t(Mul(a[0], 11) ^ Mul(a[1], 13) ^ Mul(a[2], 9) ^ Mul(a[3], 14));
      }
    } else {
      std::memcpy(st, t, 16);
    }
  }
  std::memcpy(out, st, 16);
}

// The XECRYPT_AES_STATE: encryption round keys, then the decryption ones as the reference derives them (round 10's
// key, InvMixColumns of rounds 9 to 1, round 0's key).
void AesState(const uint8_t* key, uint8_t* state) {
  AesSchedule(key, state);
  uint8_t* dec = state + 176;
  std::memcpy(dec, state + 160, 16);
  for (int i = 1; i < 10; ++i) {
    const uint8_t* enc = state + 16 * (10 - i);
    uint8_t* d = dec + 16 * i;
    for (int c = 0; c < 16; c += 4) {
      uint8_t t = enc[c] ^ enc[c + 1] ^ enc[c + 2] ^ enc[c + 3];
      d[c] = uint8_t(t ^ enc[c] ^ Mul2(enc[c] ^ enc[c + 1]));
      d[c + 1] = uint8_t(t ^ enc[c + 1] ^ Mul2(enc[c + 1] ^ enc[c + 2]));
      d[c + 2] = uint8_t(t ^ enc[c + 2] ^ Mul2(enc[c + 2] ^ enc[c + 3]));
      d[c + 3] = uint8_t(t ^ enc[c + 3] ^ Mul2(enc[c + 3] ^ enc[c]));
      const uint8_t u = Mul2(Mul2(enc[c] ^ enc[c + 2])), v = Mul2(Mul2(enc[c + 1] ^ enc[c + 3]));
      t = Mul2(u ^ v);
      d[c] ^= t ^ u, d[c + 1] ^= t ^ v, d[c + 2] ^= t ^ u, d[c + 3] ^= t ^ v;
    }
  }
  std::memcpy(dec + 160, state, 16);
}

void AesCbc(Guest g, const uint8_t* round_keys, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt) {
  uint8_t* f = g.host(feed);
  for (uint32_t i = 0; i < size; i += 16, in += 16, out += 16) {
    if (encrypt) {
      for (int j = 0; j < 16; ++j) f[j] ^= g.Load8(in + j);
      AesEncrypt(round_keys, f, f);
      std::memcpy(g.host(out), f, 16);
    } else {
      uint8_t block[16];
      std::memcpy(block, g.host(in), 16);  // in case in == out
      AesDecrypt(round_keys, block, g.host(out));
      for (int j = 0; j < 16; ++j) g.host(out)[j] ^= f[j];
      std::memcpy(f, block, 16);
    }
  }
}

// --- DES, bits numbered from 1 at the most significant end as FIPS 46 does ---

constexpr uint8_t kDesIp[] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};
constexpr uint8_t kDesFp[] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25,
};
constexpr uint8_t kDesE[] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};
constexpr uint8_t kDesP[] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25,
};
constexpr uint8_t kDesPc1[] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2,
    59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39,
    31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37,
    29, 21, 13, 5, 28, 20, 12, 4,
};
constexpr uint8_t kDesPc2[] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
    26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
};
constexpr uint8_t kDesShifts[] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1,
};
constexpr uint8_t kDesS[] = {
    14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
    0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
    4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
    15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13,
    15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
    3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
    0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
    13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9,
    10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
    13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
    13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
    1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12,
    7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
    13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
    10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
    3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14,
    2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
    14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
    4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
    11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3,
    12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
    10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
    9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
    4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13,
    4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
    13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
    1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
    6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12,
    13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
    1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
    7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
    2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11,
};

uint64_t Permute(uint64_t value, int width, const uint8_t* table, int count) {
  uint64_t result = 0;
  for (int i = 0; i < count; ++i) result = result << 1 | ((value >> (width - table[i])) & 1);
  return result;
}

void DesSchedule(uint64_t key, uint64_t sub_keys[16]) {
  const uint64_t pc1 = Permute(key, 64, kDesPc1, 56);
  uint32_t c = uint32_t(pc1 >> 28) & 0x0FFFFFFF, d = uint32_t(pc1) & 0x0FFFFFFF;
  for (int i = 0; i < 16; ++i) {
    for (int j = 0; j < kDesShifts[i]; ++j) {
      c = (0x0FFFFFFF & (c << 1)) | (c >> 27);
      d = (0x0FFFFFFF & (d << 1)) | (d >> 27);
    }
    sub_keys[i] = Permute(uint64_t(c) << 28 | d, 56, kDesPc2, 48);
  }
}

uint32_t DesRound(uint32_t r, uint64_t k) {
  const uint64_t s_input = Permute(r, 32, kDesE, 48) ^ k;
  uint32_t s_output = 0;
  for (int i = 0; i < 8; ++i) {
    const uint32_t six = uint32_t(s_input >> (42 - 6 * i)) & 0x3F;
    const uint32_t row = (six >> 4 & 2) | (six & 1), column = six >> 1 & 0xF;
    s_output = s_output << 4 | (kDesS[64 * i + 16 * row + column] & 0xF);
  }
  return uint32_t(Permute(s_output, 32, kDesP, 32));
}

uint64_t Des(uint64_t block, const uint64_t sub_keys[16], bool decrypt) {
  block = Permute(block, 64, kDesIp, 64);
  uint32_t l = uint32_t(block >> 32), r = uint32_t(block);
  for (int i = 0; i < 16; ++i) {
    const uint32_t f = DesRound(r, sub_keys[decrypt ? 15 - i : i]);
    const uint32_t t = r;
    r = l ^ f, l = t;
  }
  return Permute(uint64_t(r) << 32 | l, 64, kDesFp, 64);
}

struct Des3 {
  uint64_t keys[3][16];
  Des3(Guest g, uint32_t state) {
    // The reference keeps each subkey as a host (little-endian) 64-bit word.
    std::memcpy(keys, g.host(state), sizeof keys);
    if constexpr (std::endian::native == std::endian::big) {
      for (auto& schedule : keys) {
        for (uint64_t& k : schedule) k = std::byteswap(k);
      }
    }
  }
  uint64_t Encrypt(uint64_t b) const { return Des(Des(Des(b, keys[0], false), keys[1], true), keys[2], false); }
  uint64_t Decrypt(uint64_t b) const { return Des(Des(Des(b, keys[2], true), keys[1], false), keys[0], true); }
};

constexpr uint8_t kRetailKey19[16] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9, 0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};
constexpr uint8_t kKeyObfuscationKey[16] = {};

// An obscured key in the clear: AES-ECB decrypted with the key-obfuscation key.
void Deobscure(Guest g, uint32_t obscured, uint8_t key[16]) {
  uint8_t round_keys[176];
  AesSchedule(kKeyObfuscationKey, round_keys);
  AesDecrypt(round_keys, g.host(obscured), key);
}

}  // namespace

void XeCryptShaInit(Guest g, uint32_t state) {
  std::memset(g.host(state), 0, kShaStateSize);
  StoreDigest(g, state, Sha1());
}

void XeCryptShaUpdate(Guest g, uint32_t state, uint32_t input, uint32_t size) {
  Sha1 sha = LoadSha1(g, state);
  if (size) sha.Update(g.host(input), size);
  StoreDigest(g, state, sha);
  g.Store32(state + kShaCount, uint32_t(sha.total()));
  std::memcpy(g.host(state + kShaBuffer), sha.buffer(), sha.total() % 64);
}

void XeCryptShaFinal(Guest g, uint32_t state, uint32_t out, uint32_t out_size) {
  Sha1 sha = LoadSha1(g, state);
  const auto digest = sha.Final();
  std::memcpy(g.host(out), digest.data(), std::min<uint32_t>(20, out_size));
  StoreDigest(g, state, sha);
}

void XeCryptSha256Init(Guest g, uint32_t state) {
  static constexpr uint32_t kInitial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memset(g.host(state), 0, kSha256Buffer + 64);
  for (int i = 0; i < 8; ++i) g.Store32(state + kSha256Hash + 4 * i, kInitial[i]);
}

void XeCryptSha256Update(Guest g, uint32_t state, uint32_t input, uint32_t size) {
  uint32_t h[8];
  for (int i = 0; i < 8; ++i) h[i] = g.Load32(state + kSha256Hash + 4 * i);
  const uint32_t count = g.Load32(state + kSha256Count);
  uint64_t done = count & ~63u;
  uint32_t buffered = count & 63;
  uint8_t* buffer = g.host(state + kSha256Buffer);
  const uint8_t* data = size ? g.host(input) : nullptr;
  // As the reference: top up a partial buffer, then whole blocks straight from the input, the rest buffered.
  if (buffered > 0) {
    while (size > 0 && buffered < 64) buffer[buffered++] = *data++, --size;
  }
  if (buffered == 64) {
    Sha256Block(h, buffer);
    done += 64;
    buffered = 0;
  }
  for (; size >= 64; data += 64, size -= 64, done += 64) Sha256Block(h, data);
  while (size > 0) buffer[buffered++] = *data++, --size;
  for (int i = 0; i < 8; ++i) g.Store32(state + kSha256Hash + 4 * i, h[i]);
  g.Store32(state + kSha256Count, uint32_t(done + buffered));
}

void XeCryptSha256Final(Guest g, uint32_t state, uint32_t out, uint32_t out_size) {
  uint32_t h[8];
  for (int i = 0; i < 8; ++i) h[i] = g.Load32(state + kSha256Hash + 4 * i);
  const uint32_t count = g.Load32(state + kSha256Count), buffered = count & 63;
  uint8_t blocks[128];
  std::memcpy(blocks, g.host(state + kSha256Buffer), 64);
  blocks[buffered] = 0x80;
  const uint32_t length = buffered < 56 ? 64 : 128;
  std::memset(blocks + buffered + 1, 0, length - buffered - 1);
  const uint64_t bits = uint64_t(count) * 8;
  for (int i = 0; i < 8; ++i) blocks[length - 8 + i] = uint8_t(bits >> (56 - 8 * i));
  Sha256Block(h, blocks);
  if (length == 128) Sha256Block(h, blocks + 64);
  uint8_t hash[32];
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 4; ++j) hash[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
  }
  std::memcpy(g.host(out), hash, std::min<uint32_t>(32, out_size));
  std::memcpy(g.host(state + kSha256Buffer), hash, 32);
}

void XeCryptHmacSha(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                    uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size) {
  const auto digest = HmacSha(g, g.host(key), key_size, in1, size1, in2, size2, in3, size3);
  std::memcpy(g.host(out), digest.data(), std::min<uint32_t>(20, out_size));
}

void XeCryptRc4Key(Guest g, uint32_t state, uint32_t key, uint32_t) { Rc4Schedule(g, g.host(state), key); }

void XeCryptRc4Ecb(Guest g, uint32_t state, uint32_t data, uint32_t size) { Rc4Crypt(g, g.host(state), data, size); }

void XeCryptRc4(Guest g, uint32_t key, uint32_t, uint32_t data, uint32_t size) {
  uint8_t state[0x102];  // on the host, as in the reference
  Rc4Schedule(g, state, key);
  Rc4Crypt(g, state, data, size);
}

void XeCryptAesKey(Guest g, uint32_t state, uint32_t key) {
  uint8_t k[16];
  std::memcpy(k, g.host(key), 16);
  AesState(k, g.host(state));
}

void XeCryptAesEcb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt) {
  uint8_t block[16];
  std::memcpy(block, g.host(in), 16);
  if (encrypt) {
    AesEncrypt(g.host(state), block, g.host(out));
  } else {
    AesDecrypt(g.host(state), block, g.host(out));
  }
}

void XeCryptAesCbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt) {
  uint8_t round_keys[176];
  std::memcpy(round_keys, g.host(state), sizeof round_keys);
  AesCbc(g, round_keys, in, size, out, feed, encrypt);
}

void XeCryptDes3Key(Guest g, uint32_t state, uint32_t keys) {
  for (int i = 0; i < 3; ++i) {
    uint64_t sub_keys[16];
    DesSchedule(g.Load64(keys + 8 * i), sub_keys);
    for (int k = 0; k < 16; ++k) {
      uint64_t little = sub_keys[k];
      if constexpr (std::endian::native == std::endian::big) little = std::byteswap(little);
      std::memcpy(g.host(state + 128 * i + 8 * k), &little, 8);
    }
  }
}

void XeCryptDes3Ecb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt) {
  const Des3 des(g, state);
  const uint64_t block = g.Load64(in);
  g.Store64(out, encrypt ? des.Encrypt(block) : des.Decrypt(block));
}

void XeCryptDes3Cbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt) {
  const Des3 des(g, state);
  uint64_t last = g.Load64(feed);
  for (uint32_t i = 0; i < size / 8; ++i) {
    const uint64_t block = g.Load64(in + 8 * i);
    if (encrypt) {
      last = des.Encrypt(block ^ last);
      g.Store64(out + 8 * i, last);
    } else {
      g.Store64(out + 8 * i, des.Decrypt(block) ^ last);
      last = block;
    }
  }
  g.Store64(feed, last);
}

void XeCryptDesParity(Guest g, uint32_t in, uint32_t size, uint32_t out) {
  for (uint32_t i = 0; i < size; ++i) {
    const uint8_t b = g.Load8(in + i);
    g.Store8(out + i, uint8_t((b & 0xFE) | (std::popcount(b) % 2 == 0 ? 1 : 0)));
  }
}

void XeCryptBnQwSwapDwQwLeBe(Guest g, uint32_t in, uint32_t out, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t v;
    std::memcpy(&v, g.host(in + 8 * i), 8);
    v = std::byteswap(v);
    std::memcpy(g.host(out + 8 * i), &v, 8);
  }
}

void XeCryptRandom(Guest g, uint32_t buffer, uint32_t size) {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::lock_guard lock(mutex);
  for (uint32_t i = 0; i < size; ++i) g.Store8(buffer + i, uint8_t(engine()));
}

uint32_t XeKeysHmacSha(Guest g, uint32_t key_number, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3,
                       uint32_t size3, uint32_t out, uint32_t out_size) {
  if (key_number != 0x19) return 0xC0000001;
  const auto digest = HmacSha(g, kRetailKey19, 16, in1, size1, in2, size2, in3, size3);
  std::memcpy(g.host(out), digest.data(), std::min<uint32_t>(20, out_size));
  return 0;
}

uint32_t XeKeysObscureKey(Guest g, uint32_t in, uint32_t out) {
  uint8_t round_keys[176], block[16];
  AesSchedule(kKeyObfuscationKey, round_keys);
  std::memcpy(block, g.host(in), 16);
  AesEncrypt(round_keys, block, g.host(out));
  return 0;
}

uint32_t XeKeysAesCbcUsingKey(Guest g, uint32_t obscured_key, uint32_t in, uint32_t size, uint32_t out, uint32_t feed,
                              uint32_t encrypt) {
  uint8_t key[16], round_keys[176];
  Deobscure(g, obscured_key, key);
  AesSchedule(key, round_keys);
  AesCbc(g, round_keys, in, size, out, feed, encrypt);
  return 0;
}

uint32_t XeKeysHmacShaUsingKey(Guest g, uint32_t obscured_key, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                               uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size) {
  if (!obscured_key) return 0xC000000D;
  uint8_t key[16];
  Deobscure(g, obscured_key, key);
  const auto digest = HmacSha(g, key, 16, in1, size1, in2, size2, in3, size3);
  std::memcpy(g.host(out), digest.data(), std::min<uint32_t>(20, out_size));
  return 0;
}

uint32_t XeCryptBnDwLePkcs1Verify() { return 1; }

}  // namespace any360::sys
