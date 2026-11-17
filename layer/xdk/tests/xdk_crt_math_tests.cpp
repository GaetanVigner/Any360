// The C runtime's sin, cos, asin, atan2 and pow (any360/xdk/crt_math.h) against their models
// (any360/xdk/crt_math_reference.h): the result's 64 bits and the whole guest memory (the stack words each leaves), on
// doubles of every exponent and sign, the special values (+-0, infinities, NaNs with payloads, subnormals, past 2^63
// where fctid saturates), the ranges the functions are used on, asin around its threshold and past 1, atan2 in every
// octant, pow on each of its paths (integer powers, x near 1, negative x, overflow and underflow, |y| past its limit). The tables are made up here
// (polynomials of the right shape, not any title's): what is tested is that the native one does the model's
// operations in its order, whatever the coefficients. Declined: a stack or a table that is not plain memory.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "any360/xdk/crt_math.h"
#include "any360/xdk/crt_math_reference.h"
#include "xdk_test.h"

namespace {

namespace xdk = any360::xdk;

constexpr uint32_t kTrig = 0x1000, kOne = 0x1100, kBig = 0x1108, kAsin = 0x1200, kAtan2 = 0x1300, kPow = 0x1400,
                   kPool = 0x1600, kStack = xdk_test::kStack;
const xdk::CrtTrigBuild kTrigBuild{kTrig, kOne, kBig};
const xdk::CrtAsinBuild kAsinBuild{kAsin};
const xdk::CrtAtan2Build kAtan2Build{kAtan2};
const xdk::CrtPowBuild kPowBuild{kPow,       kPool,      kPool + 8,  kPool + 16, kPool + 24, kPool + 32,
                                 kPool + 40, kPool + 48, kPool + 56, kPool + 64, kPool + 72};

void PutD(std::vector<uint8_t>& m, uint32_t at, double v) {
  const uint64_t b = std::byteswap(std::bit_cast<uint64_t>(v));
  std::memcpy(&m[at], &b, 8);
}
void PutS(std::vector<uint8_t>& m, uint32_t at, float v) {
  const uint32_t b = std::byteswap(std::bit_cast<uint32_t>(v));
  std::memcpy(&m[at], &b, 4);
}

void Tables(std::vector<uint8_t>& m) {
  const double pi = 3.141592653589793;
  PutD(m, kTrig + 0, pi / 2);           // cos: the shift to sin
  PutD(m, kTrig + 8, 2 / pi);           // the reduction's scale
  PutD(m, kTrig + 16, 1.0e9);           // past it, the constant at kBig
  PutS(m, kTrig + 24, std::numeric_limits<float>::infinity());  // cos: an argument with its own answer
  PutS(m, kTrig + 28, 1.0f);            // sin's sign for x >= 0 (and cos's special answer)
  PutS(m, kTrig + 32, -1.0f);           // sin's sign below 0
  PutS(m, kTrig + 36, 0.5f);            // cos: half a quadrant
  PutD(m, kTrig + 40, 1.5707963267341256);  // pi/2 in two parts
  PutD(m, kTrig + 48, 6.077100506506192e-11);
  const double c[8] = {-1.6666666666666666e-1, 8.333333333332249e-3, -1.984126982985795e-4, 2.7557313707070068e-6,
                       -2.5050760253406863e-8, 1.58969099521155e-10, -7.6e-13, 2.8e-15};
  for (int i = 0; i < 8; ++i) PutD(m, kTrig + 56 + 8 * i, c[i]);
  PutD(m, kOne, 1.0);
  PutD(m, kBig, std::numeric_limits<double>::quiet_NaN());
  PutS(m, kAsin + 0, 0.0f);
  PutS(m, kAsin + 4, 1.0f);
  PutS(m, kAsin + 8, 2.0f);
  PutS(m, kAsin + 12, 0.5f);
  PutD(m, kAsin + 16, 0.0);
  PutD(m, kAsin + 24, pi / 2);
  const double p[4] = {1.6666666666666666e-1, -3.255658186224009e-1, 2.0121253213486293e-1, -4.0055534500679e-2};
  const double q[5] = {-2.403394911734414, 2.0209457602335057, -6.882839716054533e-1, 7.703815055590194e-2, 0.0};
  for (int i = 0; i < 4; ++i) PutD(m, kAsin + 48 + 8 * i, p[i]);
  for (int i = 0; i < 5; ++i) PutD(m, kAsin + 88 + 8 * i, q[i]);
  // atan2: its octant offsets 0, pi/6, pi/2, pi/3, the reduction past tan(pi/12), a rational polynomial.
  PutD(m, kAtan2 + 8, pi / 2);
  PutD(m, kAtan2 + 16, pi);
  PutD(m, kAtan2 + 24, 0.2679491924311227);
  PutD(m, kAtan2 + 40, 1.7320508075688772);
  const double ap[4] = {-13.7, -20.5, -8.49, -0.838}, aq[4] = {41.07, 86.16, 59.58, 15.02};
  for (int i = 0; i < 4; ++i) PutD(m, kAtan2 + 56 + 8 * i, ap[i]);
  for (int i = 0; i < 4; ++i) PutD(m, kAtan2 + 88 + 8 * i, aq[i]);
  const double offsets[4] = {0.0, pi / 6, pi / 2, pi / 3};
  for (int i = 0; i < 4; ++i) PutD(m, kAtan2 + 128 + 8 * i, offsets[i]);
  PutS(m, kAtan2 + 168, 0.0f);
  PutS(m, kAtan2 + 176, 1.0f);
  // pow: 2^(-i/16) and small corrections, the log2 and exp2 polynomials, its limits, its log's coefficients.
  for (int i = 0; i <= 16; ++i) PutD(m, kPow + 8 + 8 * i, std::exp2(-i / 16.0));
  for (int i = 1; i <= 8; ++i) PutD(m, kPow + 144 + 8 * i, (i % 3 - 1) * 1.7e-17 * i);
  const double log2e = 1.4426950408889634;
  PutD(m, kPow + 216, log2e);
  PutD(m, kPow + 224, log2e - 1);
  const double lp[4] = {0.0833333333333332, 0.0125000000005, 0.00223214212859, 0.000434457756722};
  for (int i = 0; i < 4; ++i) PutD(m, kPow + 232 + 8 * i, lp[i]);
  const double ep[7] = {0.6931471805599453, 0.2402265069590954, 0.0555041086640856, 0.00961812905951724,
                        0.00133335413135858, 0.000154002904409898, 1.49288526805956e-05};
  for (int i = 0; i < 7; ++i) PutD(m, kPow + 264 + 8 * i, ep[i]);
  PutD(m, kPow + 320, 40960.0);
  PutD(m, kPow + 328, -40928.0);
  PutD(m, kPow + 336, 128.0);
  PutD(m, kPow + 344, 0.7071067811865476);
  PutD(m, kPow + 352, 0.693359375);
  PutD(m, kPow + 360, -2.121944400546906e-4);
  const double lg[13] = {0.4342944819032518, -64.12, 16.38, -0.7896, -769.5, 312.0, -35.67,
                         769.5,              64.12,  2.12e-4, 35.67, 0.7896, 9007199254740992.0};
  for (int i = 0; i < 13; ++i) PutD(m, kPow + 368 + 8 * i, lg[i]);
  PutD(m, kPow + 472, std::numeric_limits<double>::min());
  PutD(m, kPow + 480, std::numeric_limits<double>::max());
  const double pool[10] = {0.0, 1.0, -1.0, 0.5, 0.0625, 2.0, 16.0, 1e20, std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::quiet_NaN()};
  for (int i = 0; i < 10; ++i) PutD(m, kPool + 8 * i, pool[i]);
}

uint64_t g_compared = 0, g_declined = 0;
xdk_test::Pair* g_pair = nullptr;

void Compare(double x) {
  xdk_test::Pair& pair = *g_pair;
  for (int f = 0; f < 3; ++f) {
    double native = 0, model = 0;
    const bool taken = f == 0   ? xdk::CrtSin(pair.a, kTrigBuild, kStack, x, native)
                       : f == 1 ? xdk::CrtCos(pair.a, kTrigBuild, kStack, x, native)
                                : xdk::CrtAsin(pair.a, kAsinBuild, x, native);
    if (std::isnan(x) && f < 2) {  // sin and cos decline NaNs (nothing written): the function's own code runs
      CHECK(!taken);
      ++g_declined;
      continue;
    }
    model = f == 0   ? xdk::reference::CrtSin(pair.b, kTrigBuild, kStack, x)
            : f == 1 ? xdk::reference::CrtCos(pair.b, kTrigBuild, kStack, x)
                     : xdk::reference::CrtAsin(pair.b, kAsinBuild, x);
    CHECK(taken);
    // The stack words each leaves (and the bytes around them), every call; the whole memory now and then.
    const bool same = std::bit_cast<uint64_t>(native) == std::bit_cast<uint64_t>(model) &&
                      std::memcmp(&pair.native[kStack - 64], &pair.model[kStack - 64], 128) == 0 &&
                      (g_compared % 1000 || pair.SameEverywhere());
    CHECK(same);
    if (!same && xdk_test::g_failures < 10) {
      std::printf("  %s(%.17g = %016llX): native %016llX, model %016llX\n", f == 0 ? "sin" : f == 1 ? "cos" : "asin", x,
                  (unsigned long long)std::bit_cast<uint64_t>(x), (unsigned long long)std::bit_cast<uint64_t>(native),
                  (unsigned long long)std::bit_cast<uint64_t>(model));
    }
    ++g_compared;
  }
}

// atan2(a, b) (f = 0) and pow(a, b) (f = 1); what each declines, the function's own code takes.
uint64_t g_compared2 = 0, g_declined2 = 0;
void Compare2(int f, double a, double b) {
  xdk_test::Pair& pair = *g_pair;
  double native = 0;
  const bool taken = f == 0 ? xdk::CrtAtan2(pair.a, kAtan2Build, kStack, a, b, native)
                            : xdk::CrtPow(pair.a, kPowBuild, kStack, a, b, native);
  const bool finite = std::isfinite(a) && std::isfinite(b);
  const bool declines = f == 0 ? std::isnan(a) || std::isnan(b) : b != 0.0 && (a == 0.0 || !finite);
  CHECK(taken == !declines);
  if (!taken) {
    ++g_declined2;
    return;
  }
  const double model = f == 0 ? xdk::reference::CrtAtan2(pair.b, kAtan2Build, kStack, a, b)
                              : xdk::reference::CrtPow(pair.b, kPowBuild, kStack, a, b);
  const bool same = std::bit_cast<uint64_t>(native) == std::bit_cast<uint64_t>(model) &&
                    std::memcmp(&pair.native[kStack - 64], &pair.model[kStack - 64], 128) == 0 &&
                    (g_compared2 % 1000 || pair.SameEverywhere());
  CHECK(same);
  if (!same && xdk_test::g_failures < 10) {
    std::printf("  %s(%.17g, %.17g): native %016llX, model %016llX\n", f == 0 ? "atan2" : "pow", a, b,
                (unsigned long long)std::bit_cast<uint64_t>(native), (unsigned long long)std::bit_cast<uint64_t>(model));
  }
  ++g_compared2;
}

}  // namespace

