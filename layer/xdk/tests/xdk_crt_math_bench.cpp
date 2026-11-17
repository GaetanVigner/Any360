// Timings of the C runtime's sin, cos, asin, atan2 and pow (any360/xdk/crt_math.h) against their models and the version first
// written (kept here as the fixed point, never edited): nanoseconds a call on arguments of the ranges titles use. Not
// a test: the figures that decide a binding are the title's own, with --any360_xdk_bench (docs/NATIVE_FUNCTIONS.md).
// The tables are made up (the shape of a CRT's), as in xdk_crt_math_tests.
// Run: xdk_crt_math_bench [repeats]
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "xdk_bench.h"

#include "any360/xdk/crt_math.h"
#include "any360/xdk/crt_math_reference.h"

namespace first {
using any360::xdk::CrtAsinBuild;
using any360::xdk::CrtAtan2Build;
using any360::xdk::CrtPowBuild;
using any360::xdk::CrtTrigBuild;
using any360::xdk::IsPlainMemory;
using any360::xdk::Memory;
using namespace any360::xdk::detail;

NOINLINE bool CrtSin(const Memory& memory, const CrtTrigBuild& b, uint32_t r1, double x, double& result) {
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

NOINLINE bool CrtCos(const Memory& memory, const CrtTrigBuild& b, uint32_t r1, double x, double& result) {
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

NOINLINE bool CrtAsin(const Memory& memory, const CrtAsinBuild& b, double x, double& result) {
  if (!IsPlainMemory(b.table, 128)) return false;
  const uint8_t* a = memory.Host(b.table);
  const double threshold = GuestSingle(a + 12);
  double ax = Abs(x), s, u;
  uint32_t index;
  if (ax > threshold) {
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


NOINLINE bool CrtAtan2(const Memory& memory, const CrtAtan2Build& b, uint32_t r1, double y, double x, double& result) {
  using namespace any360::xdk::detail;
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

NOINLINE bool CrtPow(const Memory& memory, const CrtPowBuild& b, uint32_t r1, double x, double y, double& result) {
  using namespace any360::xdk::detail;
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

}  // namespace first

namespace {

namespace xdk = any360::xdk;

template <class F>
double NsPerCall(F&& f, const std::vector<double>& xs) {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const auto start = std::chrono::steady_clock::now();
  double sum = 0;
  for (double x : xs) sum += f(x);
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / double(xs.size());
  xdk_bench::g_sink = xdk_bench::g_sink + uint32_t(std::bit_cast<uint64_t>(sum));
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  xdk_bench::Allocated& a = xdk_bench::g_allocated;
  a.calls += xs.size(), a.bytes += spent.bytes, a.count += spent.count;
  a.max_bytes_a_call = std::max(a.max_bytes_a_call, double(spent.bytes) / double(xs.size()));
  return ns;
}

void Put(std::vector<uint8_t>& m, uint32_t at, double v) {
  const uint64_t b = std::byteswap(std::bit_cast<uint64_t>(v));
  std::memcpy(&m[at], &b, 8);
}
void PutS(std::vector<uint8_t>& m, uint32_t at, float v) {
  const uint32_t b = std::byteswap(std::bit_cast<uint32_t>(v));
  std::memcpy(&m[at], &b, 4);
}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  std::vector<uint8_t> arena(1u << 20, 0);
  xdk::Memory memory;
  memory.base = arena.data();
  const double pi = 3.141592653589793;
  Put(arena, 0x1000, pi / 2);
  Put(arena, 0x1008, 2 / pi);
  Put(arena, 0x1010, 1.0e9);
  PutS(arena, 0x1018, 1.0e30f);
  PutS(arena, 0x101C, 1.0f);
  PutS(arena, 0x1020, -1.0f);
  PutS(arena, 0x1024, 0.5f);
  Put(arena, 0x1028, 1.5707963267341256);
  Put(arena, 0x1030, 6.077100506506192e-11);
  for (int i = 0; i < 8; ++i) Put(arena, 0x1038 + 8 * i, (i % 2 ? 1.0 : -1.0) / std::tgamma(2.0 * i + 4));
  Put(arena, 0x1100, 1.0);
  Put(arena, 0x1108, 0.0);
  PutS(arena, 0x1200, 0.0f);
  PutS(arena, 0x1204, 1.0f);
  PutS(arena, 0x1208, 2.0f);
  PutS(arena, 0x120C, 0.5f);
  for (int i = 0; i < 14; ++i) Put(arena, 0x1210 + 8 * i, 0.1 * (i + 1));
  // atan2's and pow's (the shape of a CRT's, as in xdk_crt_math_tests).
  const double atan2_values[] = {pi / 2, pi, 0.2679491924311227, 0, 1.7320508075688772, 0, -13.7, -20.5, -8.49, -0.838,
                                 41.07, 86.16, 59.58, 15.02, 0, 0, pi / 6, pi / 2, pi / 3};
  for (int i = 0; i < 19; ++i) Put(arena, 0x1308 + 8 * i, atan2_values[i]);
  PutS(arena, 0x1300 + 168, 0.0f);
  PutS(arena, 0x1300 + 176, 1.0f);
  for (int i = 0; i <= 16; ++i) Put(arena, 0x1400 + 8 + 8 * i, std::exp2(-i / 16.0));
  for (int i = 1; i <= 8; ++i) Put(arena, 0x1400 + 144 + 8 * i, (i % 3 - 1) * 1.7e-17 * i);
  const double pow_values[] = {1.4426950408889634, 0.4426950408889634, 0.0833333333333332, 0.0125000000005,
                               0.00223214212859, 0.000434457756722, 0.6931471805599453, 0.2402265069590954,
                               0.0555041086640856, 0.00961812905951724, 0.00133335413135858, 0.000154002904409898,
                               1.49288526805956e-05, 40960.0, -40928.0, 128.0, 0.7071067811865476, 0.693359375,
                               -2.121944400546906e-4, 0.4342944819032518, -64.12, 16.38, -0.7896, -769.5, 312.0,
                               -35.67, 769.5, 64.12, 2.12e-4, 35.67, 0.7896, 9007199254740992.0,
                               std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
  for (int i = 0; i < 34; ++i) Put(arena, 0x1400 + 216 + 8 * i, pow_values[i]);
  const double pool[10] = {0.0, 1.0, -1.0, 0.5, 0.0625, 2.0, 16.0, 1e20, std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::quiet_NaN()};
  for (int i = 0; i < 10; ++i) Put(arena, 0x1600 + 8 * i, pool[i]);
  const xdk::CrtAtan2Build atan2{0x1300};
  const xdk::CrtPowBuild pow{0x1400, 0x1600, 0x1608, 0x1610, 0x1618, 0x1620, 0x1628, 0x1630, 0x1638, 0x1640, 0x1648};
  const xdk::CrtTrigBuild trig{0x1000, 0x1100, 0x1108};
  const xdk::CrtAsinBuild asin{0x1200};
  const uint32_t r1 = 0xFFF00;
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> angle(-6.3, 6.3), unit(-1.0, 1.0);
  std::vector<double> angles(1 << 20), units(1 << 20);
  for (double& x : angles) x = angle(rng);
  for (double& x : units) x = unit(rng);
  // atan2 on pairs of coordinates; pow on what titles raise (a positive base to a real power, and integer powers).
  std::uniform_real_distribution<double> coordinate(-100.0, 100.0), base(0.001, 10.0), power(-3.0, 3.0);
  std::vector<double> ys(1 << 20), xs(1 << 20), bases(1 << 20), powers(1 << 20);
  for (size_t i = 0; i < ys.size(); ++i) {
    ys[i] = coordinate(rng), xs[i] = coordinate(rng), bases[i] = base(rng);
    powers[i] = i % 4 == 3 ? double(rng() % 5 + 1) : power(rng);
  }
  const auto pairs = [&](const std::vector<double>& a, const std::vector<double>& b, auto&& f) {
    size_t i = 0;
    std::vector<double> index(a.size());
    return NsPerCall([&](double) { const double r = f(a[i], b[i]); ++i; return r; }, index);
  };
  for (int r = 0; r < repeats; ++r) {
    double out = 0;
    const double sin_now = NsPerCall([&](double x) { xdk::CrtSin(memory, trig, r1, x, out); return out; }, angles);
    const double sin_first = NsPerCall([&](double x) { first::CrtSin(memory, trig, r1, x, out); return out; }, angles);
    const double sin_model = NsPerCall([&](double x) { return xdk::reference::CrtSin(memory, trig, r1, x); }, angles);
    const double cos_now = NsPerCall([&](double x) { xdk::CrtCos(memory, trig, r1, x, out); return out; }, angles);
    const double cos_first = NsPerCall([&](double x) { first::CrtCos(memory, trig, r1, x, out); return out; }, angles);
    const double cos_model = NsPerCall([&](double x) { return xdk::reference::CrtCos(memory, trig, r1, x); }, angles);
    const double asin_now = NsPerCall([&](double x) { xdk::CrtAsin(memory, asin, x, out); return out; }, units);
    const double asin_first = NsPerCall([&](double x) { first::CrtAsin(memory, asin, x, out); return out; }, units);
    const double asin_model = NsPerCall([&](double x) { return xdk::reference::CrtAsin(memory, asin, x); }, units);
    const double atan2_now = pairs(ys, xs, [&](double a, double b) { xdk::CrtAtan2(memory, atan2, r1, a, b, out); return out; });
    const double atan2_first = pairs(ys, xs, [&](double a, double b) { first::CrtAtan2(memory, atan2, r1, a, b, out); return out; });
    const double atan2_model = pairs(ys, xs, [&](double a, double b) { return xdk::reference::CrtAtan2(memory, atan2, r1, a, b); });
    const double pow_now = pairs(bases, powers, [&](double a, double b) { xdk::CrtPow(memory, pow, r1, a, b, out); return out; });
    const double pow_first = pairs(bases, powers, [&](double a, double b) { first::CrtPow(memory, pow, r1, a, b, out); return out; });
    const double pow_model = pairs(bases, powers, [&](double a, double b) { return xdk::reference::CrtPow(memory, pow, r1, a, b); });
    std::printf("sin  %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", sin_now, sin_first, sin_first / sin_now,
                sin_model, sin_model / sin_now);
    std::printf("cos  %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", cos_now, cos_first, cos_first / cos_now,
                cos_model, cos_model / cos_now);
    std::printf("asin %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", asin_now, asin_first,
                asin_first / asin_now, asin_model, asin_model / asin_now);
    std::printf("atan2 %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", atan2_now, atan2_first,
                atan2_first / atan2_now, atan2_model, atan2_model / atan2_now);
    std::printf("pow  %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", pow_now, pow_first, pow_first / pow_now,
                pow_model, pow_model / pow_now);
  }
  xdk_bench::PrintAllocated("xdk_crt_math_bench");
  return 0;
}
