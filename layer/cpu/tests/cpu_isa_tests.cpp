// The CPU translator's instruction tests: each case of isa_cases.txt, translated by tools/ppc_translate.py at build
// time, against the PowerPC ISA's definition of its instructions (Power ISA 2.06, Book I) written here by hand, on
// many register states and the same guest memory. Floating-point cases draw their operands from edge values (zeros,
// infinities, a NaN, denormals, values that round or overflow). The known differences between the titles'
// recompilation, which the translation follows, and the ISA are checked by name at the end.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>

#include "cpu_indirect.h"
#include "cpu_isa_cases.h"
#include "cpu_test.h"

using namespace any360::cpu::test;
using any360::xdk::ppc_model::Load16;
using any360::xdk::ppc_model::Load32;
using any360::xdk::ppc_model::Load64;
using any360::xdk::ppc_model::Load8;
using any360::xdk::ppc_model::Store16;
using any360::xdk::ppc_model::Store32;
using any360::xdk::ppc_model::Store64;
using any360::xdk::ppc_model::Store8;
namespace isa = any360::cpu::translated::isa;

namespace {

using Translated = void (*)(Ctx&, const Memory&);
using Expected = void (*)(Ctx&, const Memory&);

void Compare(Cr& field, int64_t a, int64_t b, uint8_t so) {
  field.lt = a < b, field.gt = a > b, field.eq = a == b, field.so = so;
}
void CompareUnsigned(Cr& field, uint64_t a, uint64_t b, uint8_t so) {
  field.lt = a < b, field.gt = a > b, field.eq = a == b, field.so = so;
}
uint64_t Rotate32(uint32_t value, int n) {
  const uint64_t doubled = uint64_t(value) | (uint64_t(value) << 32);
  return std::rotl(doubled, n);
}
uint64_t Mask(int mb, int me) {
  uint64_t m = 0;
  for (int bit = mb;; bit = (bit + 1) & 63) {
    m |= 1ull << (63 - bit);
    if (bit == me) break;
  }
  return m;
}

void ExpectAddiLi(Ctx& c, const Memory&) {
  c.r3.u64 = c.r4.u64 - 5;
  c.r5.u64 = 123;
}
void ExpectAddisLis(Ctx& c, const Memory&) {
  c.r3.u64 = c.r4.u64 + (uint64_t(0x1234) << 16);
  c.r6.u64 = 0xFFFFFFFFFFFF0000ull;
}
void ExpectLwz(Ctx& c, const Memory& m) {
  c.r3.u64 = Load32(m, uint32_t(c.r4.u64 + 8));
  c.r5.u64 = Load32(m, 0x100);
}
void ExpectStw(Ctx& c, const Memory& m) { Store32(m, uint32_t(c.r4.u64 - 4), uint32_t(c.r3.u64)); }
void ExpectStwu(Ctx& c, const Memory& m) {
  const uint64_t ea = c.r1.u64 - 32;
  Store32(m, uint32_t(ea), uint32_t(c.r1.u64));
  c.r1.u64 = ea;
}
void ExpectLdStd(Ctx& c, const Memory& m) {
  c.r3.u64 = Load64(m, uint32_t(c.r4.u64 + 8));
  Store64(m, uint32_t(c.r4.u64 + 16), c.r5.u64);
}
void ExpectLfsFmulsStfs(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  const float a = std::bit_cast<float>(Load32(m, uint32_t(c.r4.u64 + 4)));
  const float b = std::bit_cast<float>(Load32(m, uint32_t(c.r4.u64 + 12)));
  c.f1.f64 = a;
  c.f2.f64 = b;
  c.f3.f64 = double(a * b);
  Store32(m, uint32_t(c.r4.u64 + 8), std::bit_cast<uint32_t>(a * b));
}
void ExpectCompare(Ctx& c, const Memory&) {
  const uint8_t so = c.xer.so;
  Compare(c.cr6, int32_t(c.r3.u64), -1, so);
  CompareUnsigned(c.cr7, uint32_t(c.r4.u64), 10, so);
  Compare(c.cr1, int32_t(c.r3.u64), int32_t(c.r4.u64), so);
  CompareUnsigned(c.cr2, uint32_t(c.r3.u64), uint32_t(c.r4.u64), so);
  Compare(c.cr3, int64_t(c.r3.u64), 5, so);
  CompareUnsigned(c.cr4, c.r3.u64, c.r4.u64, so);
  CompareUnsigned(c.cr5, c.r3.u64, 7, so);
  Compare(c.cr0, int64_t(c.r3.u64), int64_t(c.r4.u64), so);
}
void ExpectRlwinm(Ctx& c, const Memory&) {
  const uint32_t x = uint32_t(c.r4.u64);
  c.r3.u64 = Rotate32(x, 5) & Mask(10 + 32, 20 + 32);
  c.r5.u64 = Rotate32(x, 0) & Mask(24 + 32, 31 + 32);
  c.r6.u64 = Rotate32(x, 16) & Mask(20 + 32, 10 + 32);
  Compare(c.cr0, int32_t(c.r6.u64), 0, c.xer.so);
  c.r7.u64 = Rotate32(x, 9) & Mask(0 + 32, 31 + 32);
}
void ExpectOrMr(Ctx& c, const Memory&) {
  const uint64_t r4 = c.r4.u64, r5 = c.r5.u64;
  c.r3.u64 = r4 | r5;
  c.r6.u64 = r4;
  c.r7.u64 = r4 | r5;
  Compare(c.cr0, int32_t(c.r7.u64), 0, c.xer.so);
}
void ExpectAdd(Ctx& c, const Memory&) {
  c.r3.u64 = c.r4.u64 + c.r5.u64;
  c.r6.u64 = c.r4.u64 + c.r5.u64;
  Compare(c.cr0, int32_t(c.r6.u64), 0, c.xer.so);
}
void ExpectSpr(Ctx& c, const Memory&) {
  c.r3.u64 = c.lr;
  c.ctr.u64 = c.r4.u64;
  c.r5.u64 = c.ctr.u64;
  c.lr = c.r6.u64;
}
void ExpectBranch(Ctx& c, const Memory&) {
  Compare(c.cr6, int32_t(c.r4.u64), 0, c.xer.so);
  if (c.cr6.eq) {
    c.r3.u64 = 2;
    c.r5.u64 = 7;
  } else {
    c.r3.u64 = 1;
  }
}
void ExpectCountedLoop(Ctx& c, const Memory&) {
  c.r3.u64 = 3 * c.r4.u64;
  c.ctr.u64 = 0;
}
bool Lt(const Cr& f) { return f.lt; }
bool Gt(const Cr& f) { return f.gt; }
bool Eq(const Cr& f) { return f.eq; }
bool So(const Cr& f) { return f.so; }
void ExpectConditions(Ctx& c, const Memory&) {
  Compare(c.cr6, int32_t(c.r3.u64), int32_t(c.r4.u64), c.xer.so);
  const bool taken[8] = {Lt(c.cr6), Gt(c.cr6), Eq(c.cr6), So(c.cr6), !Lt(c.cr6), !Gt(c.cr6), !Eq(c.cr6), !So(c.cr6)};
  for (int k = 0; k < 8; ++k) {
    if (!taken[k]) c.r5.u64 += 1u << k;
  }
}
template <bool (*Bit)(const Cr&), bool kSense>
void ExpectConditionalReturn(Ctx& c, const Memory&) {
  Compare(c.cr6, int32_t(c.r3.u64), int32_t(c.r4.u64), c.xer.so);
  if (Bit(c.cr6) != kSense) c.r5.u64 = 9;
}
template <bool kBranchIfZero, bool (*Bit)(const Cr&), bool kSense>
void ExpectCountAndCondition(Ctx& c, const Memory&) {
  c.ctr.u64 = c.r4.u64 - 1;
  Compare(c.cr6, int32_t(c.r3.u64), int32_t(c.r5.u64), c.xer.so);
  const bool count = (uint32_t(c.ctr.u64) == 0) == kBranchIfZero;
  if (!(count && Bit(c.cr6) == kSense)) c.r6.u64 = 1;
}
void ExpectBdzlr(Ctx& c, const Memory&) {
  c.ctr.u64 = c.r4.u64 - 1;
  if (uint32_t(c.ctr.u64) != 0) c.r6.u64 = 1;
}
void ExpectBlSelf(Ctx& c, const Memory&) {
  if (uint32_t(c.r4.u64) != 0) c.lr = 0x10014;
  c.r3.u64 += c.r4.u64;
  c.r4.u64 = 0;
  CompareUnsigned(c.cr0, 0, 0, c.xer.so);
}
void ExpectBeqlr(Ctx& c, const Memory&) {
  CompareUnsigned(c.cr0, uint32_t(c.r4.u64), 0, c.xer.so);
  if (!c.cr0.eq) c.r3.u64 = 9;
}

uint32_t Ea(uint64_t base, int32_t offset) { return uint32_t(base + uint64_t(int64_t(offset))); }
uint64_t Carry32(uint64_t a, uint64_t b, uint64_t c = 0) {
  return (uint64_t(uint32_t(a)) + uint64_t(uint32_t(b)) + c) >> 32;
}
double Single(double x) { return double(float(x)); }

void ExpectLbzLhzStbSth(Ctx& c, const Memory& m) {
  c.r3.u64 = Load8(m, Ea(c.r4.u64, 1));
  c.r5.u64 = Load16(m, Ea(c.r4.u64, 2));
  Store8(m, Ea(c.r4.u64, 7), uint8_t(c.r3.u64));
  Store16(m, Ea(c.r4.u64, 10), uint16_t(c.r5.u64));
}
void ExpectIndexed(Ctx& c, const Memory& m) {
  c.r6.u64 = 12;
  c.r3.u64 = Load32(m, Ea(c.r4.u64, 12));
  c.r5.u64 = Load8(m, Ea(c.r4.u64, 12));
  c.r7.u64 = Load16(m, Ea(c.r4.u64, 12));
  c.r8.u64 = 20;
  Store32(m, Ea(c.r4.u64, 20), uint32_t(c.r5.u64));
  Store8(m, Ea(c.r4.u64, 20), uint8_t(c.r3.u64));
  Store16(m, Ea(c.r4.u64, 20), uint16_t(c.r7.u64));
  c.r9.u64 = Load32(m, uint32_t(c.r4.u64));
}
void ExpectLfd(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  c.f1.u64 = Load64(m, Ea(c.r4.u64, 8));
  Store64(m, Ea(c.r4.u64, 16), c.f1.u64);
}
void ExpectStfd(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  Store64(m, Ea(c.r4.u64, 24), c.f2.u64);
}
void ExpectLfsx(Ctx& c, const Memory& m) {
  c.r6.u64 = 24;
  c.fpscr.disableFlushMode();
  c.f2.f64 = double(std::bit_cast<float>(Load32(m, Ea(c.r4.u64, 24))));
}
void ExpectStfsx(Ctx& c, const Memory& m) {
  c.r7.u64 = 32;
  c.fpscr.disableFlushMode();
  Store32(m, Ea(c.r4.u64, 32), std::bit_cast<uint32_t>(float(c.f2.f64)));
}
void ExpectStfs(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  Store32(m, Ea(c.r4.u64, 40), std::bit_cast<uint32_t>(float(c.f1.f64)));
}
void ExpectFmr(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f3.u64 = c.f1.u64;
}
void ExpectFneg(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f4.u64 = c.f1.u64 ^ (1ull << 63);
}
void ExpectFabs(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f5.u64 = c.f1.u64 & ~(1ull << 63);
}
void ExpectFrsp(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f6.f64 = Single(c.f1.f64);
}
void FloatCompare(Cr& field, double a, double b) {
  const bool unordered = std::isnan(a) || std::isnan(b);
  field.lt = !unordered && a < b, field.gt = !unordered && a > b, field.eq = !unordered && a == b;
  field.so = unordered;
}
void ExpectFcmpu(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  FloatCompare(c.cr6, c.f1.f64, c.f2.f64);
  FloatCompare(c.cr0, c.f2.f64, c.f1.f64);
}
template <char kOp, bool kSingle>
void ExpectArithmetic(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  const double a = c.f1.f64, b = c.f2.f64;
  const double exact = kOp == '+' ? a + b : kOp == '-' ? a - b : kOp == '*' ? a * b : a / b;
  c.f3.f64 = kSingle ? Single(exact) : exact;
}
template <int kForm, bool kSingle>
void ExpectFused(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  const double a = c.f1.f64, cc = c.f2.f64, b = c.f3.f64;
  const double product_sum = kForm == 0 ? std::fma(a, cc, b) : std::fma(a, cc, -b);
  const double result = kForm == 2 ? -product_sum : product_sum;
  c.f4.f64 = kSingle ? Single(result) : result;
}
void ExpectLogicalImmediate(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  c.r3.u64 = x | 0x8001;
  c.r5.u64 = x | 0x80010000;
  c.r6.u64 = x ^ 0xFFFF;
  c.r7.u64 = x & 0xF0F0;
  Compare(c.cr0, int32_t(c.r7.u64), 0, c.xer.so);
}
void ExpectLogical(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r5.u64;
  c.r3.u64 = x & y;
  c.r6.u64 = x & ~y;
  c.r7.u64 = x ^ y;
  c.r8.u64 = ~(x | y);
  c.r9.u64 = ~x;
  c.r10.u64 = x & y;
  Compare(c.cr0, int32_t(c.r10.u64), 0, c.xer.so);
}
void ExpectIntegerArithmetic(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r5.u64;
  c.r3.u64 = y - x;
  c.r6.u64 = x - y;
  Compare(c.cr0, int32_t(c.r6.u64), 0, c.xer.so);
  c.r7.u64 = 0 - x;
  c.r8.u64 = uint64_t(int64_t(int8_t(x)));
  c.r9.u64 = uint64_t(int64_t(int16_t(x)));
  c.r10.u64 = uint64_t(int64_t(int32_t(x)));
  c.r11.u64 = uint64_t(int64_t(int8_t(x)));
  Compare(c.cr0, int32_t(c.r11.u64), 0, c.xer.so);
}
uint64_t ShiftWord(uint32_t value, uint64_t amount, bool left) {
  const uint32_t n = uint32_t(amount & 0x3F);
  if (n >= 32) return 0;
  return left ? uint64_t(uint32_t(value << n)) : uint64_t(value >> n);
}
void ShiftRightAlgebraic(Ctx& c, any360::xdk::ppc_model::Reg& target, uint32_t value, int n) {
  const int32_t s = int32_t(value);
  const uint32_t lost = n ? value & ((1u << n) - 1) : 0;
  c.xer.ca = s < 0 && lost != 0;
  target.u64 = uint64_t(int64_t(s >> n));
}
void ExpectShifts(Ctx& c, const Memory&) {
  const uint32_t x = uint32_t(c.r4.u64), y = uint32_t(c.r5.u64);
  c.r3.u64 = ShiftWord(y, c.r4.u64, true);
  c.r6.u64 = ShiftWord(y, c.r4.u64, false);
  ShiftRightAlgebraic(c, c.r7, x, 5);
  ShiftRightAlgebraic(c, c.r8, x, 0);
  ShiftRightAlgebraic(c, c.r9, x, 31);
  Compare(c.cr0, int32_t(c.r9.u64), 0, c.xer.so);
  c.r10.u64 = x == 0 ? 32 : uint64_t(std::countl_zero(x));
}
void ExpectRlwimi(Ctx& c, const Memory&) {
  const uint32_t x = uint32_t(c.r4.u64);
  uint64_t m = Mask(10 + 32, 20 + 32);
  c.r3.u64 = (Rotate32(x, 5) & m) | (c.r3.u64 & ~m);
  m = Mask(20 + 32, 10 + 32);
  c.r5.u64 = (Rotate32(x, 16) & m) | (c.r5.u64 & ~m);
  m = Mask(32, 63);
  c.r6.u64 = (Rotate32(x, 0) & m) | (c.r6.u64 & ~m);
  Compare(c.cr0, int32_t(c.r6.u64), 0, c.xer.so);
}
void ExpectMultiplyCarry(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  c.r3.u64 = x * uint64_t(-7ll);
  c.r5.u64 = uint64_t(int64_t(int32_t(x)) * int64_t(int32_t(c.r6.u64)));
  c.xer.ca = Carry32(x, uint64_t(-1ll));
  c.r7.u64 = x - 1;
  c.xer.ca = Carry32(x, 5);
  c.r8.u64 = x + 5;
  Compare(c.cr0, int32_t(c.r8.u64), 0, c.xer.so);
  c.xer.ca = Carry32(~x, uint64_t(-3ll), 1);
  c.r9.u64 = uint64_t(-3ll) - x;
  c.xer.ca = Carry32(~x, 100, 1);
  c.r10.u64 = 100 - x;
  const uint64_t ca = c.xer.ca;
  c.xer.ca = Carry32(x, ca);
  c.r11.u64 = x + ca;
}
void ExpectAddze(Ctx& c, const Memory&) {
  uint64_t ca = c.xer.ca;
  c.r3.u64 = c.r4.u64 + ca;
  c.xer.ca = Carry32(c.r4.u64, ca);
  ca = c.xer.ca;
  const uint64_t y = c.r5.u64;
  c.r5.u64 = y + ca;
  c.xer.ca = Carry32(y, ca);
}
void ExpectRotate64(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  c.r3.u64 = std::rotl(x, 32) & Mask(32, 63);
  c.r5.u64 = x & Mask(32, 63);
  c.r6.u64 = std::rotl(x, 1);
  c.r7.u64 = std::rotl(x, 61);
  c.r8.u64 = std::rotl(x, 8) & Mask(0, 55);
  c.r9.u64 = (std::rotl(x, 32) & Mask(0, 31)) | (c.r9.u64 & ~Mask(0, 31));
  c.r10.u64 = std::rotl(x, 4) & Mask(60, 63);
  Compare(c.cr0, int32_t(c.r10.u64), 0, c.xer.so);
}
void ExpectNoEffect(Ctx& c, const Memory&) { c.r3.u64 = 1; }
void ExpectBctrl(Ctx& c, const Memory& m) {
  c.ctr.u64 = c.r4.u64;
  c.lr = 0x10008;
  IndirectCall(c, m, uint32_t(c.ctr.u64));
  c.r3.u64 += 1;
}

// Operand values of the floating-point cases. kSingle and kDouble: frA any edge, frB finite and not zero (no
// operation makes a new NaN; a NaN operand goes through). kFused: values whose products and sums are exact in a
// double, so that one rounding and two agree. kCompare: any edge for both.
const double kAnyEdge[] = {0.0, -0.0, 1.5, -2.25, std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                           double(1e-40f), double(3.0e38f), double(1.0f / 3.0f), double(-1e-45f), 1e-300, 1.0 / 3.0,
                           1e308, 4.9e-324};
const double kFiniteEdge[] = {1.5, -2.25, double(1.0f / 3.0f), double(3.0e38f), double(1e-40f), -1024.5, 1.0 / 3.0, 1e300};
const double kFusedEdge[] = {0.0, -0.0, 1.5, -2.25, 0.125, 1024.5, -3.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()};
constexpr int kSingleEdges = 11, kSingleFinite = 6;

struct Case {
  const char* name;
  Translated translated;
  Expected expected;
  enum R4 { kAnyValue, kPointer, kCount } r4;
  enum Floats { kNoFloats, kSingle, kDouble, kFused, kCompare } floats = kNoFloats;
};

void DrawFloats(Ctx& c, Case::Floats floats, uint32_t s) {
  if (floats == Case::kSingle || floats == Case::kDouble) {
    const int edges = floats == Case::kSingle ? kSingleEdges : int(std::size(kAnyEdge));
    const int finite = floats == Case::kSingle ? kSingleFinite : int(std::size(kFiniteEdge));
    c.f1.f64 = kAnyEdge[s % edges];
    c.f2.f64 = kFiniteEdge[(s / edges) % finite];
  } else if (floats == Case::kCompare) {
    const int n = int(std::size(kAnyEdge));
    c.f1.f64 = kAnyEdge[s % n];
    c.f2.f64 = kAnyEdge[(s / n) % n];
  } else if (floats == Case::kFused) {
    const int n = int(std::size(kFusedEdge));
    c.f1.f64 = kFusedEdge[s % n];
    c.f2.f64 = kFusedEdge[(s / n) % n];
    c.f3.f64 = kFusedEdge[(s / (n * n)) % n];
    if (std::isinf(c.f1.f64) && c.f2.f64 == 0) c.f2.f64 = 1.5;
    if (std::isinf(c.f2.f64) && c.f1.f64 == 0) c.f1.f64 = -2.25;
    if (std::isinf(c.f3.f64) && (std::isinf(c.f1.f64) || std::isinf(c.f2.f64))) c.f3.f64 = 0.125;
  }
}

const Case kCases[] = {
    {"addi_li", &isa::addi_li, &ExpectAddiLi, Case::kAnyValue},
    {"addis_lis", &isa::addis_lis, &ExpectAddisLis, Case::kAnyValue},
    {"lwz", &isa::lwz, &ExpectLwz, Case::kPointer},
    {"stw", &isa::stw, &ExpectStw, Case::kPointer},
    {"stwu", &isa::stwu, &ExpectStwu, Case::kAnyValue},
    {"ld_std", &isa::ld_std, &ExpectLdStd, Case::kPointer},
    {"lfs_fmuls_stfs", &isa::lfs_fmuls_stfs, &ExpectLfsFmulsStfs, Case::kPointer},
    {"compare", &isa::compare, &ExpectCompare, Case::kAnyValue},
    {"rlwinm", &isa::rlwinm, &ExpectRlwinm, Case::kAnyValue},
    {"or_mr", &isa::or_mr, &ExpectOrMr, Case::kAnyValue},
    {"add", &isa::add, &ExpectAdd, Case::kAnyValue},
    {"spr", &isa::spr, &ExpectSpr, Case::kAnyValue},
    {"branch", &isa::branch, &ExpectBranch, Case::kAnyValue},
    {"bdnz", &isa::bdnz, &ExpectCountedLoop, Case::kCount},
    {"beqlr", &isa::beqlr, &ExpectBeqlr, Case::kAnyValue},
    {"conditions", &isa::conditions, &ExpectConditions, Case::kAnyValue},
    {"bnelr", &isa::bnelr, &ExpectConditionalReturn<Eq, false>, Case::kAnyValue},
    {"bltlr", &isa::bltlr, &ExpectConditionalReturn<Lt, true>, Case::kAnyValue},
    {"bgelr", &isa::bgelr, &ExpectConditionalReturn<Lt, false>, Case::kAnyValue},
    {"bgtlr", &isa::bgtlr, &ExpectConditionalReturn<Gt, true>, Case::kAnyValue},
    {"blelr", &isa::blelr, &ExpectConditionalReturn<Gt, false>, Case::kAnyValue},
    {"bdz", &isa::bdz, &ExpectCountedLoop, Case::kCount},
    {"bdnzf", &isa::bdnzf, &ExpectCountAndCondition<false, Eq, false>, Case::kAnyValue},
    {"bdnzt", &isa::bdnzt, &ExpectCountAndCondition<false, Lt, true>, Case::kAnyValue},
    {"bdzf", &isa::bdzf, &ExpectCountAndCondition<true, Eq, false>, Case::kAnyValue},
    {"bdzlr", &isa::bdzlr, &ExpectBdzlr, Case::kAnyValue},
    {"bl_self", &isa::bl_self, &ExpectBlSelf, Case::kCount},
    {"lbz_lhz_stb_sth", &isa::lbz_lhz_stb_sth, &ExpectLbzLhzStbSth, Case::kPointer},
    {"indexed", &isa::indexed, &ExpectIndexed, Case::kPointer},
    {"lfd", &isa::lfd, &ExpectLfd, Case::kPointer},
    {"stfd", &isa::stfd, &ExpectStfd, Case::kPointer, Case::kDouble},
    {"lfsx", &isa::lfsx, &ExpectLfsx, Case::kPointer},
    {"stfsx", &isa::stfsx, &ExpectStfsx, Case::kPointer, Case::kSingle},
    {"stfs", &isa::stfs, &ExpectStfs, Case::kPointer, Case::kSingle},
    {"fmr", &isa::fmr, &ExpectFmr, Case::kAnyValue, Case::kDouble},
    {"fneg", &isa::fneg, &ExpectFneg, Case::kAnyValue, Case::kDouble},
    {"fabs", &isa::fabs, &ExpectFabs, Case::kAnyValue, Case::kDouble},
    {"frsp", &isa::frsp, &ExpectFrsp, Case::kAnyValue, Case::kDouble},
    {"fcmpu", &isa::fcmpu, &ExpectFcmpu, Case::kAnyValue, Case::kCompare},
    {"fadds", &isa::fadds, &ExpectArithmetic<'+', true>, Case::kAnyValue, Case::kSingle},
    {"fsubs", &isa::fsubs, &ExpectArithmetic<'-', true>, Case::kAnyValue, Case::kSingle},
    {"fdivs", &isa::fdivs, &ExpectArithmetic<'/', true>, Case::kAnyValue, Case::kSingle},
    {"fmuls", &isa::fmuls, &ExpectArithmetic<'*', true>, Case::kAnyValue, Case::kSingle},
    {"fadd", &isa::fadd, &ExpectArithmetic<'+', false>, Case::kAnyValue, Case::kDouble},
    {"fsub", &isa::fsub, &ExpectArithmetic<'-', false>, Case::kAnyValue, Case::kDouble},
    {"fmul", &isa::fmul, &ExpectArithmetic<'*', false>, Case::kAnyValue, Case::kDouble},
    {"fdiv", &isa::fdiv, &ExpectArithmetic<'/', false>, Case::kAnyValue, Case::kDouble},
    {"fmadds", &isa::fmadds, &ExpectFused<0, true>, Case::kAnyValue, Case::kFused},
    {"fmsubs", &isa::fmsubs, &ExpectFused<1, true>, Case::kAnyValue, Case::kFused},
    {"fnmsubs", &isa::fnmsubs, &ExpectFused<2, true>, Case::kAnyValue, Case::kFused},
    {"fmadd", &isa::fmadd, &ExpectFused<0, false>, Case::kAnyValue, Case::kFused},
    {"fmsub", &isa::fmsub, &ExpectFused<1, false>, Case::kAnyValue, Case::kFused},
    {"logical_immediate", &isa::logical_immediate, &ExpectLogicalImmediate, Case::kAnyValue},
    {"logical", &isa::logical, &ExpectLogical, Case::kAnyValue},
    {"arithmetic", &isa::arithmetic, &ExpectIntegerArithmetic, Case::kAnyValue},
    {"shifts", &isa::shifts, &ExpectShifts, Case::kAnyValue},
    {"rlwimi", &isa::rlwimi, &ExpectRlwimi, Case::kAnyValue},
    {"multiply_carry", &isa::multiply_carry, &ExpectMultiplyCarry, Case::kAnyValue},
    {"addze", &isa::addze, &ExpectAddze, Case::kAnyValue},
    {"rotate64", &isa::rotate64, &ExpectRotate64, Case::kAnyValue},
    {"no_effect", &isa::no_effect, &ExpectNoEffect, Case::kAnyValue},
    {"bctrl", &isa::bctrl, &ExpectBctrl, Case::kAnyValue},
};

uint64_t Bits(double x) { return std::bit_cast<uint64_t>(x); }

// Where the titles' recompilation, and so the translation, differs from the ISA: each checked against the
// recompilation's value, the ISA's printed beside it.
void KnownDifferences(const Memory& m) {
  Ctx c;
  c.fpscr.csr = _mm_getcsr();
  c.f1.f64 = 641.0, c.f2.f64 = std::ldexp(6700417.0, -56), c.f3.f64 = 1.0;
  RunFrom([&](Ctx& x) { isa::fmadds(x, m); }, c);
  const uint32_t single = std::bit_cast<uint32_t>(float(c.f4.f64));
  CHECK(single == 0x3F800000u);
  std::printf("cpu_isa_tests: fmadds 641 * 6700417 * 2^-56 + 1: %08X (rounded to double, then to single), "
              "the ISA %08X (rounded once)\n", single, 0x3F800001u);
  c = Ctx{};
  c.fpscr.csr = _mm_getcsr();
  volatile double infinity = std::numeric_limits<double>::infinity();
  c.f1.f64 = infinity, c.f2.f64 = -infinity;
  RunFrom([&](Ctx& x) { isa::fadd(x, m); }, c);
  CHECK(Bits(c.f3.f64) == 0xFFF8000000000000ull);
  std::printf("cpu_isa_tests: fadd inf + -inf: %016llX (the host's default NaN), the ISA %016llX\n",
              (unsigned long long)Bits(c.f3.f64), 0x7FF8000000000000ull);
  c = Ctx{};
  c.fpscr.csr = _mm_getcsr();
  c.r4.u64 = kWindow + 0x800, c.f1.f64 = 1.0 / 3.0;
  RunFrom([&](Ctx& x) { isa::stfs(x, m); }, c);
  const uint32_t stored = Load32(m, kWindow + 0x800 + 40);
  const uint64_t f = Bits(1.0 / 3.0);
  const uint32_t truncated = uint32_t((f >> 32) & 0xC0000000u) | uint32_t((f >> 29) & 0x3FFFFFFFu);
  CHECK(stored == std::bit_cast<uint32_t>(float(1.0 / 3.0)));
  std::printf("cpu_isa_tests: stfs of the double 1/3: %08X (rounded to single), the ISA %08X (truncated)\n", stored,
              truncated);
}

}  // namespace

