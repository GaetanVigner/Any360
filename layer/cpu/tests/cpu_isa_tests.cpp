// The CPU translator's instruction tests: each case of isa_cases.txt, translated by tools/ppc_translate.py at build
// time, against the PowerPC ISA's definition of its instructions (Power ISA 2.06, Book I) written here by hand, on
// many register states and the same guest memory. Floating-point cases draw their operands from edge values (zeros,
// infinities, a NaN, denormals, values that round or overflow). The known differences between the titles'
// recompilation, which the translation follows, and the ISA are checked by name at the end.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "cpu_hooks.h"
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
  const double product_sum = kForm == 0 || kForm == 3 ? std::fma(a, cc, b) : std::fma(a, cc, -b);
  const double result = kForm >= 2 ? -product_sum : product_sum;
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
void ExpectBctrWords(Ctx& c, const Memory& m) {
  CompareUnsigned(c.cr6, uint32_t(c.r4.u64), 3, c.xer.so);
  if (c.cr6.gt) {
    c.r3.u64 = ~0ull;
    return;
  }
  c.r12.u64 = 0x20000;
  c.r0.u64 = uint32_t(c.r4.u64) << 2;
  c.r0.u64 = Load32(m, 0x20000 + uint32_t(c.r0.u64));
  c.ctr.u64 = c.r0.u64;
  c.r3.u64 = 10 + (uint32_t(c.ctr.u64) - 0x10020) / 8;
}
void ExpectBctrBytes(Ctx& c, const Memory& m) {
  CompareUnsigned(c.cr6, uint32_t(c.r4.u64), 2, c.xer.so);
  if (c.cr6.gt) {
    c.r3.u64 = ~0ull;
    return;
  }
  c.r0.u64 = uint32_t(Load8(m, 0x20010 + uint32_t(c.r4.u64))) << 2;
  c.r12.u64 = 0x10030 + c.r0.u64;
  c.ctr.u64 = c.r12.u64;
  c.r3.u64 = 20 + (uint32_t(c.ctr.u64) - 0x10030) / 8;
}
void ExpectBctrTail(Ctx& c, const Memory& m) {
  c.ctr.u64 = c.r4.u64;
  IndirectCall(c, m, uint32_t(c.ctr.u64));
}
void ExpectBnectr(Ctx& c, const Memory& m) {
  Compare(c.cr0, int32_t(c.r4.u64), 0, c.xer.so);
  c.ctr.u64 = c.r5.u64;
  if (!c.cr0.eq) {
    IndirectCall(c, m, uint32_t(c.ctr.u64));
    return;
  }
  c.r3.u64 = 1;
}
void ExpectBlrl(Ctx& c, const Memory& m) {
  const uint32_t target = uint32_t(c.r4.u64);
  c.lr = 0x10008;
  IndirectCall(c, m, target);
  c.r3.u64 += 1;
}

// The cases' data lines (jump tables) in a guest space.
void WriteCaseData(const Memory& m) {
  for (const isa::CaseData& d : isa::kCaseData) {
    for (uint32_t i = 0; i < d.count; ++i) Store32(m, d.address + 4 * i, d.words[i]);
  }
}

void SetMsr(Ctx& c, uint64_t source) { c.msr = (uint32_t(source) & 0x8020u) | (c.msr & ~0x8020u); }
void ExpectMsr(Ctx& c, const Memory&) {
  c.r3.u64 = CheckGlobalLock();
  SetMsr(c, c.r13.u64);
  EnterGlobalLock();
  c.r5.u64 = CheckGlobalLock();
  SetMsr(c, c.r3.u64);
  LeaveGlobalLock();
  c.r6.u64 = CheckGlobalLock();
}
void ExpectMtmsrdMask(Ctx& c, const Memory&) {
  SetMsr(c, c.r4.u64);
  LeaveGlobalLock();
}
void ExpectTimebase(Ctx& c, const Memory&) {
  c.r3.u64 = QueryTimebase();
  c.r5.u64 = QueryTimebase() >> 32;
}

// The trap conditions of the TO field: a < b, a > b, a == b signed, a < b, a > b unsigned.
bool TrapHolds(int to, int64_t a, int64_t b, uint64_t ua, uint64_t ub) {
  return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) || ((to & 2) && ua < ub) ||
         ((to & 1) && ua > ub);
}
void TrapWord(Ctx& c, const Memory& m, int to, uint64_t a, uint64_t b) {
  if (TrapHolds(to, int32_t(uint32_t(a)), int32_t(uint32_t(b)), uint32_t(a), uint32_t(b))) Trap(c, m, 0);
}
void TrapDouble(Ctx& c, const Memory& m, int to, uint64_t a, uint64_t b) {
  if (TrapHolds(to, int64_t(a), int64_t(b), a, b)) Trap(c, m, 0);
}
void ExpectTrapImmediate(Ctx& c, const Memory& m) {
  TrapWord(c, m, 6, c.r4.u64, 0);
  TrapWord(c, m, 5, c.r4.u64, ~0ull);
  TrapDouble(c, m, 6, c.r4.u64, 0);
  TrapDouble(c, m, 5, c.r4.u64, ~0ull);
  c.r6.u64 = 1ull << 32;
  TrapWord(c, m, 6, c.r6.u64, 0);
  TrapDouble(c, m, 6, c.r6.u64, 0);
  TrapWord(c, m, 5, c.r6.u64, ~0ull);
  TrapDouble(c, m, 5, c.r6.u64, ~0ull);
}
void ExpectTrapOther(Ctx& c, const Memory& m) {
  Trap(c, m, 22);
  Trap(c, m, 20);
  Trap(c, m, 0);
  TrapWord(c, m, 24, c.r4.u64, 5);
  TrapDouble(c, m, 8, c.r4.u64, 5);
  TrapDouble(c, m, 3, c.r4.u64, uint64_t(-2));
}
void ExpectTrapRegister(Ctx& c, const Memory& m) {
  TrapWord(c, m, 4, c.r4.u64, c.r5.u64);
  TrapWord(c, m, 16, c.r4.u64, c.r5.u64);
  TrapWord(c, m, 3, c.r4.u64, c.r5.u64);
  TrapDouble(c, m, 16, c.r4.u64, c.r5.u64);
  TrapDouble(c, m, 9, c.r4.u64, c.r5.u64);
  Trap(c, m, 0);
  TrapWord(c, m, 4, c.r3.u64, c.r5.u64);
}
void ExpectImports(Ctx& c, const Memory& m) {
  c.lr = 0x10004;
  ImportCall(c, m, "xboxkrnl", 304);
  c.r3.u64 += 1;
  Compare(c.cr0, int32_t(c.r3.u32), 0, c.xer.so);
  ImportCall(c, m, c.cr0.eq ? "xam" : "xboxkrnl", c.cr0.eq ? 65 : 179);
}

