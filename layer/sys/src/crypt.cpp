#include "any360/sys/crypt.h"

#include "crypt_blocks.h"
#include "crypt_host.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

namespace any360::sys {

namespace {

// The one-call SHA forms on the host's library when the SHA path is kHost (crypt.h): from 64 bytes of input (below,
// the portable code's single block costs less than the host's call), and below 512 MB for SHA-1 (whose length field
// the reference writes as 32 bits). False when the portable code is to run.
bool HostDigest(crypt_host::Hash hash, bool hmac, const uint8_t* key, uint32_t key_size, const crypt_host::Part (&parts)[3],
                uint8_t* out) {
  if (CurrentShaPath() != ShaPath::kHost) return false;
  const uint64_t total = uint64_t(parts[0].size) + parts[1].size + parts[2].size;
  if (total < 64 || (hash == crypt_host::Hash::kSha1 && total + 64 >= (uint64_t(1) << 29))) return false;
  return crypt_host::Digest(hash, hmac, key, key_size, parts, 3, out);
}

// Up to three guest buffers, null or empty ones skipped.
void GuestParts(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3,
                crypt_host::Part (&parts)[3]) {
  const uint32_t at[3] = {in1, in2, in3}, size[3] = {size1, size2, size3};
  for (int i = 0; i < 3; ++i) parts[i] = at[i] && size[i] ? crypt_host::Part{g.host(at[i]), size[i]} : crypt_host::Part{nullptr, 0};
}

}  // namespace

Sha1::Sha1() : h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::Block(const uint8_t* block) { crypt_blocks::Sha1(h_, block, 1); }

void Sha1::Update(const uint8_t* data, size_t size) {
  total_ += size;
  if (buffered_) {
    const size_t n = std::min(size, sizeof buffer_ - buffered_);
    std::memcpy(buffer_ + buffered_, data, n);
    buffered_ += n, data += n, size -= n;
    if (buffered_ < sizeof buffer_) return;
    Block(buffer_);
    buffered_ = 0;
  }
  // Whole blocks straight from the input, the rest buffered.
  crypt_blocks::Sha1(h_, data, size / 64);
  data += size & ~size_t(63), size &= 63;
  std::memcpy(buffer_, data, size);
  buffered_ = size;
}

std::array<uint8_t, 20> Sha1::Final() {
  // Only the low 32 bits of the bit count, as the reference's SHA-1 writes it (the same below 512 MB).
  const uint32_t bits = uint32_t(total_ * 8);
  uint8_t blocks[128] = {};
  std::memcpy(blocks, buffer_, buffered_);
  blocks[buffered_] = 0x80;
  const size_t length = buffered_ < 56 ? 64 : 128;
  for (int i = 0; i < 4; ++i) blocks[length - 4 + i] = uint8_t(bits >> (24 - 8 * i));
  crypt_blocks::Sha1(h_, blocks, length / 64);
  total_ += length - buffered_;
  buffered_ = 0;
  std::array<uint8_t, 20> digest;
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 4; ++j) digest[4 * i + j] = uint8_t(h_[i] >> (24 - 8 * j));
  }
  return digest;
}

