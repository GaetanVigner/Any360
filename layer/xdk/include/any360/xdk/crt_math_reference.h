// The reference models of the C runtime's sin, cos and asin (any360/xdk/crt_math.h has the native ones): each
// function's PowerPC step by step, every load from the build's tables where the function loads it. r1 the stack
// pointer, f1 the argument; each returns f1. sin leaves |x| at r1 - 8 and its quadrant (fctidz) at r1 - 16, cos the
// quadrant at r1 - 16, asin nothing. fctid rounds as the host's rounding mode says; the caller turns flush-to-zero off
// first, as the function's first instruction does.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "any360/xdk/crt_math.h"

namespace any360::xdk::reference {

namespace math_detail {
inline uint64_t L64(const Memory& m, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, m.Host(a), 8);
  return std::byteswap(v);
}
inline double D(const Memory& m, uint32_t a) { return std::bit_cast<double>(L64(m, a)); }
inline double S(const Memory& m, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, m.Host(a), 4);
  return double(std::bit_cast<float>(std::byteswap(v)));
}
inline void St64(const Memory& m, uint32_t a, uint64_t v) {
  v = std::byteswap(v);
  std::memcpy(m.Host(a), &v, 8);
}
inline double Neg(double v) { return std::bit_cast<double>(std::bit_cast<uint64_t>(v) ^ 0x8000000000000000ull); }
}  // namespace math_detail

inline double CrtSin(const Memory& m, const CrtTrigBuild& b, uint32_t r1, double f1) {
  using namespace math_detail;
  const uint32_t t = b.table;
  const double f12 = std::bit_cast<double>(std::bit_cast<uint64_t>(f1) & ~0x8000000000000000ull);
  St64(m, r1 - 8, std::bit_cast<uint64_t>(f12));
  double f0 = S(m, t + 32), f9 = S(m, t + 28);
  f9 = f1 >= 0.0 ? f9 : f0;
  f0 = D(m, t + 8) * f12;
  double f13 = D(m, t + 40), f11 = D(m, t + 48), f10 = D(m, t + 112);
  f0 = double(detail::Fctid(f0));
  f13 = -std::fma(f13, f0, -f12);
  const int64_t f8 = detail::Fctidz(f0);
  St64(m, r1 - 16, uint64_t(f8));
  f13 = -std::fma(f11, f0, -f13);
  f11 = D(m, t + 104);
  f0 = f13 * f13;
  f10 = std::fma(f10, f0, f11);
  f10 = std::fma(f10, f0, D(m, t + 96));
  f10 = std::fma(f10, f0, D(m, t + 88));
  f10 = std::fma(f10, f0, D(m, t + 80));
  f10 = std::fma(f10, f0, D(m, t + 72));
  f10 = std::fma(f10, f0, D(m, t + 64));
  f10 = std::fma(f10, f0, D(m, t + 56));
  f0 = std::fma(f10, f0, D(m, b.one));
  f0 = f0 * f13;
  if (L64(m, r1 - 16) & 1) f0 = Neg(f0);
  if (L64(m, r1 - 8) == 0) return f1;  // beqlr: sin(+-0) is its argument
  f13 = D(m, t + 16);
  f11 = f0 * f9;
  f13 = f12 - f13;
  f0 = D(m, b.big);
  return f13 >= 0.0 ? f0 : f11;
}

inline double CrtCos(const Memory& m, const CrtTrigBuild& b, uint32_t r1, double f1) {
  using namespace math_detail;
  const uint32_t t = b.table;
  double f12 = std::bit_cast<double>(std::bit_cast<uint64_t>(f1) & ~0x8000000000000000ull);
  double f0 = D(m, t + 0), f13 = S(m, t + 36), f10 = D(m, t + 40);
  const double f11 = f0 + f12;
  f0 = D(m, t + 8);
  double f9 = D(m, t + 48);
  const double f8 = D(m, t + 112);
  f0 = f0 * f11;
  f0 = double(detail::Fctid(f0));
  const double f7 = f0;
  f0 = f0 - f13;
  St64(m, r1 - 16, uint64_t(detail::Fctidz(f7)));
  f13 = -std::fma(f10, f0, -f12);
  f10 = D(m, t + 104);
  f13 = -std::fma(f9, f0, -f13);
  f0 = f13 * f13;
  f9 = std::fma(f8, f0, f10);
  f9 = std::fma(f9, f0, D(m, t + 96));
  f9 = std::fma(f9, f0, D(m, t + 88));
  f9 = std::fma(f9, f0, D(m, t + 80));
  f9 = std::fma(f9, f0, D(m, t + 72));
  f9 = std::fma(f9, f0, D(m, t + 64));
  f9 = std::fma(f9, f0, D(m, t + 56));
  f0 = std::fma(f9, f0, D(m, b.one));
  f0 = f0 * f13;
  if (L64(m, r1 - 16) & 1) f0 = Neg(f0);
  if (f12 == S(m, t + 24)) return S(m, t + 28);
  f12 = f11 - D(m, t + 16);
  return f12 >= 0.0 ? D(m, b.big) : f0;
}

inline double CrtAsin(const Memory& m, const CrtAsinBuild& b, double f1) {
  using namespace math_detail;
  const uint32_t a = b.table;
  double f13 = std::bit_cast<double>(std::bit_cast<uint64_t>(f1) & ~0x8000000000000000ull), f0 = S(m, a + 12), f12;
  uint32_t index;
  if (f13 > f0) {
    f12 = S(m, a + 4);
    index = 1;
    f13 = f12 - f13;
    const double f11 = S(m, a + 8);
    f0 = f13 * f0;
    f13 = std::sqrt(f0);
    f13 = f13 * f11;
    f13 = Neg(f13);
  } else {
    f12 = S(m, a + 0);
    index = 0;
    f0 = f13 * f13;
    if (f1 == f12) return f1;  // beqlr
  }
  double f11 = D(m, a + 120), f10 = f11 + f0;
  f12 = D(m, a + 80);
  f11 = D(m, a + 72);
  f11 = std::fma(f12, f0, f11);
  f12 = D(m, a + 112);
  const double f9 = D(m, a + 16 + 8 * index);
  f10 = std::fma(f10, f0, f12);
  f11 = std::fma(f11, f0, D(m, a + 64));
  f10 = std::fma(f10, f0, D(m, a + 104));
  f11 = std::fma(f11, f0, D(m, a + 56));
  f10 = std::fma(f10, f0, D(m, a + 96));
  f11 = std::fma(f11, f0, D(m, a + 48));
  f12 = std::fma(f10, f0, D(m, a + 88));
  f0 = f11 * f0;
  f0 = f0 * f13;
  f0 = f0 / f12;
  f0 = f0 + f13;
  f0 = f0 + f9;
  f13 = Neg(f0);
  return f1 >= 0.0 ? f0 : f13;
}

}  // namespace any360::xdk::reference