int main() {
  xdk_test::Pair pair(0x5135);
  Tables(pair.native);
  Tables(pair.model);
  g_pair = &pair;
  const double inf = std::numeric_limits<double>::infinity();
  const double specials[] = {0.0, -0.0, inf, -inf, std::numeric_limits<double>::quiet_NaN(),
                             std::bit_cast<double>(0x7FF8000000000123ull), std::bit_cast<double>(0xFFF4000000000001ull),
                             std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(),
                             std::numeric_limits<double>::min(), std::numeric_limits<double>::max(), 9.3e18, -9.3e18,
                             1.0e9, 1.0e9 + 1, 0.5, -0.5, 0.49999999999999994, 0.5000000000000001, 1.0, -1.0,
                             1.0000000000000002, 2.0, 3.141592653589793, 1.5707963267948966, 6.283185307179586};
  for (double x : specials) Compare(x);
  std::mt19937_64 rng(0x5137);
  for (int i = 0; i < 20000; ++i) Compare(std::bit_cast<double>(rng()));  // every exponent, sign and NaN
  std::uniform_real_distribution<double> wide(-1000.0, 1000.0), angle(-10.0, 10.0), unit(-1.0, 1.0);
  for (int i = 0; i < 20000; ++i) Compare(wide(rng));
  for (int i = 0; i < 40000; ++i) Compare(angle(rng));
  for (int i = 0; i < 40000; ++i) Compare(unit(rng));
  for (int i = 0; i < 4000; ++i) Compare(0.5 + (int64_t(rng() % 2001) - 1000) * 1e-16);  // around asin's threshold
  // atan2 and pow: special values against each other, random bits, then the ranges and each path.
  for (double a : specials) {
    for (double b : specials) {
      Compare2(0, a, b);
      Compare2(1, a, b);
    }
  }
  for (int i = 0; i < 20000; ++i) {
    const double a = std::bit_cast<double>(rng()), b = std::bit_cast<double>(rng());
    Compare2(0, a, b);
    Compare2(1, a, b);
  }
  std::uniform_real_distribution<double> positive(1e-6, 100.0), exponent(-12.0, 12.0), near_one(0.99, 1.01);
  for (int i = 0; i < 40000; ++i) Compare2(0, wide(rng), wide(rng));
  for (int i = 0; i < 20000; ++i) Compare2(0, unit(rng) * 1e-300, unit(rng));  // tiny ratios
  for (int i = 0; i < 40000; ++i) Compare2(1, positive(rng), exponent(rng));
  for (int i = 0; i < 20000; ++i) Compare2(1, near_one(rng), wide(rng));          // the log's path
  for (int i = 0; i < 20000; ++i) Compare2(1, 1.0 + (int64_t(rng() % 2001) - 1000) * 1e-15, angle(rng));
  for (int i = 0; i < 20000; ++i) {                                               // integer powers, negative x
    const double base = double(int64_t(rng() % 41) - 20), power = double(int64_t(rng() % 300) - 20);
    Compare2(1, base, power);
    Compare2(1, base + 0.5, power);
    Compare2(1, base, power + 0.5);
  }
  for (int i = 0; i < 20000; ++i) {  // overflow, underflow, subnormal x, |y| near and past its limit
    Compare2(1, std::ldexp(positive(rng), int(rng() % 2100) - 1100), exponent(rng) * 100.0);
    Compare2(1, std::numeric_limits<double>::denorm_min() * double(rng() % 100000 + 1), unit(rng) * 3.0);
    Compare2(1, positive(rng) / 50.0, unit(rng) * 2e20);
    Compare2(1, 2.0, 1023.0 + unit(rng) * 4.0);
  }
  CHECK(pair.SameEverywhere());
  // Declined, nothing written: a stack or a table that is not plain memory.
  {
    xdk_test::Pair pair(5);
    Tables(pair.native);
    const std::vector<uint8_t> before = pair.native;
    double r = 0;
    CHECK(!xdk::CrtSin(pair.a, kTrigBuild, 0x7F000008, 1.0, r) && !xdk::CrtCos(pair.a, kTrigBuild, 0x7F000008, 1.0, r));
    CHECK(!xdk::CrtSin(pair.a, {0x7EFFFFC0, kOne, kBig}, kStack, 1.0, r));
    CHECK(!xdk::CrtAsin(pair.a, {0x7EFFFFC0}, 0.3, r));
    CHECK(!xdk::CrtAtan2(pair.a, kAtan2Build, 0x7EFFFFF8, 1.0, 2.0, r) && !xdk::CrtAtan2(pair.a, {0x7EFFFFC0}, kStack, 1.0, 2.0, r));
    CHECK(!xdk::CrtPow(pair.a, kPowBuild, 0x7EFFFFF8, 3.0, 2.0, r));
    xdk::CrtPowBuild far = kPowBuild;
    far.nan = 0x7EFFFFFC;
    CHECK(!xdk::CrtPow(pair.a, far, kStack, 3.0, 2.0, r));
    CHECK(pair.native == before);
  }
  std::printf("xdk_crt_math_tests: sin, cos, asin: %llu calls compared with their models (result bits and memory), "
              "%llu NaN arguments declined by sin and cos; atan2, pow: %llu compared, %llu declined (NaN, infinity, "
              "pow of 0)\n",
              (unsigned long long)g_compared, (unsigned long long)g_declined, (unsigned long long)g_compared2,
              (unsigned long long)g_declined2);
  return xdk_test::Finish("xdk_crt_math_tests");
}
