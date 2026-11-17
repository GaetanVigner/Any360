// The C runtime's sin, cos, asin, atan2 and pow, native: the same operations as the function's own code, in the same order, on the
// build's own coefficients (its tables, read where the function reads them), so the result is the same to the bit:
// fused multiply-adds where the code has fmadd and fnmsub, fctid rounding as the host's mode says, the same tests.
// Their models are any360/xdk/crt_math_reference.h. Which build has them where: db/xdk_seed.json.
//
// sin and cos also leave on the stack what the function stores there (sin: |x| at r1 - 8 and the quadrant at
// r1 - 16; cos: the quadrant at r1 - 16). They decline (false, nothing written) a stack or a table that is not plain
// memory, and sin and cos a NaN argument: their fnmsub, -fma(a, b, -c) in the recompiled code, is one the compiler may
// fold into another fused form, which carries a NaN's sign otherwise, so a NaN's sign bit depends on how the function
// was compiled (the unit tests saw both). Numbers and infinities do not reach it. The caller turns the host's flush-to-zero mode off first, as the function's first instruction does.
// Allocate nothing.
#pragma once

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define ANY360_XDK_MATH_SSE2 1
#endif

#include "any360/xdk/crt.h"

namespace any360::xdk {

// Where a build keeps sin's and cos's coefficients: the table (doubles and floats at +0 to +119), the constant term of
// the polynomial, and the value given past the range the reduction holds for.
struct CrtTrigBuild {
  uint32_t table = 0, one = 0, big = 0;
};
// Where a build keeps asin's coefficients (+0 to +127).
struct CrtAsinBuild {
  uint32_t table = 0;
};
// Where a build keeps atan2's (+0 to +179: doubles, the floats 0 and 1 at +168 and +176).
struct CrtAtan2Build {
  uint32_t table = 0;
};
// Where a build keeps pow's: the table (+0 to +487: the powers 2^(-i/16) and their corrections, the log2 and exp2
// polynomials, the limits, its log's coefficients) and the constants it loads from the runtime's shared pool.
struct CrtPowBuild {
  uint32_t table = 0;
  uint32_t zero = 0, one = 0, minus_one = 0, half = 0, sixteenth = 0, two = 0, sixteen = 0;
  uint32_t y_limit = 0, huge = 0, nan = 0;
};

namespace detail {

// fctid and fctidz as the recompiled code has them: NaN gives the lowest integer, above the highest the highest,
// below the lowest the processor's own answer (the lowest).
inline int64_t Fctid(double v) {
  if (std::isnan(v)) return INT64_MIN;
  if (v > double(LLONG_MAX)) return LLONG_MAX;
#if ANY360_XDK_MATH_SSE2
  return _mm_cvtsd_si64(_mm_load_sd(&v));
#else
  return v < double(LLONG_MIN) ? INT64_MIN : std::llrint(v);
#endif
}
// fctiwz as the recompiled code has it: NaN gives the lowest 32-bit integer, from the highest up the highest, below
// the lowest the processor's own answer (the lowest).
inline int32_t Fctiwz(double v) {
  if (std::isnan(v)) return INT32_MIN;
  if (v >= double(INT_MAX)) return INT_MAX;
#if ANY360_XDK_MATH_SSE2
  return _mm_cvttsd_si32(_mm_load_sd(&v));
#else
  return v <= double(INT_MIN) - 1.0 ? INT32_MIN : int32_t(v);
#endif
}
inline int64_t Fctidz(double v) {
  if (std::isnan(v)) return INT64_MIN;
  if (v > double(LLONG_MAX)) return LLONG_MAX;
#if ANY360_XDK_MATH_SSE2
  return _mm_cvttsd_si64(_mm_load_sd(&v));
#else
  return v < double(LLONG_MIN) ? INT64_MIN : int64_t(v);
#endif
}

inline double GuestDouble(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return std::bit_cast<double>(std::byteswap(v));
}
inline double GuestSingle(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return double(std::bit_cast<float>(std::byteswap(v)));
}
inline void GuestStoreBits(uint8_t* p, uint64_t v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 8);
}
inline double FlipSign(double v) { return std::bit_cast<double>(std::bit_cast<uint64_t>(v) ^ 0x8000000000000000ull); }
inline double Abs(double v) { return std::bit_cast<double>(std::bit_cast<uint64_t>(v) & ~0x8000000000000000ull); }

// sin's and cos's shared tail: the polynomial of the reduced argument r (r * r its square), its sign by the
// quadrant's lowest bit.
inline double TrigPolynomial(const uint8_t* t, double one, double r, int64_t quadrant) {
  const double s = r * r;
  double p = std::fma(GuestDouble(t + 112), s, GuestDouble(t + 104));
  p = std::fma(p, s, GuestDouble(t + 96));
  p = std::fma(p, s, GuestDouble(t + 88));
  p = std::fma(p, s, GuestDouble(t + 80));
  p = std::fma(p, s, GuestDouble(t + 72));
  p = std::fma(p, s, GuestDouble(t + 64));
  p = std::fma(p, s, GuestDouble(t + 56));
  const double v = std::fma(p, s, one) * r;
  return (quadrant & 1) ? FlipSign(v) : v;
}

inline bool TrigPlain(const CrtTrigBuild& b, uint32_t r1) {
  return IsPlainMemory(r1 - 16, 16) && IsPlainMemory(b.table, 120) && IsPlainMemory(b.one, 8) && IsPlainMemory(b.big, 8);
}

}  // namespace detail