int main() {
  Space translated_space(7), expected_space(7);
  const Memory translated_memory{translated_space.base()}, expected_memory{expected_space.base()};
  constexpr uint32_t kStates = 4096;
  int checked = 0;
  for (const Case& c : kCases) {
    int failed = 0;
    for (uint32_t s = 0; s < kStates; ++s) {
      Ctx input = RandomState(s * 31 + 1);
      static constexpr uint64_t kEdges[] = {0, 0xFFFFFFFFFFFFFFFFull, 0x80000000u, 0x7FFFFFFFu, 1, 0x100000001ull};
      if (s % 8 < 6) input.r4.u64 = kEdges[s % 8];
      if (s % 16 == 7) input.r5.u64 = input.r3.u64;
      if (c.r4 == Case::kPointer) input.r4.u64 = kWindow + 0x800 + (s % 64) * 8;
      if (c.r4 == Case::kCount) input.r4.u64 = 1 + s % 63;
      DrawFloats(input, c.floats, s);
      Ctx translated = input, expected = input;
      RunFrom([&](Ctx& x) { c.translated(x, translated_memory); }, translated);
      RunFrom([&](Ctx& x) { c.expected(x, expected_memory); }, expected);
      const std::string registers = Differences(translated, expected);
      uint32_t where = 0;
      const bool memory = MemoryDiffers(translated_space, expected_space, &where);
      if ((!registers.empty() || memory) && failed++ < 3) {
        std::printf("FAILED %s state %u:%s%s\n", c.name, s, registers.c_str(),
                    memory ? (" memory at " + std::to_string(where)).c_str() : "");
      }
      translated_space.Reset();
      expected_space.Reset();
    }
    if (failed) ++g_failures;
    checked += kStates;
  }
  KnownDifferences(translated_memory);
  translated_space.Reset();
  std::printf("cpu_isa_tests: %zu cases, %d states, %s\n", sizeof kCases / sizeof kCases[0], checked,
              g_failures ? "FAILED" : "all match the ISA");
  return g_failures ? 1 : 0;
}
