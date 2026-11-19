// Montgomery product variants (scratch lab): exact same output as the kernel's split sums.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>

using U = unsigned __int128;

static inline uint64_t Mul64(uint64_t a, uint64_t b, uint64_t& high) {
  const U p = U(a) * b;
  high = uint64_t(p >> 64);
  return uint64_t(p);
}
static inline void MulAdd(uint64_t a, uint64_t b, uint64_t y, uint64_t z, uint64_t& low, uint64_t& high) {
  low = Mul64(a, b, high);
  low += y;
  high += low < y;
  low += z;
  high += low < z;
}

static void Finish(const uint64_t* ab, const uint64_t* mn, const uint64_t* m, uint64_t* out, uint32_t n) {
  bool greater = false;
  for (uint32_t i = n; i-- > 0;) {
    if (ab[i] != mn[i]) {
      greater = ab[i] > mn[i];
      break;
    }
  }
  uint64_t carry = 0, borrow = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const U sum = U(ab[i]) + (greater ? 0 : m[i]) + carry;
    carry = uint64_t(sum >> 64);
    const uint64_t lo = uint64_t(sum), diff = lo - mn[i];
    const uint64_t next_borrow = (lo < mn[i]) | (diff < borrow);
    out[i] = diff - borrow;
    borrow = next_borrow;
  }
}

// V0: current (MulAdd with compares).
__attribute__((noinline)) void V0(const uint64_t* a, const uint64_t* b, const uint64_t* m, uint64_t inv, uint64_t* out, uint32_t n) {
  uint64_t ab[64] = {}, mn[64] = {};
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t bi = b[i];
    const uint64_t q = (ab[0] - mn[0] + bi * a[0]) * inv;
    uint64_t ab_carry, mn_carry, low;
    MulAdd(bi, a[0], ab[0], 0, low, ab_carry);
    MulAdd(q, m[0], mn[0], 0, low, mn_carry);
    for (uint32_t j = 1; j < n; ++j) {
      MulAdd(bi, a[j], ab[j], ab_carry, ab[j - 1], ab_carry);
      MulAdd(q, m[j], mn[j], mn_carry, mn[j - 1], mn_carry);
    }
    ab[n - 1] = ab_carry;
    mn[n - 1] = mn_carry;
  }
  Finish(ab, mn, m, out, n);
}

// V1: 128-bit sums.
__attribute__((noinline)) void V1(const uint64_t* a, const uint64_t* b, const uint64_t* m, uint64_t inv, uint64_t* out, uint32_t n) {
  uint64_t ab[64] = {}, mn[64] = {};
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t bi = b[i];
    const uint64_t q = (ab[0] - mn[0] + bi * a[0]) * inv;
    U t = U(bi) * a[0] + ab[0];
    U u = U(q) * m[0] + mn[0];
    for (uint32_t j = 1; j < n; ++j) {
      t = U(bi) * a[j] + ab[j] + uint64_t(t >> 64);
      u = U(q) * m[j] + mn[j] + uint64_t(u >> 64);
      ab[j - 1] = uint64_t(t);
      mn[j - 1] = uint64_t(u);
    }
    ab[n - 1] = uint64_t(t >> 64);
    mn[n - 1] = uint64_t(u >> 64);
  }
  Finish(ab, mn, m, out, n);
}

// V2: V1 with the inner loop for 16-word steps (n a multiple of 4 unrolled by 4).
__attribute__((noinline)) void V2(const uint64_t* a, const uint64_t* b, const uint64_t* m, uint64_t inv, uint64_t* out, uint32_t n) {
  uint64_t ab[64] = {}, mn[64] = {};
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t bi = b[i];
    const uint64_t q = (ab[0] - mn[0] + bi * a[0]) * inv;
    U t = U(bi) * a[0] + ab[0];
    U u = U(q) * m[0] + mn[0];
    uint32_t j = 1;
#pragma clang loop unroll_count(4)
    for (; j < n; ++j) {
      t = U(bi) * a[j] + ab[j] + uint64_t(t >> 64);
      u = U(q) * m[j] + mn[j] + uint64_t(u >> 64);
      ab[j - 1] = uint64_t(t);
      mn[j - 1] = uint64_t(u);
    }
    ab[n - 1] = uint64_t(t >> 64);
    mn[n - 1] = uint64_t(u >> 64);
  }
  Finish(ab, mn, m, out, n);
}


#include <immintrin.h>
// V3: mulx + addcarry intrinsics, both chains interleaved.
__attribute__((noinline, target("bmi2,adx"))) void V3(const uint64_t* a, const uint64_t* b, const uint64_t* m, uint64_t inv, uint64_t* out, uint32_t n) {
  uint64_t ab[64] = {}, mn[64] = {};
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t bi = b[i];
    const uint64_t q = (ab[0] - mn[0] + bi * a[0]) * inv;
    unsigned long long th, uh, tl, ul;
    tl = _mulx_u64(bi, a[0], &th);
    ul = _mulx_u64(q, m[0], &uh);
    unsigned char c1 = _addcarry_u64(0, tl, ab[0], &tl);
    th += c1;
    unsigned char c2 = _addcarry_u64(0, ul, mn[0], &ul);
    uh += c2;
    for (uint32_t j = 1; j < n; ++j) {
      unsigned long long ph, qh;
      unsigned long long pl = _mulx_u64(bi, a[j], &ph);
      unsigned long long ql = _mulx_u64(q, m[j], &qh);
      unsigned char x = _addcarry_u64(0, pl, ab[j], &pl);
      x = _addcarry_u64(x, ph, 0, &ph);
      x = _addcarry_u64(0, pl, th, &pl);
      _addcarry_u64(x, ph, 0, &th);
      ab[j - 1] = pl;
      unsigned char y = _addcarry_u64(0, ql, mn[j], &ql);
      y = _addcarry_u64(y, qh, 0, &qh);
      y = _addcarry_u64(0, ql, uh, &ql);
      _addcarry_u64(y, qh, 0, &uh);
      mn[j - 1] = ql;
    }
    ab[n - 1] = th;
    mn[n - 1] = uh;
  }
  Finish(ab, mn, m, out, n);
}

using Fn = void (*)(const uint64_t*, const uint64_t*, const uint64_t*, uint64_t, uint64_t*, uint32_t);

int main() {
  std::mt19937_64 rng(3);
  uint64_t a[64], b[64], m[64], ref[64], out[64];
  for (int i = 0; i < 64; ++i) a[i] = rng(), b[i] = rng(), m[i] = rng();
  m[0] |= 1;
  uint64_t inv = (m[0] * 3) ^ 2;
  for (int i = 0; i < 4; ++i) inv *= 2 - m[0] * inv;
  const Fn fns[] = {V0, V1, V2, V3};
  for (uint32_t n : {16u, 32u, 64u}) {
    for (int f = 0; f < 4; ++f) {
      fns[f](a, b, m, inv, out, n);
      if (f == 0) std::memcpy(ref, out, sizeof out);
      const bool same = std::memcmp(ref, out, 8 * n) == 0;
      double best = 1e30;
      const int calls = 200000 / n;
      for (int r = 0; r < 15; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < calls; ++k) fns[f](a, b, m, inv, out, n);
        best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / calls);
      }
      std::printf("n=%2u V%d %8.1f ns %s\n", n, f, best, same ? "" : "WRONG");
    }
  }
}