inline bool CrtSin(const Memory& memory, const CrtTrigBuild& b, uint32_t r1, double x, double& result) {
  using namespace detail;
  if (std::isnan(x) || !TrigPlain(b, r1)) return false;
  const uint8_t* t = memory.Host(b.table);
  const double ax = Abs(x);
  const double sign = x >= 0.0 ? GuestSingle(t + 28) : GuestSingle(t + 32);
  const double n = double(Fctid(GuestDouble(t + 8) * ax));
  const int64_t quadrant = Fctidz(n);
  double r = -std::fma(GuestDouble(t + 40), n, -ax);
  r = -std::fma(GuestDouble(t + 48), n, -r);
  const double v = TrigPolynomial(t, GuestDouble(memory.Host(b.one)), r, quadrant);
  GuestStoreBits(memory.Host(r1 - 8), std::bit_cast<uint64_t>(ax));
  GuestStoreBits(memory.Host(r1 - 16), uint64_t(quadrant));
  if (std::bit_cast<uint64_t>(ax) == 0) {
    result = x;
    return true;
  }
  result = ax - GuestDouble(t + 16) >= 0.0 ? GuestDouble(memory.Host(b.big)) : v * sign;
  return true;
}

inline bool CrtCos(const Memory& memory, const CrtTrigBuild& b, uint32_t r1, double x, double& result) {
  using namespace detail;
  if (std::isnan(x) || !TrigPlain(b, r1)) return false;
  const uint8_t* t = memory.Host(b.table);
  const double ax = Abs(x), shifted = GuestDouble(t + 0) + ax;
  const double n = double(Fctid(GuestDouble(t + 8) * shifted));
  const int64_t quadrant = Fctidz(n);
  const double m = n - GuestSingle(t + 36);
  double r = -std::fma(GuestDouble(t + 40), m, -ax);
  r = -std::fma(GuestDouble(t + 48), m, -r);
  const double v = TrigPolynomial(t, GuestDouble(memory.Host(b.one)), r, quadrant);
  GuestStoreBits(memory.Host(r1 - 16), uint64_t(quadrant));
  if (ax == GuestSingle(t + 24)) {
    result = GuestSingle(t + 28);
    return true;
  }
  result = shifted - GuestDouble(t + 16) >= 0.0 ? GuestDouble(memory.Host(b.big)) : v;
  return true;
}

inline bool CrtAsin(const Memory& memory, const CrtAsinBuild& b, double x, double& result) {
  using namespace detail;
  if (!IsPlainMemory(b.table, 128)) return false;
  const uint8_t* a = memory.Host(b.table);
  const double threshold = GuestSingle(a + 12);
  double ax = Abs(x), s, u;
  uint32_t index;
  if (ax > threshold) {  // near 1: asin(x) = pi/2 - 2 asin(sqrt((1 - x) / 2)), the sqrt's sign flipped
    s = (GuestSingle(a + 4) - ax) * threshold;
    u = FlipSign(std::sqrt(s) * GuestSingle(a + 8));
    index = 1;
  } else {
    s = ax * ax;
    if (x == GuestSingle(a + 0)) {
      result = x;
      return true;
    }
    u = ax;
    index = 0;
  }
  double q = std::fma(GuestDouble(a + 120) + s, s, GuestDouble(a + 112));
  double p = std::fma(GuestDouble(a + 80), s, GuestDouble(a + 72));
  p = std::fma(p, s, GuestDouble(a + 64));
  q = std::fma(q, s, GuestDouble(a + 104));
  p = std::fma(p, s, GuestDouble(a + 56));
  q = std::fma(q, s, GuestDouble(a + 96));
  p = std::fma(p, s, GuestDouble(a + 48));
  q = std::fma(q, s, GuestDouble(a + 88));
  const double v = ((p * s) * u) / q + u + GuestDouble(a + 16 + 8 * index);
  result = x >= 0.0 ? v : FlipSign(v);
  return true;
}

