// SHA-512 block variants against CNG (scratch lab).
#include <windows.h>
#include <bcrypt.h>
#include <immintrin.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

constexpr uint64_t K[80] = {
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

static inline uint64_t Be64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return std::byteswap(v);
}

#define UNROLL _Pragma("clang loop unroll(full)")

// V0: the current code.
void V0(uint64_t h[8], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 128) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = Be64(data + 8 * i);
    for (int i = 16; i < 80; ++i) {
      const uint64_t s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const uint64_t s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    UNROLL
    for (int i = 0; i < 80; ++i) {
      const uint64_t t1 = hh + (std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41)) + (g ^ (e & (f ^ g))) + K[i] + w[i];
      const uint64_t t2 = (std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39)) + ((a & b) | (c & (a | b)));
      hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
}

// The rounds with the Maj reuse and a 16-word rolling schedule, written as a template for two targets.
template <int>
inline __attribute__((always_inline)) void Rounds(uint64_t h[8], const uint64_t* wk) {
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  uint64_t bc = b ^ c;
  UNROLL
  for (int i = 0; i < 80; ++i) {
    const uint64_t t1 = hh + (std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41)) + (g ^ (e & (f ^ g))) + wk[i];
    const uint64_t ab = a ^ b;
    const uint64_t t2 = (std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39)) + (b ^ (ab & bc));
    bc = ab;
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

// V1: scalar schedule into W+K, Maj reuse; generic x86-64.
void V1(uint64_t h[8], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 128) {
    uint64_t w[80], wk[80];
    for (int i = 0; i < 16; ++i) w[i] = Be64(data + 8 * i), wk[i] = w[i] + K[i];
    for (int i = 16; i < 80; ++i) {
      const uint64_t s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const uint64_t s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      wk[i] = w[i] + K[i];
    }
    Rounds<0>(h, wk);
  }
}

// V2: V1 compiled for BMI2 (rorx) and AVX2.
__attribute__((target("bmi2,avx2"))) void V2(uint64_t h[8], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 128) {
    uint64_t w[80], wk[80];
    for (int i = 0; i < 16; ++i) w[i] = Be64(data + 8 * i), wk[i] = w[i] + K[i];
    for (int i = 16; i < 80; ++i) {
      const uint64_t s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const uint64_t s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      wk[i] = w[i] + K[i];
    }
    Rounds<1>(h, wk);
  }
}

// V3: BMI2 rounds, the schedule two words at a time in SSE/AVX2 registers.
__attribute__((target("bmi2,avx2"))) void V3(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  auto rotr = [](__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); };
  for (; blocks; --blocks, data += 128) {
    alignas(32) uint64_t w[80], wk[80];
    for (int i = 0; i < 16; i += 2) {
      const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 8 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(w + i), v);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + i), _mm_add_epi64(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + i))));
    }
    for (int i = 16; i < 80; i += 2) {
      const __m128i w15 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + i - 15));
      const __m128i w2 = _mm_load_si128(reinterpret_cast<const __m128i*>(w + i - 2));
      const __m128i w16 = _mm_load_si128(reinterpret_cast<const __m128i*>(w + i - 16));
      const __m128i w7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + i - 7));
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      const __m128i v = _mm_add_epi64(_mm_add_epi64(w16, s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(w + i), v);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + i), _mm_add_epi64(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + i))));
    }
    Rounds<1>(h, wk);
  }
}