void XeCryptSha(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3, uint32_t output,
                uint32_t output_size) {
  crypt_host::Part parts[3];
  GuestParts(g, in1, size1, in2, size2, in3, size3, parts);
  std::array<uint8_t, 20> digest;
  if (!HostDigest(crypt_host::Hash::kSha1, false, nullptr, 0, parts, digest.data())) {
    Sha1 sha;
    for (const auto& part : parts) {
      if (part.size) sha.Update(part.data, part.size);
    }
    digest = sha.Final();
  }
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

void Sha256Block(uint32_t h[8], const uint8_t* block) { crypt_blocks::Sha256(h, block, 1); }

// The digest of an HMAC-SHA-1 with a host key.
std::array<uint8_t, 20> HmacSha(Guest g, const uint8_t* key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2,
                                uint32_t size2, uint32_t in3, uint32_t size3) {
  const crypt_host::Part parts[3] = {{size1 ? g.host(in1) : nullptr, size1}, {size2 ? g.host(in2) : nullptr, size2},
                                     {size3 ? g.host(in3) : nullptr, size3}};
  std::array<uint8_t, 20> host;
  if (HostDigest(crypt_host::Hash::kSha1, true, key, key_size, parts, host.data())) return host;
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

constexpr uint64_t Permute(uint64_t value, int width, const uint8_t* table, int count) {
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

// The S-boxes and P folded together: kDesSp[i][six] is P of S-box i's output for the six bits `six`, in place.
constexpr auto kDesSp = [] {
  std::array<std::array<uint32_t, 64>, 8> sp{};
  for (int i = 0; i < 8; ++i) {
    for (uint32_t six = 0; six < 64; ++six) {
      const uint32_t row = (six >> 4 & 2) | (six & 1), column = six >> 1 & 0xF;
      sp[i][six] = uint32_t(Permute(uint64_t(kDesS[64 * i + 16 * row + column] & 0xF) << (28 - 4 * i), 32, kDesP, 32));
    }
  }
  return sp;
}();

// A round: E's six-bit groups of R are R's bits 4i-1 .. 4i+4 (bit 1 the most significant, 0 standing for 32), so
// rotl(R, 1) holds groups 7, 5, 3, 1 and rotr(R, 3) groups 6, 4, 2, 0 in the low six bits of each byte; the 48-bit
// subkey (group 0 in its bits 47..42) is spread the same way. The subkey's part does not wait on R, so it runs beside
// the round chain: a round is the two XORs, eight table loads and their OR on that chain.
inline uint32_t DesSpread(uint64_t k) {
  return uint32_t(k & 0x3F) | uint32_t(k >> 4 & 0x3F00) | uint32_t(k >> 8 & 0x3F0000) | uint32_t(k >> 12 & 0x3F000000);
}

inline uint32_t DesRound(uint32_t r, uint64_t k) {
  const uint32_t x = std::rotl(r, 1) ^ DesSpread(k), y = std::rotr(r, 3) ^ DesSpread(k >> 6);
  return kDesSp[7][x & 0x3F] | kDesSp[5][x >> 8 & 0x3F] | kDesSp[3][x >> 16 & 0x3F] | kDesSp[1][x >> 24 & 0x3F] |
         kDesSp[6][y & 0x3F] | kDesSp[4][y >> 8 & 0x3F] | kDesSp[2][y >> 16 & 0x3F] | kDesSp[0][y >> 24 & 0x3F];
}

// One schedule as the reference stores it: sixteen host (little-endian) 64-bit subkeys.
struct DesKeys {
  uint64_t sub_keys[16];
};

// One swap of the bits of `b` selected by `mask` with the bits of `a` `n` places higher (IP and FP as five of them).
inline void DesSwap(uint32_t& a, uint32_t& b, int n, uint32_t mask) {
  const uint32_t t = ((a >> n) ^ b) & mask;
  b ^= t, a ^= t << n;
}

// IP on the block's halves (l the high word), and FP, its inverse: IP's steps undone in reverse order.
inline void DesIp(uint32_t& l, uint32_t& r) {
  DesSwap(l, r, 4, 0x0F0F0F0F), DesSwap(l, r, 16, 0x0000FFFF), DesSwap(r, l, 2, 0x33333333);
  DesSwap(r, l, 8, 0x00FF00FF), DesSwap(l, r, 1, 0x55555555);
}

inline void DesFp(uint32_t& l, uint32_t& r) {
  DesSwap(l, r, 1, 0x55555555), DesSwap(r, l, 8, 0x00FF00FF), DesSwap(r, l, 2, 0x33333333);
  DesSwap(l, r, 16, 0x0000FFFF), DesSwap(l, r, 4, 0x0F0F0F0F);
}

// The sixteen rounds between IP and FP, leaving (R16, L16) in (l, r) as FP takes them. Triple DES chains three of
// them with no FP / IP in between (FP then IP is the identity).
template <bool kDecrypt>
inline void DesRounds(uint32_t& l, uint32_t& r, const DesKeys& keys) {
  for (int i = 0; i < 16; i += 2) {
    const int k0 = kDecrypt ? 15 - i : i, k1 = kDecrypt ? 14 - i : i + 1;
    l ^= DesRound(r, keys.sub_keys[k0]);
    r ^= DesRound(l, keys.sub_keys[k1]);
  }
  std::swap(l, r);
}

uint64_t Des(uint64_t block, const DesKeys& keys, bool decrypt) {
  uint32_t l = uint32_t(block >> 32), r = uint32_t(block);
  DesIp(l, r);
  decrypt ? DesRounds<true>(l, r, keys) : DesRounds<false>(l, r, keys);
  DesFp(l, r);
  return uint64_t(l) << 32 | r;
}

struct Des3 {
  DesKeys keys[3];
  // The reference keeps each subkey as a host (little-endian) 64-bit word.
  Des3(Guest g, uint32_t state) : keys{Load(g, state), Load(g, state + 0x80), Load(g, state + 0x100)} {}
  static DesKeys Load(Guest g, uint32_t schedule) {
    DesKeys keys;
    std::memcpy(keys.sub_keys, g.host(schedule), sizeof keys.sub_keys);
    if constexpr (std::endian::native == std::endian::big) {
      for (uint64_t& k : keys.sub_keys) k = std::byteswap(k);
    }
    return keys;
  }
  uint64_t Encrypt(uint64_t b) const {
    uint32_t l = uint32_t(b >> 32), r = uint32_t(b);
    DesIp(l, r);
    DesRounds<false>(l, r, keys[0]), DesRounds<true>(l, r, keys[1]), DesRounds<false>(l, r, keys[2]);
    DesFp(l, r);
    return uint64_t(l) << 32 | r;
  }
  uint64_t Decrypt(uint64_t b) const {
    uint32_t l = uint32_t(b >> 32), r = uint32_t(b);
    DesIp(l, r);
    DesRounds<true>(l, r, keys[2]), DesRounds<false>(l, r, keys[1]), DesRounds<true>(l, r, keys[0]);
    DesFp(l, r);
    return uint64_t(l) << 32 | r;
  }
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
    const uint32_t n = std::min<uint32_t>(64 - buffered, size);
    std::memcpy(buffer + buffered, data, n);
    buffered += n, data += n, size -= n;
  }
  if (buffered == 64) {
    Sha256Block(h, buffer);
    done += 64;
    buffered = 0;
  }
  crypt_blocks::Sha256(h, data, size / 64);
  done += size & ~63u, data += size & ~63u, size &= 63;
  std::memcpy(buffer + buffered, data, size);
  buffered += size;
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
  crypt_blocks::Sha256(h, blocks, length / 64);
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

// --- RSA's public operation ---

namespace {

// Unsigned numbers as 32-bit limbs, least significant first.
using Limbs = std::vector<uint32_t>;

int Compare(const Limbs& a, const Limbs& b) {
  for (size_t i = std::max(a.size(), b.size()); i-- > 0;) {
    const uint32_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

void SubtractInPlace(Limbs& a, const Limbs& b) {
  int64_t borrow = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const int64_t d = int64_t(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
    a[i] = uint32_t(d);
    borrow = d < 0;
  }
}

// a * b mod n, the remainder taken bit by bit (shift in, subtract when not below n).
Limbs MulMod(const Limbs& a, const Limbs& b, const Limbs& n) {
  Limbs product(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size(); ++j) {
      const uint64_t t = uint64_t(a[i]) * b[j] + product[i + j] + carry;
      product[i + j] = uint32_t(t);
      carry = t >> 32;
    }
    product[i + b.size()] += uint32_t(carry);
  }
  Limbs r(n.size() + 1, 0);
  for (size_t bit = product.size() * 32; bit-- > 0;) {
    uint32_t in = (product[bit / 32] >> (bit % 32)) & 1;
    for (uint32_t& limb : r) {
      const uint32_t out = limb >> 31;
      limb = limb << 1 | in;
      in = out;
    }
    if (Compare(r, n) >= 0) SubtractInPlace(r, n);
  }
  r.resize(n.size());
  return r;
}

Limbs LoadNumber(Guest g, uint32_t at, uint32_t words) {
  Limbs v(size_t(words) * 2);
  for (uint32_t i = 0; i < words; ++i) {
    const uint64_t w = g.Load64(at + 8 * i);
    v[2 * i] = uint32_t(w);
    v[2 * i + 1] = uint32_t(w >> 32);
  }
  return v;
}

}  // namespace

uint32_t XeCryptBnQwNeRsaPubCrypt(Guest g, uint32_t in, uint32_t out, uint32_t key) {
  const uint32_t words = g.Load32(key), exponent = g.Load32(key + 4);
  if (!words) return 0;
  const Limbs n = LoadNumber(g, key + 16, words), base = LoadNumber(g, in, words);
  if (Compare(base, n) >= 0) return 0;
  Limbs result(n.size(), 0);
  result[0] = 1;
  for (int bit = 31; bit >= 0; --bit) {
    result = MulMod(result, result, n);
    if ((exponent >> bit) & 1) result = MulMod(result, base, n);
  }
  for (uint32_t i = 0; i < words; ++i) g.Store64(out + 8 * i, uint64_t(result[2 * i + 1]) << 32 | result[2 * i]);
  return 1;
}

// --- step 12: MD5, HMAC-MD5, streamed HMAC-SHA-1, one-call SHA-256, single DES (source: Xenia Canary's
// xboxkrnl_crypt.cc for the states and answers; RFC 1321, RFC 2202, FIPS 180-4 and FIPS 81 for the results) ---

namespace {

constexpr uint32_t kMd5Count = 0, kMd5Words = 4, kMd5Buffer = 0x14, kMd5StateSize = 0x54;
constexpr uint32_t kHmacOuter = kShaStateSize;

Md5 LoadMd5(Guest g, uint32_t state) {
  uint32_t abcd[4];
  for (int i = 0; i < 4; ++i) abcd[i] = g.Load32(state + kMd5Words + 4 * i);
  return Md5(abcd, g.host(state + kMd5Buffer), g.Load32(state + kMd5Count));
}

// An HMAC's two pads for a key cut to its first 64 bytes.
void HmacPads(const uint8_t* key, uint32_t key_size, uint8_t inner[64], uint8_t outer[64]) {
  std::memset(inner, 0x36, 64);
  std::memset(outer, 0x5C, 64);
  for (uint32_t i = 0; i < std::min<uint32_t>(key_size, 64); ++i) inner[i] ^= key[i], outer[i] ^= key[i];
}

// The hash of up to three guest buffers, null or empty ones skipped.
template <typename Hash>
auto HashOf(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3) {
  Hash hash;
  if (in1 && size1) hash.Update(g.host(in1), size1);
  if (in2 && size2) hash.Update(g.host(in2), size2);
  if (in3 && size3) hash.Update(g.host(in3), size3);
  return hash.Final();
}

class Sha256 {
 public:
  void Update(const uint8_t* data, size_t size) {
    total_ += size;
    if (buffered_) {
      const size_t n = std::min<size_t>(64 - buffered_, size);
      std::memcpy(buffer_ + buffered_, data, n);
      buffered_ += n, data += n, size -= n;
      if (buffered_ < 64) return;
      crypt_blocks::Sha256(h_, buffer_, 1);
      buffered_ = 0;
    }
    crypt_blocks::Sha256(h_, data, size / 64);
    data += size & ~size_t(63), size &= 63;
    std::memcpy(buffer_, data, size);
    buffered_ = size;
  }
  std::array<uint8_t, 32> Final() {
    const uint64_t bits = total_ * 8;
    uint8_t blocks[128] = {};
    std::memcpy(blocks, buffer_, buffered_);
    blocks[buffered_] = 0x80;
    const size_t length = buffered_ < 56 ? 64 : 128;
    for (int i = 0; i < 8; ++i) blocks[length - 8 + i] = uint8_t(bits >> (56 - 8 * i));
    crypt_blocks::Sha256(h_, blocks, length / 64);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h_[i] >> (24 - 8 * j));
    }
    return out;
  }

 private:
  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// One DES schedule as the reference stores it (host little-endian 64-bit subkeys).
struct DesState {
  DesKeys keys;
  DesState(Guest g, uint32_t state) : keys(Des3::Load(g, state)) {}
};

}  // namespace

Md5::Md5() : abcd_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

Md5::Md5(const uint32_t abcd[4], const uint8_t* buffer, uint32_t count) : buffered_(count % 64), total_(count) {
  std::memcpy(abcd_, abcd, sizeof abcd_);
  std::memcpy(buffer_, buffer, buffered_);
}

void Md5::Block(const uint8_t* block) { crypt_blocks::Md5(abcd_, block, 1); }

void Md5::Update(const uint8_t* data, size_t size) {
  total_ += size;
  if (buffered_) {
    const size_t n = std::min<size_t>(64 - buffered_, size);
    std::memcpy(buffer_ + buffered_, data, n);
    buffered_ += n, data += n, size -= n;
    if (buffered_ < 64) return;
    Block(buffer_);
    buffered_ = 0;
  }
  crypt_blocks::Md5(abcd_, data, size / 64);
  data += size & ~size_t(63), size &= 63;
  std::memcpy(buffer_, data, size);
  buffered_ = size;
}

std::array<uint8_t, 16> Md5::Final() {
  const uint64_t bits = total_ * 8;
  uint8_t blocks[128] = {};
  std::memcpy(blocks, buffer_, buffered_);
  blocks[buffered_] = 0x80;
  const size_t length = buffered_ < 56 ? 64 : 128;
  for (int i = 0; i < 8; ++i) blocks[length - 8 + i] = uint8_t(bits >> (8 * i));
  crypt_blocks::Md5(abcd_, blocks, length / 64);
  total_ += length - buffered_;
  buffered_ = 0;
  std::array<uint8_t, 16> out;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(abcd_[i] >> (8 * j));
  }
  return out;
}

void XeCryptMd5(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3, uint32_t output,
                uint32_t output_size) {
  const auto digest = HashOf<Md5>(g, in1, size1, in2, size2, in3, size3);
  std::memcpy(g.host(output), digest.data(), std::min<uint32_t>(16, output_size));
}

void XeCryptSha256(Guest g, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2, uint32_t in3, uint32_t size3,
                   uint32_t output, uint32_t output_size) {
  crypt_host::Part parts[3];
  GuestParts(g, in1, size1, in2, size2, in3, size3, parts);
  std::array<uint8_t, 32> digest;
  if (!HostDigest(crypt_host::Hash::kSha256, false, nullptr, 0, parts, digest.data())) {
    digest = HashOf<Sha256>(g, in1, size1, in2, size2, in3, size3);
  }
  std::memcpy(g.host(output), digest.data(), std::min<uint32_t>(32, output_size));
}

void XeCryptMd5Init(Guest g, uint32_t state) {
  std::memset(g.host(state), 0, kMd5StateSize);
  const Md5 md5;
  for (int i = 0; i < 4; ++i) g.Store32(state + kMd5Words + 4 * i, md5.words()[i]);
}

void XeCryptMd5Update(Guest g, uint32_t state, uint32_t input, uint32_t size) {
  Md5 md5 = LoadMd5(g, state);
  if (size) md5.Update(g.host(input), size);
  for (int i = 0; i < 4; ++i) g.Store32(state + kMd5Words + 4 * i, md5.words()[i]);
  g.Store32(state + kMd5Count, uint32_t(md5.total()));
  std::memcpy(g.host(state + kMd5Buffer), md5.buffer(), md5.total() % 64);
}

void XeCryptMd5Final(Guest g, uint32_t state, uint32_t out, uint32_t out_size) {
  Md5 md5 = LoadMd5(g, state);
  const auto digest = md5.Final();
  std::memcpy(g.host(out), digest.data(), std::min<uint32_t>(16, out_size));
  std::memcpy(g.host(state + kMd5Words), digest.data(), 16);  // the digest left in the state's words, as Xenia
}

void XeCryptHmacMd5(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2, uint32_t size2,
                    uint32_t in3, uint32_t size3, uint32_t out, uint32_t out_size) {
  uint8_t inner[64], outer[64];
  HmacPads(key_size ? g.host(key) : nullptr, key_size, inner, outer);
  Md5 first;
  first.Update(inner, 64);
  if (in1 && size1) first.Update(g.host(in1), size1);
  if (in2 && size2) first.Update(g.host(in2), size2);
  if (in3 && size3) first.Update(g.host(in3), size3);
  const auto digest = first.Final();
  Md5 second;
  second.Update(outer, 64);
  second.Update(digest.data(), 16);
  const auto result = second.Final();
  std::memcpy(g.host(out), result.data(), std::min<uint32_t>(16, out_size));
}

void XeCryptHmacShaInit(Guest g, uint32_t state, uint32_t key, uint32_t key_size) {
  uint8_t inner[64], outer[64];
  HmacPads(key_size ? g.host(key) : nullptr, key_size, inner, outer);
  std::memset(g.host(state), 0, 2 * kShaStateSize);
  // Each half is an XECRYPT_SHA_STATE that has taken its 64-byte pad (one whole block: nothing buffered).
  const std::pair<uint32_t, const uint8_t*> halves[2] = {{state, inner}, {state + kHmacOuter, outer}};
  for (const auto& [at, pad] : halves) {
    Sha1 sha;
    sha.Update(pad, 64);
    StoreDigest(g, at, sha);
    g.Store32(at + kShaCount, 64);
  }
}

void XeCryptHmacShaUpdate(Guest g, uint32_t state, uint32_t input, uint32_t size) { XeCryptShaUpdate(g, state, input, size); }

void XeCryptHmacShaFinal(Guest g, uint32_t state, uint32_t out, uint32_t out_size) {
  Sha1 inner = LoadSha1(g, state);
  const auto digest = inner.Final();
  StoreDigest(g, state, inner);
  Sha1 outer = LoadSha1(g, state + kHmacOuter);
  outer.Update(digest.data(), 20);
  const auto result = outer.Final();
  StoreDigest(g, state + kHmacOuter, outer);
  std::memcpy(g.host(out), result.data(), std::min<uint32_t>(20, out_size));
}

uint32_t XeCryptHmacShaVerify(Guest g, uint32_t key, uint32_t key_size, uint32_t in1, uint32_t size1, uint32_t in2,
                              uint32_t size2, uint32_t in3, uint32_t size3, uint32_t verify, uint32_t verify_size) {
  const auto digest = HmacSha(g, key_size ? g.host(key) : nullptr, key_size, in1, size1, in2, size2, in3, size3);
  const uint32_t n = std::min<uint32_t>(20, verify_size);
  return n == 0 || std::memcmp(digest.data(), g.host(verify), n) == 0 ? 1 : 0;
}

void XeCryptDesKey(Guest g, uint32_t state, uint32_t key) {
  uint64_t sub_keys[16];
  DesSchedule(g.Load64(key), sub_keys);
  for (int k = 0; k < 16; ++k) {
    uint64_t little = sub_keys[k];
    if constexpr (std::endian::native == std::endian::big) little = std::byteswap(little);
    std::memcpy(g.host(state + 8 * k), &little, 8);
  }
}

void XeCryptDesEcb(Guest g, uint32_t state, uint32_t in, uint32_t out, uint32_t encrypt) {
  const DesState des(g, state);
  g.Store64(out, Des(g.Load64(in), des.keys, !encrypt));
}

void XeCryptDesCbc(Guest g, uint32_t state, uint32_t in, uint32_t size, uint32_t out, uint32_t feed, uint32_t encrypt) {
  const DesState des(g, state);
  uint64_t last = g.Load64(feed);
  for (uint32_t i = 0; i < size / 8; ++i) {
    const uint64_t block = g.Load64(in + 8 * i);
    if (encrypt) {
      last = Des(block ^ last, des.keys, false);
      g.Store64(out + 8 * i, last);
    } else {
      g.Store64(out + 8 * i, Des(block, des.keys, true) ^ last);
      last = block;
    }
  }
  g.Store64(feed, last);
}

}  // namespace any360::sys