uint64_t Exts(uint64_t value, int bits) { return uint64_t(int64_t(value << (64 - bits)) >> (64 - bits)); }

// The update forms: the access at EA = rA + d (rA + rB), then EA into rA.
uint32_t Update(Ctx& c, uint64_t offset) {
  c.r4.u64 += offset;
  return uint32_t(c.r4.u64);
}
void ExpectUpdateLoads(Ctx& c, const Memory& m) {
  c.r3.u64 = Load8(m, Update(c, 1));
  c.r5.u64 = Load16(m, Update(c, 2));
  c.r6.u64 = Exts(Load16(m, Update(c, 2)), 16);
  c.r7.u64 = Load32(m, Update(c, 4));
  c.r8.u64 = Load64(m, Update(c, 8));
}
void ExpectUpdateStores(Ctx& c, const Memory& m) {
  Store8(m, Update(c, 1), uint8_t(c.r3.u64));
  Store16(m, Update(c, 2), uint16_t(c.r5.u64));
  Store64(m, Update(c, 8), c.r6.u64);
}
void ExpectUpdateIndexed(Ctx& c, const Memory& m) {
  c.r9.u64 = 8;
  c.r3.u64 = Load8(m, Update(c, 8));
  c.r5.u64 = Load16(m, Update(c, 8));
  c.r6.u64 = Exts(Load16(m, Update(c, 8)), 16);
  c.r7.u64 = Load32(m, Update(c, 8));
  c.r8.u64 = Exts(Load32(m, Update(c, 8)), 32);
  c.r10.u64 = Load64(m, Update(c, 8));
  Store8(m, Update(c, 8), uint8_t(c.r3.u64));
  Store16(m, Update(c, 8), uint16_t(c.r5.u64));
  Store32(m, Update(c, 8), uint32_t(c.r7.u64));
  Store64(m, Update(c, 8), c.r10.u64);
}
void ExpectAlgebraic(Ctx& c, const Memory& m) {
  c.r3.u64 = Exts(Load16(m, Ea(c.r4.u64, 2)), 16);
  c.r9.u64 = 6;
  c.r5.u64 = Exts(Load16(m, Ea(c.r4.u64, 6)), 16);
  c.r6.u64 = Exts(Load32(m, Ea(c.r4.u64, 8)), 32);
  c.r7.u64 = Exts(Load32(m, Ea(c.r4.u64, 6)), 32);
  c.r8.u64 = Load64(m, Ea(c.r4.u64, 6));
  Store64(m, Ea(c.r4.u64, 6), c.r3.u64);
}
void ExpectByteReverse(Ctx& c, const Memory& m) {
  c.r9.u64 = 4;
  c.r3.u64 = std::byteswap(Load32(m, Ea(c.r4.u64, 4)));
  c.r5.u64 = std::byteswap(Load16(m, Ea(c.r4.u64, 4)));
  Store32(m, Ea(c.r4.u64, 4), std::byteswap(uint32_t(c.r6.u64)));
  c.r10.u64 = 12;
  Store16(m, Ea(c.r4.u64, 12), std::byteswap(uint16_t(c.r7.u64)));
}
// lwarx / ldarx reserve the word they load; stwcx. / stdcx. store while the reservation holds and say so in cr0.eq.
// ctx.reserved, which the ISA does not name, is the reserved word as it lies in guest memory.
void ExpectReservation(Ctx& c, const Memory& m) {
  const uint32_t ea = uint32_t(c.r4.u64);
  const uint32_t word = Load32(m, ea);
  c.reserved.u32 = std::byteswap(word);
  c.r3.u64 = word + 1ull;
  Store32(m, ea, uint32_t(c.r3.u64));
  c.cr0.lt = 0, c.cr0.gt = 0, c.cr0.eq = 1, c.cr0.so = c.xer.so;
  const uint64_t doubleword = Load64(m, ea);
  c.reserved.u64 = std::byteswap(doubleword);
  c.r5.u64 = doubleword + 2;
  Store64(m, ea, c.r5.u64);
  c.cr0.lt = 0, c.cr0.gt = 0, c.cr0.eq = 1, c.cr0.so = c.xer.so;
}
void ExpectCacheZero(Ctx& c, const Memory& m) {
  const uint32_t block = uint32_t(c.r4.u64) & ~31u;
  for (uint32_t i = 0; i < 32; ++i) Store8(m, block + i, 0);
  c.r9.u64 = 256;
  const uint32_t line = uint32_t(c.r4.u64 + 256) & ~127u;
  for (uint32_t i = 0; i < 128; ++i) Store8(m, line + i, 0);
}
void ExpectOrHints(Ctx& c, const Memory&) { c.r3.u64 = 1; }
void ExpectCacheHints(Ctx& c, const Memory&) { c.r3.u64 = 2; }
void ExpectLogicalRest(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r7.u64;
  c.r3.u64 = x & 0x80010000u;
  Compare(c.cr0, int32_t(c.r3.u64), 0, c.xer.so);
  c.r5.u64 = x ^ 0x80010000u;
  c.r6.u64 = x | ~y;
  c.r8.u64 = ~(x ^ y);
  c.r9.u64 = ~(x & y);
  c.r10.u64 = x | ~y;
  Compare(c.cr0, int32_t(c.r10.u64), 0, c.xer.so);
}
uint64_t ShiftDouble(uint64_t value, uint64_t amount, bool left) {
  const uint32_t n = uint32_t(amount & 0x7F);
  if (n >= 64) return 0;
  return left ? value << n : value >> n;
}
void ExpectShift64(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  c.r3.u64 = ShiftDouble(x, 63, true);
  c.r5.u64 = ShiftDouble(x, 64, true);
  c.r6.u64 = ShiftDouble(x, 127, false);
  c.r9.u64 = 1;
  c.r7.u64 = ShiftDouble(x, 1, false);
  c.r8.u64 = ShiftDouble(x, c.r10.u64, true);
  c.r11.u64 = ShiftDouble(x, c.r10.u64, false);
  Compare(c.cr0, int32_t(c.r11.u64), 0, c.xer.so);
}
// sraw / srad: amounts of the width or more give the sign in every bit and CA the sign.
void ShiftWordAlgebraic(Ctx& c, any360::xdk::ppc_model::Reg& target, uint64_t value, uint64_t amount) {
  const int32_t s = int32_t(value);
  const uint32_t n = uint32_t(amount & 0x3F);
  if (n >= 32) {
    target.u64 = uint64_t(int64_t(s >> 31));
    c.xer.ca = s < 0;
    return;
  }
  c.xer.ca = s < 0 && (uint32_t(s) & ((1u << n) - 1)) != 0;
  target.u64 = uint64_t(int64_t(s >> n));
}
void ShiftDoubleAlgebraic(Ctx& c, any360::xdk::ppc_model::Reg& target, uint64_t value, uint64_t amount) {
  const int64_t s = int64_t(value);
  const uint32_t n = uint32_t(amount & 0x7F);
  if (n >= 64) {
    target.u64 = uint64_t(s >> 63);
    c.xer.ca = s < 0;
    return;
  }
  c.xer.ca = s < 0 && (uint64_t(s) & ((1ull << n) - 1)) != 0;
  target.u64 = uint64_t(s >> n);
}
void CaptureCarry(Ctx& c, any360::xdk::ppc_model::Reg& target) {
  const uint64_t ca = c.xer.ca;
  c.xer.ca = uint8_t(Carry32(c.r0.u64, ca));
  target.u64 = c.r0.u64 + ca;
}
void ExpectShiftAlgebraic(Ctx& c, const Memory&) {
  c.r9.u64 = 31;
  ShiftWordAlgebraic(c, c.r3, c.r4.u64, 31);
  CaptureCarry(c, c.r19);
  c.r10.u64 = 32;
  ShiftWordAlgebraic(c, c.r5, c.r6.u64, 32);
  CaptureCarry(c, c.r20);
  c.r11.u64 = 63;
  ShiftWordAlgebraic(c, c.r7, c.r6.u64, 63);
  CaptureCarry(c, c.r21);
  ShiftWordAlgebraic(c, c.r8, c.r6.u64, c.r12.u64);
  Compare(c.cr0, int32_t(c.r8.u64), 0, c.xer.so);
  CaptureCarry(c, c.r22);
  ShiftDoubleAlgebraic(c, c.r13, c.r4.u64, 63);
  CaptureCarry(c, c.r23);
  c.r14.u64 = 64;
  ShiftDoubleAlgebraic(c, c.r15, c.r6.u64, 64);
  CaptureCarry(c, c.r24);
  c.r16.u64 = 127;
  ShiftDoubleAlgebraic(c, c.r17, c.r6.u64, 127);
  CaptureCarry(c, c.r25);
  ShiftDoubleAlgebraic(c, c.r18, c.r4.u64, c.r12.u64);
  Compare(c.cr0, int32_t(c.r18.u64), 0, c.xer.so);
}
void ExpectSrawCa(Ctx& c, const Memory&) {
  c.r10.u64 = 32;
  ShiftWordAlgebraic(c, c.r5, c.r6.u64, 32);
}
void ExpectSradCa(Ctx& c, const Memory&) {
  c.r14.u64 = 64;
  ShiftDoubleAlgebraic(c, c.r15, c.r6.u64, 64);
}
void ExpectSradi(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  ShiftDoubleAlgebraic(c, c.r3, x, 0);
  CaptureCarry(c, c.r8);
  ShiftDoubleAlgebraic(c, c.r5, x, 20);
  CaptureCarry(c, c.r9);
  ShiftDoubleAlgebraic(c, c.r6, x, 63);
  CaptureCarry(c, c.r10);
  ShiftDoubleAlgebraic(c, c.r7, x, 32);
  Compare(c.cr0, int32_t(c.r7.u64), 0, c.xer.so);
}
void ExpectCountRotate(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64;
  c.r3.u64 = x == 0 ? 64 : uint64_t(std::countl_zero(x));
  c.r5.u64 = c.r3.u64;
  Compare(c.cr0, int32_t(c.r5.u64), 0, c.xer.so);
  const int n = int(c.r7.u64 & 31);
  c.r6.u64 = Rotate32(uint32_t(x), n) & Mask(8 + 32, 23 + 32);
  c.r8.u64 = Rotate32(uint32_t(x), n) & Mask(32, 63);
  c.r9.u64 = std::rotl(x, 8) & Mask(16, 63 - 8);
  c.r10.u64 = x & Mask(32, 63);
  Compare(c.cr0, int32_t(c.r10.u64), 0, c.xer.so);
}
// The carrying forms: rD = a + b + carry in, CA the carry out of the low word (as in the titles' 32-bit mode).
void Extended(Ctx& c, any360::xdk::ppc_model::Reg& target, uint64_t a, uint64_t b, uint64_t carry) {
  const uint64_t ca = Carry32(a, b, carry);
  target.u64 = a + b + carry;
  c.xer.ca = uint8_t(ca);
}
void ExpectCarryRest(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r5.u64, ones = ~0ull;
  Extended(c, c.r3, x, y, 0);
  Extended(c, c.r6, x, y, c.xer.ca);
  Extended(c, c.r7, x, ones, c.xer.ca);
  Extended(c, c.r9, ~x, y, c.xer.ca);
  Extended(c, c.r10, ~x, ones, c.xer.ca);
  Extended(c, c.r11, ~x, 0, c.xer.ca);
  Extended(c, c.r13, y, x, c.xer.ca);
  Compare(c.cr0, int32_t(c.r13.u64), 0, c.xer.so);
  Extended(c, c.r8, ~x, y, 1);
  Extended(c, c.r14, ~x, x, c.xer.ca);
  Compare(c.cr0, int32_t(c.r14.u64), 0, c.xer.so);
  Extended(c, c.r12, ~y, x, 1);
  Compare(c.cr0, int32_t(c.r12.u64), 0, c.xer.so);
}
// mulhw / mulhwu: the ISA defines the low word (the product's high word); the high word is undefined, here the low
// word sign-extended (mulhw) or zero-extended (mulhwu).
uint64_t MulHighWord(uint64_t a, uint64_t b) { return Exts(uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b))) >> 32, 32); }
uint64_t MulHighWordUnsigned(uint64_t a, uint64_t b) { return (uint64_t(uint32_t(a)) * uint32_t(b)) >> 32; }
uint64_t MulHigh(uint64_t a, uint64_t b) { return uint64_t((__int128(int64_t(a)) * __int128(int64_t(b))) >> 64); }
uint64_t MulHighUnsigned(uint64_t a, uint64_t b) {
  return uint64_t((static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) >> 64);
}
void ExpectMultiplyHigh(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r5.u64;
  c.r3.u64 = MulHighWord(x, y);
  c.r6.u64 = MulHighWordUnsigned(x, y);
  c.r7.u64 = MulHigh(x, y);
  c.r8.u64 = MulHighUnsigned(x, y);
  c.r9.u64 = x * y;
  c.r10.u64 = MulHighWord(x, y);
  Compare(c.cr0, int32_t(c.r10.u64), 0, c.xer.so);
  c.r11.u64 = ~0ull;
  c.r12.u64 = MulHigh(x, ~0ull);
  c.r13.u64 = MulHighUnsigned(x, ~0ull);
  c.r14.u64 = x * ~0ull;
}
// Division by zero and the most negative value divided by -1 leave rD undefined in the ISA, as does the high word of
// divw / divwu: here 0 and a zero high word.
uint64_t DivWord(uint64_t a, uint64_t b) {
  const int32_t n = int32_t(a), d = int32_t(b);
  if (d == 0 || (n == INT32_MIN && d == -1)) return 0;
  return uint32_t(n / d);
}
uint64_t DivWordUnsigned(uint64_t a, uint64_t b) { return uint32_t(b) ? uint32_t(a) / uint32_t(b) : 0; }
uint64_t DivDouble(uint64_t a, uint64_t b) {
  const int64_t n = int64_t(a), d = int64_t(b);
  if (d == 0 || (n == INT64_MIN && d == -1)) return 0;
  return uint64_t(n / d);
}
uint64_t DivDoubleUnsigned(uint64_t a, uint64_t b) { return b ? a / b : 0; }
void ExpectDivide(Ctx& c, const Memory&) {
  const uint64_t x = c.r4.u64, y = c.r5.u64, minus_one = ~0ull, most_negative = 1ull << 63;
  c.r3.u64 = DivWord(x, y);
  c.r9.u64 = 0;
  c.r6.u64 = DivWord(x, 0);
  c.r10.u64 = minus_one;
  c.r7.u64 = DivWord(x, minus_one);
  c.r8.u64 = DivWordUnsigned(x, y);
  c.r11.u64 = DivWordUnsigned(x, 0);
  c.r12.u64 = DivDouble(x, y);
  c.r13.u64 = DivDouble(x, minus_one);
  c.r14.u64 = most_negative;
  c.r15.u64 = DivDouble(most_negative, minus_one);
  c.r16.u64 = DivDoubleUnsigned(x, y);
  c.r17.u64 = DivDoubleUnsigned(x, 0);
  c.r18.u64 = DivWord(x, y);
  Compare(c.cr0, int32_t(c.r18.u64), 0, c.xer.so);
  c.r19.u64 = DivWordUnsigned(x, y);
  Compare(c.cr0, int32_t(c.r19.u64), 0, c.xer.so);
}
// Condition register bit n (0 the lt bit of cr0) and the whole register as mfcr reads it.
uint8_t& CrBit(Ctx& c, int n) {
  Cr* fields[8] = {&c.cr0, &c.cr1, &c.cr2, &c.cr3, &c.cr4, &c.cr5, &c.cr6, &c.cr7};
  Cr& f = *fields[n / 4];
  switch (n % 4) {
    case 0: return f.lt;
    case 1: return f.gt;
    case 2: return f.eq;
    default: return f.so;
  }
}
uint64_t CrWord(Ctx& c) {
  uint64_t word = 0;
  for (int n = 0; n < 32; ++n) word |= uint64_t(CrBit(c, n)) << (31 - n);
  return word;
}
void SetCrField(Ctx& c, int field, uint64_t value) {
  for (int b = 0; b < 4; ++b) CrBit(c, 4 * field + b) = (value >> (31 - (4 * field + b))) & 1;
}
void ExpectCrLogic(Ctx& c, const Memory&) {
  auto bit = [&](int n) -> uint8_t& { return CrBit(c, n); };
  bit(1) = bit(5) & bit(9);
  bit(2) = bit(6) & !bit(10);
  bit(3) = bit(7) == bit(11);
  bit(12) = !(bit(16) & bit(20));
  bit(13) = !(bit(17) | bit(21));
  bit(14) = bit(18) | bit(22);
  bit(15) = bit(19) | !bit(23);
  bit(24) = bit(25) ^ bit(26);
  c.cr7 = c.cr0;
}
// mfocrf: the ISA leaves the bits outside the named field undefined; here 0.
void ExpectCrMoves(Ctx& c, const Memory&) {
  c.r3.u64 = CrWord(c);
  c.r5.u64 = CrWord(c) & 0x00F00000u;
  SetCrField(c, 0, c.r4.u64);
  SetCrField(c, 7, c.r4.u64);
  c.r7.u64 = CrWord(c);
  for (int f = 0; f < 8; ++f) SetCrField(c, f, c.r6.u64);
  c.xer.so = (c.r4.u64 >> 31) & 1, c.xer.ov = (c.r4.u64 >> 30) & 1, c.xer.ca = (c.r4.u64 >> 29) & 1;
}
void ExpectLfsu(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  c.f1.f64 = double(std::bit_cast<float>(Load32(m, Update(c, 4))));
}
void ExpectLfdu(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  c.f2.u64 = Load64(m, Update(c, 8));
}
void ExpectStfsu(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  Store32(m, Update(c, 4), std::bit_cast<uint32_t>(float(c.f1.f64)));
}
void ExpectStfdu(Ctx& c, const Memory& m) {
  c.fpscr.disableFlushMode();
  Store64(m, Update(c, 8), c.f2.u64);
}
void ExpectLfsux(Ctx& c, const Memory& m) {
  c.r9.u64 = 8;
  c.fpscr.disableFlushMode();
  c.f3.f64 = double(std::bit_cast<float>(Load32(m, Update(c, 8))));
}
void ExpectLfdux(Ctx& c, const Memory& m) {
  c.r9.u64 = 8;
  c.fpscr.disableFlushMode();
  c.f4.u64 = Load64(m, Update(c, 8));
}
void ExpectStfsux(Ctx& c, const Memory& m) {
  c.r9.u64 = 8;
  c.fpscr.disableFlushMode();
  Store32(m, Update(c, 8), std::bit_cast<uint32_t>(float(c.f1.f64)));
}
void ExpectStfdux(Ctx& c, const Memory& m) {
  c.r9.u64 = 8;
  c.fpscr.disableFlushMode();
  Store64(m, Update(c, 8), c.f1.u64);
}
void ExpectLfdx(Ctx& c, const Memory& m) {
  c.r9.u64 = 16;
  c.fpscr.disableFlushMode();
  c.f2.u64 = Load64(m, Ea(c.r4.u64, 16));
}
void ExpectStfdx(Ctx& c, const Memory& m) {
  c.r9.u64 = 16;
  c.fpscr.disableFlushMode();
  Store64(m, Ea(c.r4.u64, 16), c.f1.u64);
}
void ExpectStfiwx(Ctx& c, const Memory& m) {
  c.r9.u64 = 16;
  c.fpscr.disableFlushMode();
  Store32(m, Ea(c.r4.u64, 16), uint32_t(c.f1.u64));
}
void ExpectFcfid(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f3.f64 = double(int64_t(c.f1.u64));
}
// fctiw / fctiwz: the low word is the ISA's (saturated, 0x80000000 for a NaN); the high word, undefined in the ISA,
// is here the low word sign-extended, 0 for a NaN.
template <bool kTruncate>
void ExpectConvertWord(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  const double x = c.f1.f64;
  uint32_t low;
  if (std::isnan(x)) {
    low = 0x80000000u;
  } else {
    const double r = kTruncate ? std::trunc(x) : std::nearbyint(x);
    low = r > 2147483647.0 ? 0x7FFFFFFFu : r < -2147483648.0 ? 0x80000000u : uint32_t(int32_t(r));
  }
  c.f3.u64 = std::isnan(x) ? low : Exts(low, 32);
}
template <bool kTruncate>
void ExpectConvertDouble(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  const double x = c.f1.f64;
  const double r = kTruncate ? std::trunc(x) : std::nearbyint(x);
  c.f3.u64 = std::isnan(x) ? 0x8000000000000000ull
             : r >= 9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFull
             : r < -9223372036854775808.0 ? 0x8000000000000000ull
                                         : uint64_t(int64_t(r));
}
template <bool kSingle>
void ExpectSquareRoot(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  const double root = std::sqrt(c.f1.f64);
  c.f3.f64 = kSingle ? Single(root) : root;
}
// fres / frsqrte: estimates the ISA bounds (1/256 and 1/32 of the result); here the exact result rounded to single.
void ExpectFres(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f3.f64 = Single(1.0 / c.f1.f64);
}
void ExpectFrsqrte(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f3.f64 = Single(1.0 / std::sqrt(c.f1.f64));
}
void ExpectFsel(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f4.f64 = c.f1.f64 >= 0.0 ? c.f2.f64 : c.f3.f64;
}
void ExpectFnabs(Ctx& c, const Memory&) {
  c.fpscr.disableFlushMode();
  c.f3.u64 = c.f1.u64 | (1ull << 63);
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
// kIntegerBits: frA's bits an integer (fcfid). kConvert: values around the conversions' limits and halves, 2^63 left
// out (a known difference). kRoot: zeros, infinity, a NaN, denormals and positive values (no operation makes a new
// NaN). kSelect: frA's signs, zeros and NaN, frC and frB any edge. kNegatedFused: kFused without the NaN.
const uint64_t kIntegerBitsEdge[] = {0, 1, ~0ull, 1ull << 63, ~0ull >> 1, (1ull << 53) + 1, ~(1ull << 53), 0x80000000u,
                                     0x7FFFFFFFu, 0x0123456789ABCDEFull, 0xFFFFFFFF00000001ull};
const double kConvertEdge[] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), 0.0, -0.0, 0.5, -0.5, 1.5, 2.5, -1.5, -2.5,
                               2147483646.5, 2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0, -2147483648.5,
                               -2147483649.0, 9223372036854774784.0, -9223372036854775808.0, -1.8446744073709552e19,
                               1e300, -1e300, 4.9e-324, 1e-310, 123456.789};