// V4: V3 without the separate w array: the schedule kept in 8 registers of two words.
__attribute__((target("bmi2,avx2"))) void V4(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  auto rotr = [](__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); };
  for (; blocks; --blocks, data += 128) {
    alignas(32) uint64_t wk[80];
    __m128i x[8];
    for (int i = 0; i < 8; ++i) {
      x[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    UNROLL
    for (int i = 8; i < 40; ++i) {
      // w[2i..2i+1] from x[i-8] (w-16), x[i-8..i-7] shifted by one (w-15), x[i-4..i-3] shifted (w-7), x[i-1] (w-2).
      const __m128i w15 = _mm_alignr_epi8(x[(i - 7) & 7], x[(i - 8) & 7], 8);
      const __m128i w7 = _mm_alignr_epi8(x[(i - 3) & 7], x[(i - 4) & 7], 8);
      const __m128i w2 = x[(i - 1) & 7];
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      x[i & 7] = _mm_add_epi64(_mm_add_epi64(x[(i - 8) & 7], s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i & 7], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    Rounds<1>(h, wk);
  }
}

// V5: V4 with AVX-512VL rotates.
__attribute__((target("bmi2,avx2,avx512f,avx512vl"))) void V5(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  #define rotr(x, n) _mm_ror_epi64(x, n)
  for (; blocks; --blocks, data += 128) {
    alignas(32) uint64_t wk[80];
    __m128i x[8];
    for (int i = 0; i < 8; ++i) {
      x[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    UNROLL
    for (int i = 8; i < 40; ++i) {
      // w[2i..2i+1] from x[i-8] (w-16), x[i-8..i-7] shifted by one (w-15), x[i-4..i-3] shifted (w-7), x[i-1] (w-2).
      const __m128i w15 = _mm_alignr_epi8(x[(i - 7) & 7], x[(i - 8) & 7], 8);
      const __m128i w7 = _mm_alignr_epi8(x[(i - 3) & 7], x[(i - 4) & 7], 8);
      const __m128i w2 = x[(i - 1) & 7];
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      x[i & 7] = _mm_add_epi64(_mm_add_epi64(x[(i - 8) & 7], s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i & 7], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    Rounds<1>(h, wk);
  }
}

#undef rotr
// V6: V4 on SSSE3 only (no BMI2).
__attribute__((target("ssse3"))) void V6(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  auto rotr = [](__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); };
  for (; blocks; --blocks, data += 128) {
    alignas(32) uint64_t wk[80];
    __m128i x[8];
    for (int i = 0; i < 8; ++i) {
      x[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    UNROLL
    for (int i = 8; i < 40; ++i) {
      // w[2i..2i+1] from x[i-8] (w-16), x[i-8..i-7] shifted by one (w-15), x[i-4..i-3] shifted (w-7), x[i-1] (w-2).
      const __m128i w15 = _mm_alignr_epi8(x[(i - 7) & 7], x[(i - 8) & 7], 8);
      const __m128i w7 = _mm_alignr_epi8(x[(i - 3) & 7], x[(i - 4) & 7], 8);
      const __m128i w2 = x[(i - 1) & 7];
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      x[i & 7] = _mm_add_epi64(_mm_add_epi64(x[(i - 8) & 7], s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i & 7], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    Rounds<2>(h, wk);
  }
}

// V7: V4 as bmi2+ssse3 (no VEX).
__attribute__((target("bmi2,ssse3"))) void V7(uint64_t h[8], const uint8_t* data, size_t blocks) {
  const __m128i swap = _mm_set_epi8(8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7);
  auto rotr = [](__m128i x, int n) { return _mm_or_si128(_mm_srli_epi64(x, n), _mm_slli_epi64(x, 64 - n)); };
  for (; blocks; --blocks, data += 128) {
    alignas(32) uint64_t wk[80];
    __m128i x[8];
    for (int i = 0; i < 8; ++i) {
      x[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)), swap);
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    UNROLL
    for (int i = 8; i < 40; ++i) {
      // w[2i..2i+1] from x[i-8] (w-16), x[i-8..i-7] shifted by one (w-15), x[i-4..i-3] shifted (w-7), x[i-1] (w-2).
      const __m128i w15 = _mm_alignr_epi8(x[(i - 7) & 7], x[(i - 8) & 7], 8);
      const __m128i w7 = _mm_alignr_epi8(x[(i - 3) & 7], x[(i - 4) & 7], 8);
      const __m128i w2 = x[(i - 1) & 7];
      const __m128i s0 = _mm_xor_si128(_mm_xor_si128(rotr(w15, 1), rotr(w15, 8)), _mm_srli_epi64(w15, 7));
      const __m128i s1 = _mm_xor_si128(_mm_xor_si128(rotr(w2, 19), rotr(w2, 61)), _mm_srli_epi64(w2, 6));
      x[i & 7] = _mm_add_epi64(_mm_add_epi64(x[(i - 8) & 7], s0), _mm_add_epi64(w7, s1));
      _mm_store_si128(reinterpret_cast<__m128i*>(wk + 2 * i), _mm_add_epi64(x[i & 7], _mm_loadu_si128(reinterpret_cast<const __m128i*>(K + 2 * i))));
    }
    Rounds<3>(h, wk);
  }
}

using Fn = void (*)(uint64_t*, const uint8_t*, size_t);

int main() {
  std::vector<uint8_t> data(65536);
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 131 + 7);
  BCRYPT_ALG_HANDLE alg;
  BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr, 0);
  const Fn fns[] = {V0, V1, V2, V3, V4, V5, V6, V7};
  const char* names[] = {"V0 current", "V1 wk+maj", "V2 bmi2", "V3 bmi2+sse sched", "V4 regs sched", "V5 avx512 ror", "V6 ssse3 only", "V7 bmi2+ssse3"};
  uint64_t ref[8];
  for (int f = 0; f < 8; ++f) {
    uint64_t h[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    fns[f](h, data.data(), 512);
    if (f == 0) std::memcpy(ref, h, 64);
    const bool same = std::memcmp(ref, h, 64) == 0;
    double best = 1e30;
    for (int r = 0; r < 15; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      for (int n = 0; n < 50; ++n) fns[f](h, data.data(), 512);
      best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 50);
    }
    std::printf("%-20s %9.0f ns / 64 KiB  %.3f ns/B %s\n", names[f], best, best / 65536, same ? "" : "WRONG");
  }
  uint8_t out[64];
  double best = 1e30;
  for (int r = 0; r < 15; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int n = 0; n < 50; ++n) BCryptHash(alg, nullptr, 0, data.data(), 65536, out, 64);
    best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 50);
  }
  std::printf("%-20s %9.0f ns / 64 KiB  %.3f ns/B\n", "CNG", best, best / 65536);
}
