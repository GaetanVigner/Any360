#include "crypt_blocks.h"

#include <atomic>
#include <bit>
#include <cstring>

#include "any360/sys/crypt.h"
#include "crypt_host.h"

#if defined(__x86_64__) || defined(_M_X64)
#define ANY360_SHA_NI 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__clang__) || defined(__GNUC__)
#define ANY360_SHA_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#else
#define ANY360_SHA_TARGET
#endif
#endif

#if defined(__clang__)
#define ANY360_UNROLL _Pragma("clang loop unroll(full)")
#elif defined(__GNUC__)
#define ANY360_UNROLL _Pragma("GCC unroll 64")
#else
#define ANY360_UNROLL
#endif

namespace any360::sys {
namespace {

uint32_t LoadBe32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::endian::native == std::endian::little ? std::byteswap(v) : v;
}

uint32_t LoadLe32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::endian::native == std::endian::big ? std::byteswap(v) : v;
}

uint64_t LoadBe64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return std::endian::native == std::endian::little ? std::byteswap(v) : v;
}

constexpr uint64_t kSha512K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
    0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
    0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

#if ANY360_SHA_NI

bool ProcessorHasSha() {
  int leaf1[4] = {}, leaf7[4] = {};
#if defined(_MSC_VER)
  __cpuidex(leaf1, 0, 0);
  if (leaf1[0] < 7) return false;
  __cpuidex(leaf1, 1, 0);
  __cpuidex(leaf7, 7, 0);
#else
  unsigned a, b, c, d;
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid_count(1, 0, a, b, c, d);
  leaf1[2] = int(c);
  __cpuid_count(7, 0, a, b, c, d);
  leaf7[1] = int(b);
#endif
  const bool ssse3 = leaf1[2] & (1 << 9), sse41 = leaf1[2] & (1 << 19), sha = leaf7[1] & (1 << 29);
  return ssse3 && sse41 && sha;
}

// Four rounds of SHA-256 (group I of 16) and the message schedule they feed (Intel's SHA extensions guide).
template <int I>
ANY360_SHA_TARGET inline void Sha256Quad(__m128i& state0, __m128i& state1, __m128i (&w)[4]) {
  __m128i& cur = w[I % 4];
  __m128i msg = _mm_add_epi32(cur, _mm_loadu_si128(reinterpret_cast<const __m128i*>(kSha256K + 4 * I)));
  state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
  if constexpr (I >= 3 && I <= 14) {
    __m128i& next = w[(I + 1) % 4];
    next = _mm_sha256msg2_epu32(_mm_add_epi32(next, _mm_alignr_epi8(cur, w[(I + 3) % 4], 4)), cur);
  }
  msg = _mm_shuffle_epi32(msg, 0x0E);
  state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  if constexpr (I >= 1 && I <= 12) w[(I + 3) % 4] = _mm_sha256msg1_epu32(w[(I + 3) % 4], cur);
}

ANY360_SHA_TARGET void Sha256Ni(uint32_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
  __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h)), 0xB1);  // CDAB
  __m128i state1 = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h + 4)), 0x1B);  // EFGH
  __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
  state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH
  for (; blocks; --blocks, data += 64) {
    const __m128i abef = state0, cdgh = state1;
    __m128i w[4];
    for (int i = 0; i < 4; ++i) w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
    Sha256Quad<0>(state0, state1, w), Sha256Quad<1>(state0, state1, w), Sha256Quad<2>(state0, state1, w);
    Sha256Quad<3>(state0, state1, w), Sha256Quad<4>(state0, state1, w), Sha256Quad<5>(state0, state1, w);
    Sha256Quad<6>(state0, state1, w), Sha256Quad<7>(state0, state1, w), Sha256Quad<8>(state0, state1, w);
    Sha256Quad<9>(state0, state1, w), Sha256Quad<10>(state0, state1, w), Sha256Quad<11>(state0, state1, w);
    Sha256Quad<12>(state0, state1, w), Sha256Quad<13>(state0, state1, w), Sha256Quad<14>(state0, state1, w);
    Sha256Quad<15>(state0, state1, w);
    state0 = _mm_add_epi32(state0, abef);
    state1 = _mm_add_epi32(state1, cdgh);
  }
  tmp = _mm_shuffle_epi32(state0, 0x1B);     // FEBA
  state1 = _mm_shuffle_epi32(state1, 0xB1);  // DCHG
  _mm_storeu_si128(reinterpret_cast<__m128i*>(h), _mm_blend_epi16(tmp, state1, 0xF0));    // DCBA
  _mm_storeu_si128(reinterpret_cast<__m128i*>(h + 4), _mm_alignr_epi8(state1, tmp, 8));  // HGFE
}

