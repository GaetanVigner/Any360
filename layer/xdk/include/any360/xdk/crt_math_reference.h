// The reference models of the C runtime's sin, cos, asin, atan2 and pow (any360/xdk/crt_math.h has the native ones):
// each function's PowerPC step by step, every load from the build's tables where the function loads it. r1 the stack
// pointer, f1 the argument (atan2 and pow: f1 and f2); each returns f1. sin leaves |x| at r1 - 8 and its quadrant
// (fctidz) at r1 - 16, cos the quadrant at r1 - 16, asin nothing, atan2 and pow their two arguments at r1 + 16 and
// r1 + 24 (what they store in their own frames, below r1, is left out: nothing reads it after they return). fctid rounds as the host's rounding mode says; the caller turns flush-to-zero off
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

// atan2: f1 = y, f2 = x, both stored above r1 first (the caller's parameter words). Every argument (NaNs too).
inline double CrtAtan2(const Memory& m, const CrtAtan2Build& b, uint32_t r1, double f1, double f2) {
  using namespace math_detail;
  const uint32_t r11 = b.table;
  St64(m, r1 + 16, std::bit_cast<uint64_t>(f1));
  St64(m, r1 + 24, std::bit_cast<uint64_t>(f2));
  double f0 = S(m, r11 + 168);
  if (f2 == f0) {
    if (f1 == f0) {
      if ((L64(m, r1 + 24) >> 32 & 0x80000000u) == 0) return f1;  // beqlr
      if ((L64(m, r1 + 16) >> 32 & 0x80000000u) == 0) return D(m, r11 + 16);
      return Neg(D(m, r11 + 16));
    }
    f0 = D(m, r11 + 8);
  } else {
    double f12 = std::bit_cast<double>(std::bit_cast<uint64_t>(f2) & ~0x8000000000000000ull);
    uint32_t r10 = 0;
    double f13 = std::bit_cast<double>(std::bit_cast<uint64_t>(f1) & ~0x8000000000000000ull);
    f0 = f12;
    if (f13 > f0) {
      f0 = f13;
      r10 = 2;
      f13 = f12;
    }
    f13 = f13 / f0;
    f0 = D(m, r11 + 24);
    if (f13 > f0) {
      f0 = D(m, r11 + 40);
      r10 = r10 + 1;
      f12 = S(m, r11 + 176);
      const double f11 = f0 + f13;
      f0 = std::fma(f0, f13, -f12);
      f13 = f0 / f11;
    }
    f0 = f13 * f13;
    double f11 = D(m, r11 + 112);
    f12 = D(m, r11 + 80);
    double f10 = f11 + f0;
    f11 = D(m, r11 + 72);
    f11 = std::fma(f12, f0, f11);
    f12 = D(m, r11 + 104);
    f10 = std::fma(f10, f0, f12);
    f12 = D(m, r11 + 64);
    f11 = std::fma(f11, f0, f12);
    f12 = D(m, r11 + 96);
    f10 = std::fma(f10, f0, f12);
    f12 = D(m, r11 + 56);
    f11 = std::fma(f11, f0, f12);
    f12 = D(m, r11 + 88);
    f12 = std::fma(f10, f0, f12);
    f0 = f11 * f0;
    f0 = f0 * f13;
    f0 = f0 / f12;
    f0 = f0 + f13;
    if (int32_t(r10) > 1) f0 = Neg(f0);
    f13 = D(m, r11 + 128 + 8 * r10);
    f0 = f13 + f0;
  }
  // loc_82AB65D0
  double f13 = D(m, r11 + 16);
  f13 = f13 - f0;
  const double r = f2 >= 0.0 ? f0 : f13;  // fsel
  return (L64(m, r1 + 16) >> 32 & 0x80000000u) ? Neg(r) : r;
}