const double kRootEdge[] = {0.0, -0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                            4.9e-324, double(1e-40f), 2.0, 0.25, 1e300, double(3.0e38f), 1.0 / 3.0, 1e-300};
const double kSelectEdge[] = {0.0, -0.0, -1.5, 1.5, std::numeric_limits<double>::quiet_NaN(),
                              -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), 4.9e-324,
                              -4.9e-324};

struct Case {
  const char* name;
  Translated translated;
  Expected expected;
  enum R4 { kAnyValue, kPointer, kCount } r4;
  enum Floats { kNoFloats, kSingle, kDouble, kFused, kCompare, kNegatedFused, kIntegerBits, kConvert, kRoot, kSelect };
  Floats floats = kNoFloats;
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
  } else if (floats == Case::kIntegerBits) {
    c.f1.u64 = kIntegerBitsEdge[s % std::size(kIntegerBitsEdge)];
  } else if (floats == Case::kConvert) {
    c.f1.f64 = kConvertEdge[s % std::size(kConvertEdge)];
  } else if (floats == Case::kRoot) {
    c.f1.f64 = kRootEdge[s % std::size(kRootEdge)];
  } else if (floats == Case::kSelect) {
    const uint32_t n = uint32_t(std::size(kAnyEdge)), k = uint32_t(std::size(kSelectEdge));
    c.f1.f64 = kSelectEdge[s % k];
    c.f2.f64 = kAnyEdge[(s / k) % n];
    c.f3.f64 = kAnyEdge[(s / k / n) % n];
  } else if (floats == Case::kFused || floats == Case::kNegatedFused) {
    const int n = int(std::size(kFusedEdge));
    c.f1.f64 = kFusedEdge[s % n];
    c.f2.f64 = kFusedEdge[(s / n) % n];
    c.f3.f64 = kFusedEdge[(s / (n * n)) % n];
    if (std::isinf(c.f1.f64) && c.f2.f64 == 0) c.f2.f64 = 1.5;
    if (std::isinf(c.f2.f64) && c.f1.f64 == 0) c.f1.f64 = -2.25;
    if (std::isinf(c.f3.f64) && (std::isinf(c.f1.f64) || std::isinf(c.f2.f64))) c.f3.f64 = 0.125;
    if (floats == Case::kNegatedFused) {
      for (double* f : {&c.f1.f64, &c.f2.f64, &c.f3.f64}) {
        if (std::isnan(*f)) *f = 0.5;
      }
    }
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
    {"fnmsubs", &isa::fnmsubs, &ExpectFused<2, true>, Case::kAnyValue, Case::kNegatedFused},
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
    {"msr", &isa::msr, &ExpectMsr, Case::kAnyValue},
    {"mtmsrd_mask", &isa::mtmsrd_mask, &ExpectMtmsrdMask, Case::kAnyValue},
    {"timebase", &isa::timebase, &ExpectTimebase, Case::kAnyValue},
    {"trap_immediate", &isa::trap_immediate, &ExpectTrapImmediate, Case::kAnyValue},
    {"trap_other", &isa::trap_other, &ExpectTrapOther, Case::kAnyValue},
    {"trap_register", &isa::trap_register, &ExpectTrapRegister, Case::kAnyValue},
    {"imports", &isa::imports, &ExpectImports, Case::kAnyValue},
    {"bctr_words", &isa::bctr_words, &ExpectBctrWords, Case::kAnyValue},
    {"bctr_bytes", &isa::bctr_bytes, &ExpectBctrBytes, Case::kAnyValue},
    {"bctr_tail", &isa::bctr_tail, &ExpectBctrTail, Case::kAnyValue},
    {"bnectr", &isa::bnectr, &ExpectBnectr, Case::kAnyValue},
    {"blrl", &isa::blrl, &ExpectBlrl, Case::kAnyValue},
    {"update_loads", &isa::update_loads, &ExpectUpdateLoads, Case::kPointer},
    {"update_stores", &isa::update_stores, &ExpectUpdateStores, Case::kPointer},
    {"update_indexed", &isa::update_indexed, &ExpectUpdateIndexed, Case::kPointer},
    {"algebraic", &isa::algebraic, &ExpectAlgebraic, Case::kPointer},
    {"byte_reverse", &isa::byte_reverse, &ExpectByteReverse, Case::kPointer},
    {"reservation", &isa::reservation, &ExpectReservation, Case::kPointer},
    {"cache_zero", &isa::cache_zero, &ExpectCacheZero, Case::kPointer},
    {"cache_hints", &isa::cache_hints, &ExpectCacheHints, Case::kAnyValue},
    {"or_hints", &isa::or_hints, &ExpectOrHints, Case::kAnyValue},
    {"logical_rest", &isa::logical_rest, &ExpectLogicalRest, Case::kAnyValue},
    {"shift64", &isa::shift64, &ExpectShift64, Case::kAnyValue},
    {"shift_algebraic", &isa::shift_algebraic, &ExpectShiftAlgebraic, Case::kAnyValue},
    {"sraw_ca", &isa::sraw_ca, &ExpectSrawCa, Case::kAnyValue},
    {"srad_ca", &isa::srad_ca, &ExpectSradCa, Case::kAnyValue},
    {"sradi", &isa::sradi, &ExpectSradi, Case::kAnyValue},
    {"count_rotate", &isa::count_rotate, &ExpectCountRotate, Case::kAnyValue},
    {"carry_rest", &isa::carry_rest, &ExpectCarryRest, Case::kAnyValue},
    {"multiply_high", &isa::multiply_high, &ExpectMultiplyHigh, Case::kAnyValue},
    {"divide", &isa::divide, &ExpectDivide, Case::kAnyValue},
    {"cr_logic", &isa::cr_logic, &ExpectCrLogic, Case::kAnyValue},
    {"cr_moves", &isa::cr_moves, &ExpectCrMoves, Case::kAnyValue},
    {"lfsu", &isa::lfsu, &ExpectLfsu, Case::kPointer},
    {"lfdu", &isa::lfdu, &ExpectLfdu, Case::kPointer},
    {"stfsu", &isa::stfsu, &ExpectStfsu, Case::kPointer, Case::kSingle},
    {"stfdu", &isa::stfdu, &ExpectStfdu, Case::kPointer, Case::kDouble},
    {"lfsux", &isa::lfsux, &ExpectLfsux, Case::kPointer},
    {"lfdux", &isa::lfdux, &ExpectLfdux, Case::kPointer},
    {"stfsux", &isa::stfsux, &ExpectStfsux, Case::kPointer, Case::kSingle},
    {"stfdux", &isa::stfdux, &ExpectStfdux, Case::kPointer, Case::kDouble},
    {"lfdx", &isa::lfdx, &ExpectLfdx, Case::kPointer},
    {"stfdx", &isa::stfdx, &ExpectStfdx, Case::kPointer, Case::kDouble},
    {"stfiwx", &isa::stfiwx, &ExpectStfiwx, Case::kPointer, Case::kIntegerBits},
    {"fcfid", &isa::fcfid, &ExpectFcfid, Case::kAnyValue, Case::kIntegerBits},
    {"fctiw", &isa::fctiw, &ExpectConvertWord<false>, Case::kAnyValue, Case::kConvert},
    {"fctiwz", &isa::fctiwz, &ExpectConvertWord<true>, Case::kAnyValue, Case::kConvert},
    {"fctid", &isa::fctid, &ExpectConvertDouble<false>, Case::kAnyValue, Case::kConvert},
    {"fctidz", &isa::fctidz, &ExpectConvertDouble<true>, Case::kAnyValue, Case::kConvert},
    {"fsqrt", &isa::fsqrt, &ExpectSquareRoot<false>, Case::kAnyValue, Case::kRoot},
    {"fsqrts", &isa::fsqrts, &ExpectSquareRoot<true>, Case::kAnyValue, Case::kRoot},
    {"fres", &isa::fres, &ExpectFres, Case::kAnyValue, Case::kRoot},
    {"frsqrte", &isa::frsqrte, &ExpectFrsqrte, Case::kAnyValue, Case::kRoot},
    {"fsel", &isa::fsel, &ExpectFsel, Case::kAnyValue, Case::kSelect},
    {"fnabs", &isa::fnabs, &ExpectFnabs, Case::kAnyValue, Case::kDouble},
    {"fnmadd", &isa::fnmadd, &ExpectFused<3, false>, Case::kAnyValue, Case::kNegatedFused},
    {"fnmadds", &isa::fnmadds, &ExpectFused<3, true>, Case::kAnyValue, Case::kNegatedFused},
    {"fnmsub", &isa::fnmsub, &ExpectFused<2, false>, Case::kAnyValue, Case::kNegatedFused},
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
  c = Ctx{};
  c.msr = 0, c.r4.u64 = 0xFFFFFFFFu;
  ResetHooks(0);
  isa::mtmsrd_mask(c, m);
  CHECK(c.msr == 0x8020u);
  CHECK(g_lock_depth == -1);
  std::printf("cpu_isa_tests: mtmsrd r4,1 of 0xFFFFFFFF: msr %X (EE and IR) and the global lock released (any register "
              "but r13), the ISA %X (EE and RI), the lock's state not an MSR bit\n",
              c.msr, 0x8002u);
  const std::pair<const char*, Translated> wide_converts[] = {{"fctidz", &isa::fctidz}, {"fctid", &isa::fctid}};
  for (const auto& [name, convert] : wide_converts) {
    c = Ctx{};
    c.fpscr.csr = _mm_getcsr();
    c.f1.f64 = 9223372036854775808.0;
    RunFrom([&](Ctx& x) { convert(x, m); }, c);
    CHECK(c.f3.u64 == 0x8000000000000000ull);
    std::printf("cpu_isa_tests: %s of 2^63: %016llX, the ISA %016llX (saturated)\n", name,
                (unsigned long long)c.f3.u64, 0x7FFFFFFFFFFFFFFFull);
  }
  c = Ctx{};
  c.r6.u64 = 0x80000000u;
  isa::sraw_ca(c, m);
  CHECK(c.xer.ca == 0 && c.r5.u64 == ~0ull);
  std::printf("cpu_isa_tests: sraw of 0x80000000 by 32: CA %d (the amount taken as 31), the ISA 1\n", c.xer.ca);
  c = Ctx{};
  c.r6.u64 = 1ull << 63;
  isa::srad_ca(c, m);
  CHECK(c.xer.ca == 0 && c.r15.u64 == ~0ull);
  std::printf("cpu_isa_tests: srad of 2^63 by 64: CA %d (the amount taken as 63), the ISA 1\n", c.xer.ca);
  c = Ctx{};
  c.fpscr.csr = _mm_getcsr();
  c.f1.f64 = std::numeric_limits<double>::quiet_NaN(), c.f2.f64 = 1.0, c.f3.f64 = 1.0;
  RunFrom([&](Ctx& x) { isa::fnmadds(x, m); }, c);
  CHECK(Bits(c.f4.f64) == 0xFFF8000000000000ull);
  std::printf("cpu_isa_tests: fnmadds of a NaN frA 7FF8000000000000: %016llX (negated), the ISA %016llX (the NaN)\n",
              (unsigned long long)Bits(c.f4.f64), 0x7FF8000000000000ull);
  c = Ctx{};
  c.fpscr.csr = _mm_getcsr();
  volatile double minus_one = -1.0;
  c.f1.f64 = minus_one;
  RunFrom([&](Ctx& x) { isa::fsqrt(x, m); }, c);
  CHECK(Bits(c.f3.f64) == 0xFFF8000000000000ull);
  std::printf("cpu_isa_tests: fsqrt of -1: %016llX (the host's default NaN), the ISA %016llX\n",
              (unsigned long long)Bits(c.f3.f64), 0x7FF8000000000000ull);
  c = Ctx{};
  c.r4.u64 = kWindow + 0x900, c.r3.u64 = 0x12345678u;
  std::memcpy(&c.reserved.u32, any360::xdk::ppc_model::Raw(m, kWindow + 0x900), 4);
  const uint32_t before = Load32(m, kWindow + 0x900);
  isa::stwcx_alone(c, m);
  const uint32_t after = Load32(m, kWindow + 0x900);
  CHECK(c.cr0.eq == 1 && after == 0x12345678u);
  std::printf("cpu_isa_tests: stwcx. with no lwarx before it, the word %08X equal to the context's reserved value: "
              "stores (%08X, eq %d), the ISA does not (no reservation, eq 0)\n", before, after, c.cr0.eq);
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
      ResetHooks(s);
      WriteCaseData(translated_memory);
      WriteCaseData(expected_memory);
      RunFrom([&](Ctx& x) { c.translated(x, translated_memory); }, translated);
      const uint64_t translated_hooks = g_hook_trace;
      ResetHooks(s);
      RunFrom([&](Ctx& x) { c.expected(x, expected_memory); }, expected);
      const std::string registers = Differences(translated, expected);
      uint32_t where = 0;
      const bool memory = MemoryDiffers(translated_space, expected_space, &where);
      const bool hooks = translated_hooks != g_hook_trace;
      if ((!registers.empty() || memory || hooks) && failed++ < 3) {
        std::printf("FAILED %s state %u:%s%s%s\n", c.name, s, registers.c_str(),
                    memory ? (" memory at " + std::to_string(where)).c_str() : "", hooks ? " runtime hooks" : "");
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