// Four rounds of SHA-1 (group I of 20) and the message schedule they feed.
template <int I>
ANY360_SHA_TARGET inline void Sha1Quad(__m128i& abcd, __m128i (&e)[2], __m128i (&m)[4]) {
  __m128i& cur = m[I % 4];
  __m128i& e_in = e[I % 2];
  if constexpr (I == 0) {
    e_in = _mm_add_epi32(e_in, cur);
  } else {
    e_in = _mm_sha1nexte_epu32(e_in, cur);
  }
  e[(I + 1) % 2] = abcd;
  if constexpr (I >= 3 && I <= 18) m[(I + 1) % 4] = _mm_sha1msg2_epu32(m[(I + 1) % 4], cur);
  abcd = _mm_sha1rnds4_epu32(abcd, e_in, I / 5);
  if constexpr (I >= 1 && I <= 16) m[(I + 3) % 4] = _mm_sha1msg1_epu32(m[(I + 3) % 4], cur);
  if constexpr (I >= 2 && I <= 17) m[(I + 2) % 4] = _mm_xor_si128(m[(I + 2) % 4], cur);
}

ANY360_SHA_TARGET void Sha1Ni(uint32_t h[5], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi64x(0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);
  __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(h)), 0x1B);
  __m128i e0 = _mm_set_epi32(int(h[4]), 0, 0, 0);
  for (; blocks; --blocks, data += 64) {
    const __m128i abcd_save = abcd, e0_save = e0;
    __m128i m[4], e[2] = {e0, e0};
    for (int i = 0; i < 4; ++i) m[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
    Sha1Quad<0>(abcd, e, m), Sha1Quad<1>(abcd, e, m), Sha1Quad<2>(abcd, e, m), Sha1Quad<3>(abcd, e, m);
    Sha1Quad<4>(abcd, e, m), Sha1Quad<5>(abcd, e, m), Sha1Quad<6>(abcd, e, m), Sha1Quad<7>(abcd, e, m);
    Sha1Quad<8>(abcd, e, m), Sha1Quad<9>(abcd, e, m), Sha1Quad<10>(abcd, e, m), Sha1Quad<11>(abcd, e, m);
    Sha1Quad<12>(abcd, e, m), Sha1Quad<13>(abcd, e, m), Sha1Quad<14>(abcd, e, m), Sha1Quad<15>(abcd, e, m);
    Sha1Quad<16>(abcd, e, m), Sha1Quad<17>(abcd, e, m), Sha1Quad<18>(abcd, e, m), Sha1Quad<19>(abcd, e, m);
    e0 = _mm_sha1nexte_epu32(e[0], e0_save);
    abcd = _mm_add_epi32(abcd, abcd_save);
  }
  _mm_storeu_si128(reinterpret_cast<__m128i*>(h), _mm_shuffle_epi32(abcd, 0x1B));
  h[4] = uint32_t(_mm_extract_epi32(e0, 3));
}

#else
bool ProcessorHasSha() { return false; }
#endif

const bool kProcessorHasSha = ProcessorHasSha();

ShaPath FirstAvailable() {
  if (kProcessorHasSha) return ShaPath::kProcessor;
  return crypt_host::Available() ? ShaPath::kHost : ShaPath::kPortable;
}

std::atomic<ShaPath> g_path{FirstAvailable()};

}  // namespace