// atan2(y, x): y in f1, x in f2; leaves both where the function stores them (y at r1 + 16, x at r1 + 24, the
// caller's parameter words). Declines a NaN (an addition's NaN takes the payload of the operand the compiler put
// first) and a stack or table that is not plain memory.
inline bool CrtAtan2(const Memory& memory, const CrtAtan2Build& b, uint32_t r1, double y, double x, double& result) {
  using namespace detail;
  if (std::isnan(x) || std::isnan(y) || !IsPlainMemory(r1 + 16, 16) || !IsPlainMemory(b.table, 180)) return false;
  const uint8_t* a = memory.Host(b.table);
  GuestStoreBits(memory.Host(r1 + 16), std::bit_cast<uint64_t>(y));
  GuestStoreBits(memory.Host(r1 + 24), std::bit_cast<uint64_t>(x));
  const double zero = GuestSingle(a + 168);
  double v;
  if (x == zero) {
    if (y == zero) {  // atan2(+-0, +0) is y; atan2(+-0, -0) is +-pi
      result = !std::signbit(x) ? y : std::signbit(y) ? FlipSign(GuestDouble(a + 16)) : GuestDouble(a + 16);
      return true;
    }
    v = GuestDouble(a + 8);
  } else {
    double big = Abs(x), small = Abs(y);
    uint32_t index = 0;
    if (small > big) {
      const double swap = big;
      big = small;
      small = swap;
      index = 2;
    }
    double r = small / big;
    if (r > GuestDouble(a + 24)) {  // past tan(pi/12): r' = (sqrt3 r - 1) / (sqrt3 + r)
      const double k = GuestDouble(a + 40);
      ++index;
      const double sum = k + r;
      r = std::fma(k, r, -GuestSingle(a + 176)) / sum;
    }
    const double s = r * r;
    double q = GuestDouble(a + 112) + s;
    double p = std::fma(GuestDouble(a + 80), s, GuestDouble(a + 72));
    q = std::fma(q, s, GuestDouble(a + 104));
    p = std::fma(p, s, GuestDouble(a + 64));
    q = std::fma(q, s, GuestDouble(a + 96));
    p = std::fma(p, s, GuestDouble(a + 56));
    q = std::fma(q, s, GuestDouble(a + 88));
    v = ((p * s) * r) / q + r;
    if (index > 1) v = FlipSign(v);
    v = GuestDouble(a + 128 + 8 * index) + v;
  }
  const double pi_less = GuestDouble(a + 16) - v;
  v = x >= 0.0 ? v : pi_less;
  result = std::signbit(y) ? FlipSign(v) : v;
  return true;
}

