// Timings of the C runtime's sin, cos and asin (any360/xdk/crt_math.h) against their models and the version first
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
#include <random>
#include <vector>

#include "xdk_bench.h"

#include "any360/xdk/crt_math.h"
#include "any360/xdk/crt_math_reference.h"

namespace first {
using any360::xdk::CrtAsinBuild;
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
  const xdk::CrtTrigBuild trig{0x1000, 0x1100, 0x1108};
  const xdk::CrtAsinBuild asin{0x1200};
  const uint32_t r1 = 0xFFF00;
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> angle(-6.3, 6.3), unit(-1.0, 1.0);
  std::vector<double> angles(1 << 20), units(1 << 20);
  for (double& x : angles) x = angle(rng);
  for (double& x : units) x = unit(rng);
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
    std::printf("sin  %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", sin_now, sin_first, sin_first / sin_now,
                sin_model, sin_model / sin_now);
    std::printf("cos  %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", cos_now, cos_first, cos_first / cos_now,
                cos_model, cos_model / cos_now);
    std::printf("asin %.2f ns a call, first %.2f (%.2fx), model %.2f (%.2fx)\n", asin_now, asin_first,
                asin_first / asin_now, asin_model, asin_model / asin_now);
  }
  xdk_bench::PrintAllocated("xdk_crt_math_bench");
  return 0;
}