bool ShaPathAvailable(ShaPath path) {
  switch (path) {
    case ShaPath::kProcessor: return kProcessorHasSha;
    case ShaPath::kHost: return crypt_host::Available();
    case ShaPath::kPortable: return true;
  }
  return false;
}
ShaPath DefaultShaPath() { return FirstAvailable(); }
ShaPath CurrentShaPath() { return g_path.load(std::memory_order_relaxed); }
void SetShaPath(ShaPath path) { g_path.store(ShaPathAvailable(path) ? path : FirstAvailable(), std::memory_order_relaxed); }

namespace crypt_blocks {

void Sha1Portable(uint32_t h[5], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = LoadBe32(data + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    auto step = [&](uint32_t f, uint32_t k, uint32_t wi) {
      const uint32_t t = std::rotl(a, 5) + f + e + k + wi;
      e = d, d = c, c = std::rotl(b, 30), b = a, a = t;
    };
    ANY360_UNROLL
    for (int i = 0; i < 20; ++i) step(d ^ (b & (c ^ d)), 0x5A827999, w[i]);
    ANY360_UNROLL
    for (int i = 20; i < 40; ++i) step(b ^ c ^ d, 0x6ED9EBA1, w[i]);
    ANY360_UNROLL
    for (int i = 40; i < 60; ++i) step((b & c) | (d & (b | c)), 0x8F1BBCDC, w[i]);
    ANY360_UNROLL
    for (int i = 60; i < 80; ++i) step(b ^ c ^ d, 0xCA62C1D6, w[i]);
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
}

void Sha256Portable(uint32_t h[8], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = LoadBe32(data + 4 * i);
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    ANY360_UNROLL
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + (g ^ (e & (f ^ g))) + kSha256K[i] + w[i];
      const uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) | (c & (a | b)));
      hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
}

namespace {

// SHA-512's 80 rounds over the schedule with its constants added (`wk`). Maj(a, b, c) = b ^ ((a ^ b) & (b ^ c)), and
// this round's a ^ b is the next one's b ^ c: one operation less a round.
template <int>
inline __attribute__((always_inline)) void Sha512Rounds(uint64_t h[8], const uint64_t* wk) {
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  uint64_t bc = b ^ c;
  ANY360_UNROLL
  for (int i = 0; i < 80; ++i) {
    const uint64_t t1 = hh + (std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41)) + (g ^ (e & (f ^ g))) + wk[i];
    const uint64_t ab = a ^ b;
    const uint64_t t2 = (std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39)) + (b ^ (ab & bc));
    bc = ab;
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

#if ANY360_SHA_NI

// x86-64 with BMI2 (and SSSE3): every Intel from Haswell, every AMD from Excavator (Zen 2 handhelds included).
bool ProcessorHasBmi2() {
  int leaf0[4] = {}, leaf1[4] = {}, leaf7[4] = {};
#if defined(_MSC_VER)
  __cpuidex(leaf0, 0, 0);
  if (leaf0[0] < 7) return false;
  __cpuidex(leaf1, 1, 0);
  __cpuidex(leaf7, 7, 0);
#else
  unsigned a, b, c, d;
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid_count(1, 0, a, b, c, d);
  leaf1[2] = int(c);
  __cpuid_count(7, 0, a, b, c, d);
  leaf7[1] = int(b);
#endif
  return (leaf1[2] & (1 << 9)) && (leaf7[1] & (1 << 8));
}

// The schedule two words at a time in eight SSE registers (w[i - 15] and w[i - 7] straddle two of them), the rounds
// with BMI2's rorx: 1.5-1.6x the portable code, 1.2-1.4x CNG's SHA-512 on a Zen 4 (docs/perf/archive/
// 2026-11-19-sys-step12-crypt).
__attribute__((target("bmi2,ssse3"))) void Sha512Bmi2(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  auto rotr = [](__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); };
  auto k = [](int i) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(kSha512K + i)); };
  for (; blocks; --blocks, data += 128) {
    alignas(16) uint64_t wk[80];
    __m128i x[8];
    for (int i = 0; i < 8; ++i) {
      x[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i], k(2 * i)));
    }
    ANY360_UNROLL
    for (int i = 8; i < 40; ++i) {
      const __m128i w15 = _mm_alignr_epi8(x[(i - 7) & 7], x[(i - 8) & 7], 8);
      const __m128i w7 = _mm_alignr_epi8(x[(i - 3) & 7], x[(i - 4) & 7], 8);
      const __m128i w2 = x[(i - 1) & 7];
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      x[i & 7] = _mm_add_epi64(_mm_add_epi64(x[(i - 8) & 7], s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i & 7], k(2 * i)));
    }
    Sha512Rounds<1>(h, wk);
  }
}

