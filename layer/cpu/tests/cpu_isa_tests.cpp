// The CPU translator's instruction tests: each case of isa_cases.txt, translated by tools/ppc_translate.py at build
// time, against the PowerPC ISA's definition of its instructions (Power ISA 2.06, Book I) written here by hand, on
// many register states and the same guest memory.
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

#include "cpu_isa_cases.h"
#include "cpu_test.h"

using namespace any360::cpu::test;
using any360::xdk::ppc_model::Load32;
using any360::xdk::ppc_model::Load64;
using any360::xdk::ppc_model::Store32;
using any360::xdk::ppc_model::Store64;
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

struct Case {
  const char* name;
  Translated translated;
  Expected expected;
  enum R4 { kAnyValue, kPointer, kCount } r4;
};

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
};

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
  std::printf("cpu_isa_tests: %zu cases, %d states, %s\n", sizeof kCases / sizeof kCases[0], checked,
              g_failures ? "FAILED" : "all match the ISA");
  return g_failures ? 1 : 0;
}
