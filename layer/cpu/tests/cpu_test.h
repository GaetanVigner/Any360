// What the CPU translator's tests share: register states drawn as a title's data gives them, and the comparison of
// two contexts register by register.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "any360/xdk/ppc_model.h"
#include "cpu_space.h"

namespace any360::cpu::test {

using any360::xdk::Memory;
using any360::xdk::ppc_model::Cr;
using any360::xdk::ppc_model::Ctx;

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++any360::cpu::test::g_failures;                                       \
    }                                                                        \
  } while (0)

inline Ctx RandomState(uint32_t seed) {
  Ctx ctx;
  uint32_t n = 0;
  auto next = [&] { return Hash(n++, seed ^ 0xC0FFEEu); };
  for (int r = 0; r < 32; ++r) ctx.Gpr(r).u64 = DataWord(next());
  for (int r = 0; r < 32; ++r) ctx.Fpr(r).f64 = double(int32_t(next() % 2001) - 1000) / 8.0;
  ctx.r1.u64 = kStackTop - 0x400;
  ctx.r13.u64 = kWindow + 0x100;
  Cr* fields[8] = {&ctx.cr0, &ctx.cr1, &ctx.cr2, &ctx.cr3, &ctx.cr4, &ctx.cr5, &ctx.cr6, &ctx.cr7};
  for (Cr* f : fields) {
    const uint32_t h = next();
    f->lt = h & 1, f->gt = (h >> 1) & 1, f->eq = (h >> 2) & 1, f->so = (h >> 3) & 1;
  }
  ctx.xer.so = next() & 1;
  ctx.lr = DataWord(next());
  ctx.ctr.u64 = DataWord(next());
  const uint32_t flush = any360::xdk::ppc_model::Fpscr::kFlushMask;
  ctx.fpscr.csr = next() & 1 ? (ctx.fpscr.csr | flush) : (ctx.fpscr.csr & ~flush);
  ctx.xer.ca = next() & 1;
  ctx.msr = next();
  return ctx;
}

// Runs f on ctx with the host's floating-point mode the context's (the flush mode it enters with), then puts the
// thread's mode back.
template <typename F>
void RunFrom(F&& f, Ctx& ctx) {
  const uint32_t saved = _mm_getcsr();
  _mm_setcsr(ctx.fpscr.csr);
  f(ctx);
  _mm_setcsr(saved);
}

// The registers where two contexts differ, "" when none.
inline std::string Differences(const Ctx& a, const Ctx& b) {
  std::string out;
  char line[96];
  for (int r = 0; r < 32; ++r) {
    const uint64_t x = const_cast<Ctx&>(a).Gpr(r).u64, y = const_cast<Ctx&>(b).Gpr(r).u64;
    if (x != y) std::snprintf(line, sizeof line, " r%d %016llx/%016llx", r, (unsigned long long)x, (unsigned long long)y), out += line;
  }
  for (int r = 0; r < 32; ++r) {
    const uint64_t x = const_cast<Ctx&>(a).Fpr(r).u64, y = const_cast<Ctx&>(b).Fpr(r).u64;
    if (x != y) std::snprintf(line, sizeof line, " f%d %016llx/%016llx", r, (unsigned long long)x, (unsigned long long)y), out += line;
  }
  const Cr* fa[8] = {&a.cr0, &a.cr1, &a.cr2, &a.cr3, &a.cr4, &a.cr5, &a.cr6, &a.cr7};
  const Cr* fb[8] = {&b.cr0, &b.cr1, &b.cr2, &b.cr3, &b.cr4, &b.cr5, &b.cr6, &b.cr7};
  for (int i = 0; i < 8; ++i) {
    if (fa[i]->lt != fb[i]->lt || fa[i]->gt != fb[i]->gt || fa[i]->eq != fb[i]->eq || fa[i]->so != fb[i]->so) {
      std::snprintf(line, sizeof line, " cr%d %d%d%d%d/%d%d%d%d", i, fa[i]->lt, fa[i]->gt, fa[i]->eq, fa[i]->so, fb[i]->lt,
                    fb[i]->gt, fb[i]->eq, fb[i]->so);
      out += line;
    }
  }
  if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) out += " xer";
  if (a.lr != b.lr) std::snprintf(line, sizeof line, " lr %llx/%llx", (unsigned long long)a.lr, (unsigned long long)b.lr), out += line;
  if (a.ctr.u64 != b.ctr.u64) {
    std::snprintf(line, sizeof line, " ctr %llx/%llx", (unsigned long long)a.ctr.u64, (unsigned long long)b.ctr.u64);
    out += line;
  }
  if (a.fpscr.csr != b.fpscr.csr) std::snprintf(line, sizeof line, " fpscr %x/%x", a.fpscr.csr, b.fpscr.csr), out += line;
  if (a.msr != b.msr) std::snprintf(line, sizeof line, " msr %x/%x", a.msr, b.msr), out += line;
  return out;
}

// The first guest address where the two spaces differ among the pages either touched, or 0 and false.
inline bool MemoryDiffers(Space& a, Space& b, uint32_t* where) {
  for (Space* s : {&a, &b}) {
    const std::vector<uint32_t> pages = s->touched();
    for (uint32_t page : pages) {
      const uint8_t* x = a.Page(page);
      const uint8_t* y = b.Page(page);
      if (std::memcmp(x, y, kPage) != 0) {
        uint32_t i = 0;
        while (x[i] == y[i]) ++i;
        *where = page * kPage + i;
        return true;
      }
    }
  }
  return false;
}

}  // namespace any360::cpu::test