const bool g_sha512_bmi2 = ProcessorHasBmi2();

#endif

}  // namespace

void Sha512Portable(uint64_t h[8], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 128) {
    uint64_t w[80], wk[80];
    for (int i = 0; i < 16; ++i) w[i] = LoadBe64(data + 8 * i), wk[i] = w[i] + kSha512K[i];
    for (int i = 16; i < 80; ++i) {
      const uint64_t s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const uint64_t s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      wk[i] = w[i] + kSha512K[i];
    }
    Sha512Rounds<0>(h, wk);
  }
}

void Sha512(uint64_t h[8], const uint8_t* data, size_t blocks) {
#if ANY360_SHA_NI
  // ShaPath::kPortable (crypt.h) forces the portable code here too.
  if (g_sha512_bmi2 && g_path.load(std::memory_order_relaxed) != ShaPath::kPortable) return Sha512Bmi2(h, data, blocks);
#endif
  Sha512Portable(h, data, blocks);
}

void Sha1(uint32_t h[5], const uint8_t* data, size_t blocks) {
#if ANY360_SHA_NI
  if (g_path.load(std::memory_order_relaxed) == ShaPath::kProcessor) return Sha1Ni(h, data, blocks);
#endif
  Sha1Portable(h, data, blocks);
}

void Sha256(uint32_t h[8], const uint8_t* data, size_t blocks) {
#if ANY360_SHA_NI
  if (g_path.load(std::memory_order_relaxed) == ShaPath::kProcessor) return Sha256Ni(h, data, blocks);
#endif
  Sha256Portable(h, data, blocks);
}

void Md5(uint32_t abcd[4], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 64) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = LoadLe32(data + 4 * i);
    uint32_t a = abcd[0], b = abcd[1], c = abcd[2], d = abcd[3];
    // One step: the rotation of the sum into b, the words moved down (a <- d, d <- c, c <- b). b is the word the
    // previous step just made, so everything that does not need it is summed first (`early`), then the part of the
    // round function that does (`late`): the chain from one step to the next is as short as the function allows.
    auto step = [&](uint32_t early, uint32_t late, int s) {
      const uint32_t t = d;
      d = c, c = b;
      b = b + std::rotl(early + late, s);
      a = t;
    };
    static constexpr int kShift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    // Fully unrolled, so every rotation count is a constant.
    // F = (b & c) | (~b & d) = d ^ (b & (c ^ d)).
    ANY360_UNROLL
    for (int i = 0; i < 16; ++i) step(a + kMd5K[i] + m[i], d ^ (b & (c ^ d)), kShift[0][i % 4]);
    // G = (b & d) | (c & ~d) = (b & d) + (c & ~d): the two halves never share a bit.
    ANY360_UNROLL
    for (int i = 16; i < 32; ++i) step(a + kMd5K[i] + m[(5 * i + 1) % 16] + (c & ~d), b & d, kShift[1][i % 4]);
    // H = b ^ c ^ d.
    ANY360_UNROLL
    for (int i = 32; i < 48; ++i) step(a + kMd5K[i] + m[(3 * i + 5) % 16], b ^ (c ^ d), kShift[2][i % 4]);
    // I = c ^ (b | ~d).
    ANY360_UNROLL
    for (int i = 48; i < 64; ++i) step(a + kMd5K[i] + m[(7 * i) % 16], c ^ (b | ~d), kShift[3][i % 4]);
    abcd[0] += a, abcd[1] += b, abcd[2] += c, abcd[3] += d;
  }
}

}  // namespace crypt_blocks
}  // namespace any360::sys
