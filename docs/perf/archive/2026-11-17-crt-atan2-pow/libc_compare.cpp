// Any360's CRT math (Crackdown's own tables, read from its decrypted image) against the host's C library (UCRT):
// time a call, and how far the results are apart (bits differ, ULPs). Local tool, not in the repository.
// Run: libc_compare <image.bin>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

#include "any360/xdk/crt_math.h"

namespace xdk = any360::xdk;

static volatile double g_sink;

template <class F>
double NsPerCall(F&& f, size_t n) {
  double best = 1e30;
  for (int r = 0; r < 5; ++r) {
    const auto start = std::chrono::steady_clock::now();
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += f(i);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
    g_sink = sum;
    best = std::min(best, ns);
  }
  return best;
}

uint64_t Ulps(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0 : UINT64_MAX;
  const auto key = [](double v) {
    const int64_t i = std::bit_cast<int64_t>(v);
    return i < 0 ? INT64_MIN - i : i;  // monotonic over all doubles
  };
  const int64_t d = key(a) - key(b);
  return uint64_t(d < 0 ? -d : d);
}

struct Diff {
  size_t n = 0, differ = 0, one = 0, more = 0;
  uint64_t max = 0;
  void Add(double a, double b) {
    ++n;
    const uint64_t u = Ulps(a, b);
    if (u) ++differ;
    if (u == 1) ++one;
    if (u > 1) ++more;
    if (u > max) max = u;
  }
};

void Report(const char* name, double any360, double libc, const Diff& d) {
  std::printf("%-6s Any360 %6.2f ns, libC %6.2f ns (libC %.2fx Any360) | results: %zu of %zu differ (%.2f%%), "
              "1 ULP: %zu, more: %zu, max %llu ULP\n",
              name, any360, libc, any360 / libc, d.differ, d.n, 100.0 * d.differ / d.n, d.one, d.more,
              (unsigned long long)d.max);
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), {});
  xdk::Memory memory;
  memory.base = image.data() - 0x82000000u;
  const uint32_t r1 = 0x82000000u + uint32_t(image.size()) - 0x1000;  // a scratch stack in the copy
  const xdk::CrtTrigBuild trig{0x82BA3AF0, 0x820ED510, 0x82BA4308};
  const xdk::CrtAsinBuild asin{0x82BA3C20};
  const xdk::CrtAtan2Build atan2{0x82BA3B68};
  const xdk::CrtPowBuild pow{0x82063B38, 0x820ED4C0, 0x820ED510, 0x820EDA50, 0x820ED868, 0x820EE230,
                             0x820ED708, 0x820ED4F0, 0x82BA3AE8, 0x82BA4300, 0x82BA4308};
  const size_t n = 1 << 20;
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> angle(-6.3, 6.3), unit(-1.0, 1.0), coordinate(-100.0, 100.0),
      base(0.001, 10.0), power(-3.0, 3.0);
  std::vector<double> a(n), u(n), y(n), x(n), b(n), p(n);
  for (size_t i = 0; i < n; ++i) {
    a[i] = angle(rng), u[i] = unit(rng), y[i] = coordinate(rng), x[i] = coordinate(rng), b[i] = base(rng);
    p[i] = i % 4 == 3 ? double(rng() % 5 + 1) : power(rng);
  }
  double out = 0;
  {
    Diff d;
    for (size_t i = 0; i < n; ++i) d.Add((xdk::CrtSin(memory, trig, r1, a[i], out), out), std::sin(a[i]));
    Report("sin", NsPerCall([&](size_t i) { xdk::CrtSin(memory, trig, r1, a[i], out); return out; }, n),
           NsPerCall([&](size_t i) { return std::sin(a[i]); }, n), d);
  }
  {
    Diff d;
    for (size_t i = 0; i < n; ++i) d.Add((xdk::CrtCos(memory, trig, r1, a[i], out), out), std::cos(a[i]));
    Report("cos", NsPerCall([&](size_t i) { xdk::CrtCos(memory, trig, r1, a[i], out); return out; }, n),
           NsPerCall([&](size_t i) { return std::cos(a[i]); }, n), d);
  }
  {
    Diff d;
    for (size_t i = 0; i < n; ++i) d.Add((xdk::CrtAsin(memory, asin, u[i], out), out), std::asin(u[i]));
    Report("asin", NsPerCall([&](size_t i) { xdk::CrtAsin(memory, asin, u[i], out); return out; }, n),
           NsPerCall([&](size_t i) { return std::asin(u[i]); }, n), d);
  }
  {
    Diff d;
    for (size_t i = 0; i < n; ++i) d.Add((xdk::CrtAtan2(memory, atan2, r1, y[i], x[i], out), out), std::atan2(y[i], x[i]));
    Report("atan2", NsPerCall([&](size_t i) { xdk::CrtAtan2(memory, atan2, r1, y[i], x[i], out); return out; }, n),
           NsPerCall([&](size_t i) { return std::atan2(y[i], x[i]); }, n), d);
  }
  {
    Diff d;
    for (size_t i = 0; i < n; ++i) d.Add((xdk::CrtPow(memory, pow, r1, b[i], p[i], out), out), std::pow(b[i], p[i]));
    Report("pow", NsPerCall([&](size_t i) { xdk::CrtPow(memory, pow, r1, b[i], p[i], out); return out; }, n),
           NsPerCall([&](size_t i) { return std::pow(b[i], p[i]); }, n), d);
  }
  return 0;
}
