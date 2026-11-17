// The C runtime's sin, cos and asin (any360/xdk/crt_math.h) against their models (any360/xdk/crt_math_reference.h):
// the result's 64 bits and the whole guest memory (the stack words sin and cos leave), on doubles of every exponent
// and sign, the special values (+-0, infinities, NaNs with payloads, subnormals, past 2^63 where fctid saturates), the
// ranges the functions are used on, and asin around its threshold and past 1. The tables are made up here
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

constexpr uint32_t kTrig = 0x1000, kOne = 0x1100, kBig = 0x1108, kAsin = 0x1200, kStack = xdk_test::kStack;
const xdk::CrtTrigBuild kTrigBuild{kTrig, kOne, kBig};
const xdk::CrtAsinBuild kAsinBuild{kAsin};

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
    CHECK(pair.native == before);
  }
  std::printf("xdk_crt_math_tests: sin, cos, asin: %llu calls compared with their models (result bits and memory), "
              "%llu NaN arguments declined by sin and cos\n",
              (unsigned long long)g_compared, (unsigned long long)g_declined);
  return xdk_test::Finish("xdk_crt_math_tests");
}