namespace math_detail {
// pow's callees, each from its own code, on the words it stores and loads (its own stack slots are local words here).
inline uint16_t Hi16(double v) { return uint16_t(std::bit_cast<uint64_t>(v) >> 48); }
inline uint32_t Hi32(double v) { return uint32_t(std::bit_cast<uint64_t>(v) >> 32); }
inline uint32_t Lo32(double v) { return uint32_t(std::bit_cast<uint64_t>(v)); }
inline double WithHi16(double v, uint16_t h) {
  return std::bit_cast<double>((std::bit_cast<uint64_t>(v) & 0x0000FFFFFFFFFFFFull) | uint64_t(h) << 48);
}
inline double Fcfid(int64_t v) { return double(v); }
// sub_82AB5608: 0 not an integer, 1 odd, 2 even.
inline int32_t IntegerKind(const Memory& m, const CrtPowBuild& b, double f1) {
  if ((Hi16(f1) & 0x7FF0) == 0) {
    if ((Hi32(f1) & 0xFFFFF) != 0) return 0;
    if (Lo32(f1) != 0) return 0;
  }
  double f0 = Fcfid(detail::Fctid(f1));
  if (f0 != f1) return 0;
  f0 = f1 * D(m, b.half);
  const double f13 = Fcfid(detail::Fctid(f0));
  return f13 == f0 ? 2 : 1;
}
// sub_82AC4530: frexp, the exponent in r9 (stored through r4).
inline double Frexp(const Memory& m, const CrtPowBuild& b, double f1, int32_t& r9) {
  const double f0z = D(m, b.zero);
  if (f1 == f0z) {
    r9 = 0;
    return f1;
  }
  uint16_t r8 = Hi16(f1);
  const uint16_t r7 = r8;
  double word;  // what goes to -8(r1) and -16(r1)
  if ((r7 & 0x7FF0) != 0 || ((Hi32(f1) & 0xFFFFF) == 0 && Lo32(f1) == 0)) {
    r9 = int32_t((r7 & 0x7FF0) >> 4) - 1022;
    word = f1;
  } else {
    uint32_t r6 = Hi32(f1), r10 = Lo32(f1);
    r9 = -1021;
    const bool r5 = f1 < f0z;
    if (!(r7 & 0x10)) {
      do {
        r6 = r6 << 1;
        if (r10 & 0x80000000u) r6 |= 1;
        r8 = uint16_t(r6 >> 16);
        r10 = r10 << 1;
        r9 = r9 - 1;
      } while (!(r8 & 0x10));
    }
    uint16_t r11 = r8 & 0xFFEF;
    if (r5) r11 |= 0x8000;
    word = std::bit_cast<double>(uint64_t(r11) << 48 | uint64_t(r6 & 0xFFFF) << 32 | r10);
  }
  return WithHi16(word, uint16_t((Hi16(word) & 0x800F) | 0x3FE0));
}
// sub_82AC4458 and sub_82AC4430: the exponent and the double with its exponent field made r4 + 1022.
inline int32_t GetExp(double f1) { return int32_t(int16_t(uint16_t(((Hi16(f1) >> 4) & 0x7FF) - 1022))); }
inline double SetExp(double f1, int32_t r4) {
  const uint32_t r11 = (uint32_t(r4) + 1022) << 4;
  return WithHi16(f1, uint16_t(r11 | (Hi16(f1) & 0x800F)));
}
// sub_82AB5CF8: log, from where pow calls it (a positive finite value).
inline double Log(const Memory& m, const CrtPowBuild& b, double f1) {
  const uint32_t t = b.table;
  const double f8 = D(m, b.one);
  if (f1 == f8) return D(m, b.zero);
  uint16_t r11 = Hi16(f1);
  int32_t r10;
  if (f1 < D(m, t + 472)) {
    f1 = f1 * D(m, t + 464);
    r11 = Hi16(f1);
    r10 = int32_t((r11 >> 4) & 0x7FF) - 1075;
  } else {
    r10 = int32_t((r11 & 0x7FF0) >> 4) - 1022;
  }
  double f0 = WithHi16(f1, uint16_t((r11 & 0x800F) | 0x3FE0));
  const uint32_t r = t + 344;
  double f13 = D(m, r + 0);
  const bool above = f0 > f13;
  f13 = D(m, b.half);
  if (above) {
    const double f12 = f0 - f13, f11 = f0 + f8;
    f0 = f12 - f13;
    f13 = f11 * f13;
  } else {
    f0 = f0 - f13;
    r10 = r10 - 1;
    const double f12 = f0 + f8;
    f13 = f12 * f13;
  }
  f13 = f0 / f13;
  const double f10 = D(m, r + 8);
  double f11 = D(m, t + 456);
  f0 = f13 * f13;
  double f9 = D(m, t + 448);
  double f7 = f0 - f9;
  f9 = D(m, t + 440);
  const double f12 = Fcfid(int64_t(r10));
  const double f6 = f12 * f9;
  f9 = D(m, r + 40);
  f9 = -std::fma(f0, f11, -f9);
  f11 = D(m, r + 64);
  f7 = std::fma(f7, f0, f11);
  f11 = D(m, t + 432);
  f9 = std::fma(f9, f0, -f11);
  f11 = D(m, t + 424);
  f11 = std::fma(f7, f0, -f11);
  f0 = f9 * f0;
  f0 = f0 / f11;
  f0 = f0 + f8;
  f0 = std::fma(f0, f13, -f6);
  return std::fma(f12, f10, f0);
}
}  // namespace math_detail