namespace detail {

// pow's helpers, as the runtime has them (each its own function in the build).
// Whether v is an integer: 0 no (a subnormal neither), 1 odd, 2 even (fctid's saturation included).
inline int PowIntegerKind(double v, double half) {
  const uint64_t bits = std::bit_cast<uint64_t>(v);
  if ((bits & 0x7FF0000000000000ull) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0) return 0;
  if (double(Fctid(v)) != v) return 0;
  const double h = v * half;
  return double(Fctid(h)) == h ? 2 : 1;
}
// frexp for a value that is not 0: the fraction between 0.5 and 1 and the exponent; a subnormal's fraction shifted up
// one bit at a time until its leading one reaches the exponent field.
inline double PowFrexp(double v, int32_t& e) {
  uint64_t bits = std::bit_cast<uint64_t>(v);
  if ((bits & 0x7FF0000000000000ull) != 0) {
    e = int32_t(bits >> 52 & 0x7FF) - 1022;
  } else {
    const uint64_t sign = bits & 0x8000000000000000ull;
    e = -1021;
    bits &= 0x000FFFFFFFFFFFFFull;
    do {
      bits <<= 1;
      --e;
    } while (!(bits & 0x0010000000000000ull));
    bits = (bits & ~0x0010000000000000ull) | sign;
  }
  return std::bit_cast<double>((bits & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
}
inline int32_t PowGetExp(double v) {
  return int32_t(int16_t(uint16_t((std::bit_cast<uint64_t>(v) >> 52 & 0x7FF) - 1022)));
}
inline double PowSetExp(double v, int32_t e) {
  return std::bit_cast<double>(WithExponentField(std::bit_cast<uint64_t>(v), uint32_t(e) + 1022));
}
// log(v) for v positive and finite (all pow gives it: its first tests, for 1, infinities, NaNs and v <= 0, are not
// reached).
inline double PowLog(const uint8_t* t, double one, double half, double zero, double v) {
  if (v == one) return zero;
  int32_t e;
  if (v < GuestDouble(t + 472)) {  // subnormal: scaled by 2^52 first
    v = v * GuestDouble(t + 464);
    e = int32_t(std::bit_cast<uint64_t>(v) >> 52 & 0x7FF) - 1075;
  } else {
    e = int32_t(std::bit_cast<uint64_t>(v) >> 52 & 0x7FF) - 1022;
  }
  const double m =
      std::bit_cast<double>((std::bit_cast<uint64_t>(v) & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull);
  double z, d;
  if (m > GuestDouble(t + 344)) {
    z = (m - half) - half;
    d = (m + one) * half;
  } else {
    z = m - half;
    --e;
    d = (z + one) * half;
  }
  z = z / d;
  const double w = z * z, fe = double(int64_t(e));
  const double ln2_lo = fe * GuestDouble(t + 440);
  double p = -std::fma(w, GuestDouble(t + 456), -GuestDouble(t + 384));
  double q = std::fma(w - GuestDouble(t + 448), w, GuestDouble(t + 408));
  p = std::fma(p, w, -GuestDouble(t + 432));
  q = std::fma(q, w, -GuestDouble(t + 424));
  const double r = std::fma((p * w) / q + one, z, -ln2_lo);
  return std::fma(fe, GuestDouble(t + 352), r);
}
inline bool PowPlain(const CrtPowBuild& b, uint32_t r1) {
  if (!IsPlainMemory(r1 + 16, 16) || !IsPlainMemory(b.table, 488)) return false;
  for (uint32_t at : {b.zero, b.one, b.minus_one, b.half, b.sixteenth, b.two, b.sixteen, b.y_limit, b.huge, b.nan}) {
    if (!IsPlainMemory(at, 8)) return false;
  }
  return true;
}

}  // namespace detail

// pow(x, y): x in f1, y in f2; leaves both where the function stores them (x at r1 + 16, y at r1 + 24). Declines
// (nothing written; the function's own code runs) x = 0 with y not 0, and an x or y that is infinite or NaN: the
// function's own tests and callees for those. Every finite x and y otherwise, x < 0 included.
inline bool CrtPow(const Memory& memory, const CrtPowBuild& b, uint32_t r1, double x, double y, double& result) {
  using namespace detail;
  if (!PowPlain(b, r1)) return false;
  const auto c = [&](uint32_t at) { return GuestDouble(memory.Host(at)); };
  const double zero = c(b.zero), one = c(b.one);
  if (y != zero) {
    if (x == zero) return false;
    if ((std::bit_cast<uint64_t>(x) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull ||
        (std::bit_cast<uint64_t>(y) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) {
      return false;
    }
  }
  GuestStoreBits(memory.Host(r1 + 16), std::bit_cast<uint64_t>(x));
  GuestStoreBits(memory.Host(r1 + 24), std::bit_cast<uint64_t>(y));
  if (y == zero) {
    result = one;
    return true;
  }
  const uint8_t* t = memory.Host(b.table);
  double sign = one, ax = x;
  if (x < zero) {  // only an integer power: odd ones keep the sign
    const int kind = PowIntegerKind(y, c(b.half));
    if (kind == 0) {
      result = c(b.nan);
      return true;
    }
    if (kind == 1) sign = c(b.minus_one);
    ax = FlipSign(ax);
  }
  if (Abs(y) > c(b.y_limit)) {
    if (y < zero) ax = one / ax;
    result = ax > one ? c(b.huge) * sign : ax < one ? sign * zero : sign;
    return true;
  }
  int32_t e;
  double m = PowFrexp(ax, e);
  int32_t exponent;  // the result's, with w its fraction
  double w;
  if (!(y > GuestDouble(t + 336)) && PowIntegerKind(ax, c(b.half)) != 0 && PowIntegerKind(y, c(b.half)) != 0 &&
      y > zero) {
    // An integer to a small positive integer power: the fraction squared and multiplied, the exponents apart.
    int32_t n = Fctiwz(y);
    const int32_t n_e = int32_t(uint32_t(n) * uint32_t(e));
    w = one;
    while (n != 0) {
      if (n & 1) w = w * m;
      m = m * m;
      n >>= 1;
    }
    exponent = int32_t(uint32_t(PowGetExp(w)) + uint32_t(n_e));
    if (exponent > 2560) {
      result = (c(b.huge) * w) * sign;
      return true;
    }
    if (exponent < -2557) {
      result = (w * sign) * zero;
      return true;
    }
  } else {
    // log2(x) in sixteenths: the nearest 2^(-i/16) below the fraction, then a polynomial of the rest.
    const double sixteenth = c(b.sixteenth), sixteen = c(b.sixteen);
    int32_t i = m > GuestDouble(t + 72) ? 1 : 9;
    if (!(m > GuestDouble(t + 32 + 8 * i))) i += 4;
    if (!(m > GuestDouble(t + 16 + 8 * i))) i += 2;
    const int32_t d = int32_t(uint32_t(e) << 4) - i;
    double hi, lo;
    if ((d < 0 ? i - int32_t(uint32_t(e) << 4) : d) == 1) {
      hi = PowLog(t, one, c(b.half), zero, ax) * GuestDouble(t + 216);
      lo = zero;
    } else {
      const double a = GuestDouble(t + 8 + 8 * i);
      const double z = ((m - a) - GuestDouble(t + 144 + 8 * ((i + 1) >> 1))) / (a + m) * c(b.two);
      hi = double(int64_t(d)) * sixteenth;
      const double s = z * z;
      double p = std::fma(s, GuestDouble(t + 256), GuestDouble(t + 248));
      p = std::fma(p, s, GuestDouble(t + 240));
      p = std::fma(p, s, GuestDouble(t + 232));
      lo = std::fma((p * s) * z, GuestDouble(t + 216), z * GuestDouble(t + 224)) + z;
    }
    // y log2(x) in a sixteenths part and a rest, each product split so that nothing is lost.
    const auto sixteenths = [&](double v) { return double(Fctid(v * sixteen)) * sixteenth; };
    double whole = sixteenths(y);
    double rest = std::fma(y - whole, hi, lo * y);
    const double part = sixteenths(rest);
    whole = std::fma(whole, hi, part);
    rest = rest - part;
    double top = sixteenths(whole);
    rest = (whole - top) + rest;
    const double carry = sixteenths(rest);
    top = top + carry;
    rest = rest - carry;
    const double scaled = top * sixteen;
    if (scaled > GuestDouble(t + 320)) {
      result = c(b.huge) * sign;
      return true;
    }
    if (scaled < GuestDouble(t + 328)) {
      result = sign * zero;
      return true;
    }
    int32_t k = Fctiwz(scaled);
    if (rest > zero) {
      ++k;
      rest = rest - sixteenth;
    }
    double p = std::fma(rest, GuestDouble(t + 312), GuestDouble(t + 304));
    p = std::fma(p, rest, GuestDouble(t + 296));
    p = std::fma(p, rest, GuestDouble(t + 288));
    p = std::fma(p, rest, GuestDouble(t + 280));
    p = std::fma(p, rest, GuestDouble(t + 272));
    p = std::fma(p, rest, GuestDouble(t + 264));
    const int32_t q = k / 16 + (k >= 0 ? 1 : 0);
    w = std::fma(p, rest, one) * GuestDouble(t + 8 + 8 * (int32_t(uint32_t(q) << 4) - k));
    exponent = int32_t(uint32_t(PowGetExp(w)) + uint32_t(q));
  }
  if (exponent > 1024) {
    result = c(b.huge) * sign;
  } else if (exponent < -1021) {
    result = sign * zero;
  } else {
    result = PowSetExp(w, exponent) * sign;
  }
  return true;
}

}  // namespace any360::xdk