// pow: f1 = x, f2 = y, both stored above r1 first (at r1 + 16 and r1 + 24, the caller's parameter words). Modelled
// where the native one takes the call: x = 0 with y not 0 and the infinities and NaNs (sub_82AB5678's) are the
// function's own code's, not modelled.
inline double CrtPow(const Memory& m, const CrtPowBuild& b, uint32_t r1, double f1, double f2) {
  using namespace math_detail;
  const uint32_t r31 = b.table;
  double f30 = f2, f28 = f1;
  St64(m, r1 + 24, std::bit_cast<uint64_t>(f30));
  St64(m, r1 + 16, std::bit_cast<uint64_t>(f28));
  const double f27 = D(m, b.zero);
  if (f30 == f27) return D(m, b.one);
  // (f28 == 0 and exponent fields of all ones: not modelled)
  const double f26 = D(m, b.one);
  double f25 = f26;
  if (f28 < f27) {
    const int32_t r3 = IntegerKind(m, b, f30);
    if (r3 != 1 && r3 != 2) return D(m, b.nan);
    if (r3 == 1) f25 = D(m, b.minus_one);
    f28 = Neg(f28);
  }
  const auto overflow = [&] { return D(m, b.huge) * f25; };  // loc_82AB5920
  const auto underflow = [&] { return f25 * f27; };          // loc_82AB5938
  const double f13a = std::bit_cast<double>(std::bit_cast<uint64_t>(f30) & ~0x8000000000000000ull);
  if (f13a > D(m, b.y_limit)) {
    if (f30 < f27) f28 = f26 / f28;
    if (f28 > f26) return overflow();
    if (f28 < f26) return underflow();
    return f25;
  }
  int32_t r80;
  double f29 = Frexp(m, b, f28, r80);
  double f31;
  int32_t r4;
  bool general = f30 > D(m, r31 + 336);
  if (!general) general = IntegerKind(m, b, f28) == 0;
  if (!general) general = IntegerKind(m, b, f30) == 0;
  if (!general) general = !(f30 > f27);
  if (!general) {
    int32_t r11 = detail::Fctiwz(f30);
    f31 = f26;
    const int32_t r31i = int32_t(int64_t(r11) * int64_t(r80));
    if (r11 != 0) {
      do {
        if (r11 & 1) f31 = f31 * f29;
        f29 = f29 * f29;
        r11 = r11 >> 1;
      } while (r11 != 0);
    }
    r4 = int32_t(uint32_t(GetExp(f31)) + uint32_t(r31i));
    if (r4 > 2560) return (D(m, b.huge) * f31) * f25;
    if (r4 < -2557) return (f31 * f25) * f27;
  } else {
    int32_t r11 = 1;
    if (!(f29 > D(m, r31 + 72))) r11 = 9;
    if (!(f29 > D(m, r31 + 32 + 8 * uint32_t(r11)))) r11 = r11 + 4;
    if (!(f29 > D(m, r31 + 16 + 8 * uint32_t(r11)))) r11 = r11 + 2;
    uint32_t r10 = uint32_t(r80) << 4;
    const uint32_t r9 = r10 - uint32_t(r11);
    r10 = uint32_t(r11) - r10;
    if (int32_t(r9) >= 0) r10 = r9;
    const double f31s = D(m, b.sixteenth);
    double f12, f13;
    if (int32_t(r10) == 1) {
      const double f1l = Log(m, b, f28);
      const double f0 = D(m, r31 + 216);
      f13 = f27;
      f12 = f1l * f0;
    } else {
      const double f11 = D(m, r31 + 256), f10 = D(m, r31 + 216);
      double f0 = D(m, r31 + 8 + 8 * uint32_t(r11));
      r11 = (r11 + 1) >> 1;  // srawi + addze, r11 + 1 positive
      f12 = f29 - f0;
      f0 = f0 + f29;
      double f9 = Fcfid(int64_t(int32_t(r9)));
      f13 = D(m, r31 + 144 + 8 * uint32_t(r11));
      f13 = f12 - f13;
      f12 = f9 * f31s;
      f9 = D(m, r31 + 224);
      f13 = f13 / f0;
      f0 = D(m, b.two);
      f0 = f13 * f0;
      f13 = f0 * f0;
      const double f8 = f0 * f9;
      f9 = D(m, r31 + 248);
      f9 = std::fma(f13, f11, f9);
      f9 = std::fma(f9, f13, D(m, r31 + 240));
      const double f11b = std::fma(f9, f13, D(m, r31 + 232));
      f13 = f11b * f13;
      f13 = f13 * f0;
      f13 = std::fma(f13, f10, f8);
      f13 = f13 + f0;
    }
    // loc_82AB5B20
    double f10 = f13 * f30;
    double f0 = D(m, b.sixteen);
    double f11 = f30 * f0;
    f13 = Fcfid(detail::Fctid(f11));
    f13 = f13 * f31s;
    f11 = f30 - f13;
    f11 = std::fma(f11, f12, f10);
    f10 = f11 * f0;
    f10 = Fcfid(detail::Fctid(f10));
    f10 = f10 * f31s;
    f13 = std::fma(f13, f12, f10);
    f11 = f11 - f10;
    f12 = f13 * f0;
    f12 = Fcfid(detail::Fctid(f12));
    f12 = f12 * f31s;
    f13 = f13 - f12;
    f13 = f13 + f11;
    f11 = f13 * f0;
    f11 = Fcfid(detail::Fctid(f11));
    f11 = f11 * f31s;
    f12 = f12 + f11;
    f13 = f13 - f11;
    f0 = f12 * f0;
    if (f0 > D(m, r31 + 320)) return overflow();
    if (f0 < D(m, r31 + 328)) return underflow();
    int32_t k = detail::Fctiwz(f0);
    if (f13 > f27) {
      k = k + 1;
      f13 = f13 - f31s;
    }
    double f12p = std::fma(f13, D(m, r31 + 312), D(m, r31 + 304));
    // cntlzw twice, bit 5: 1 when k < 0; xori 1
    const uint32_t lz = k == 0 ? 32 : uint32_t(std::countl_zero(uint32_t(k)));
    const uint32_t lz2 = lz == 0 ? 32 : uint32_t(std::countl_zero(lz));
    const uint32_t r10b = ((lz2 >> 5) & 1) ^ 1;
    const bool ca = k < 0 && (uint32_t(k) & 0xF) != 0;
    const int32_t r9b = (k >> 4) + (ca ? 1 : 0);
    const int32_t r30 = int32_t(r10b + uint32_t(r9b));
    f12p = std::fma(f12p, f13, D(m, r31 + 296));
    const uint32_t index = (uint32_t(r30) << 4) - uint32_t(k);
    f12p = std::fma(f12p, f13, D(m, r31 + 288));
    f12p = std::fma(f12p, f13, D(m, r31 + 280));
    f12p = std::fma(f12p, f13, D(m, r31 + 272));
    f0 = std::fma(f12p, f13, D(m, r31 + 264));
    const double f12t = D(m, r31 + 8 + 8 * index);
    f0 = std::fma(f0, f13, f26);
    f31 = f0 * f12t;
    r4 = int32_t(uint32_t(GetExp(f31)) + uint32_t(r30));
  }
  // loc_82AB5C40
  if (r4 > 1024) return overflow();
  if (r4 < -1021) return underflow();
  return SetExp(f31, r4) * f25;
}

}  // namespace any360::xdk::reference
