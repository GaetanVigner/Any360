// What the reference models that tools/ppc_model.py writes run on: the PowerPC registers as the recompiled code has
// them, the guest memory as any360/xdk/crt.h's Memory (big-endian words), the vector unit's helpers. A model so made is
// a title's recompiled functions turned into code that needs neither the runtime nor the title: the same statements,
// on a context of their own. It is for the functions too long to transcribe by hand (an effect's whole chain); the
// native ones are tested against it, and the title's own code against them in game.
//
// The semantics follow the recompiler's (ReXGlue): a condition field's compare, the host's flush-to-zero mode as the
// floating-point status word's flush bit, vmaddfp rounded once (FMA3 when the processor has it, else the same result
// from doubles rounded to odd), the save and restore helpers of the PowerPC ABI (__savegprlr_N stores r_N..r31 below
// r1 and lr at r1 - 8; __savefpr_N stores f_N..f31 below r12).
#pragma once

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <emmintrin.h>
#include <tmmintrin.h>

#include "any360/xdk/crt.h"

namespace any360::xdk::ppc_model {

union Reg {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

union alignas(16) VReg {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

struct Xer {
  uint8_t so = 0, ov = 0, ca = 0;
};

struct Cr {
  uint8_t lt = 0, gt = 0, eq = 0;
  union {
    uint8_t so = 0;
    uint8_t un;
  };
  template <typename T>
  void compare(T left, T right, const Xer& xer) {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }
  void compare(double left, double right) {
    un = std::isnan(left) || std::isnan(right);
    lt = !un && left < right;
    gt = !un && left > right;
    eq = !un && left == right;
  }
};

// The flush bit (MXCSR's flush-to-zero and denormals-are-zero), set and cleared as the recompiled code does.
struct Fpscr {
  static constexpr uint32_t kFlushMask = 0x8040;
  uint32_t csr = _mm_getcsr();
  void enableFlushMode() {
    if ((csr & kFlushMask) != kFlushMask) {
      csr |= kFlushMask;
      _mm_setcsr(csr);
    }
  }
  void disableFlushMode() {
    if ((csr & kFlushMask) != 0) {
      csr &= ~kFlushMask;
      _mm_setcsr(csr);
    }
  }
};

#define ANY360_PPC_MODEL_REGS(X)                                                                                     \
  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19)     \
      X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

struct Ctx {
#define ANY360_PPC_MODEL_GPR(n) Reg r##n{.u64 = 0};
#define ANY360_PPC_MODEL_FPR(n) Reg f##n{.u64 = 0};
  ANY360_PPC_MODEL_REGS(ANY360_PPC_MODEL_GPR)
  ANY360_PPC_MODEL_REGS(ANY360_PPC_MODEL_FPR)
#undef ANY360_PPC_MODEL_GPR
#undef ANY360_PPC_MODEL_FPR
  VReg v0{.u64 = {}}, v1{.u64 = {}}, v2{.u64 = {}}, v3{.u64 = {}}, v4{.u64 = {}}, v5{.u64 = {}}, v6{.u64 = {}},
      v7{.u64 = {}}, v8{.u64 = {}}, v9{.u64 = {}}, v10{.u64 = {}}, v11{.u64 = {}}, v12{.u64 = {}}, v13{.u64 = {}},
      v14{.u64 = {}}, v15{.u64 = {}}, v16{.u64 = {}},
      v17{.u64 = {}}, v18{.u64 = {}}, v19{.u64 = {}}, v20{.u64 = {}}, v21{.u64 = {}}, v22{.u64 = {}},
      v23{.u64 = {}}, v24{.u64 = {}}, v25{.u64 = {}}, v26{.u64 = {}}, v27{.u64 = {}}, v28{.u64 = {}},
      v29{.u64 = {}}, v30{.u64 = {}}, v31{.u64 = {}};
  Cr cr0{}, cr1{}, cr2{}, cr3{}, cr4{}, cr5{}, cr6{}, cr7{};
  Xer xer{};
  Fpscr fpscr{};
  uint64_t lr = 0, ctr = 0;

  Reg& Gpr(int n);
  Reg& Fpr(int n);
};
// By member, not by pointer arithmetic: a context local to a call stays in host registers.
#define ANY360_PPC_MODEL_GPR(n) &Ctx::r##n,
#define ANY360_PPC_MODEL_FPR(n) &Ctx::f##n,
inline constexpr Reg Ctx::*kGprs[32] = {ANY360_PPC_MODEL_REGS(ANY360_PPC_MODEL_GPR)};
inline constexpr Reg Ctx::*kFprs[32] = {ANY360_PPC_MODEL_REGS(ANY360_PPC_MODEL_FPR)};
#undef ANY360_PPC_MODEL_GPR
#undef ANY360_PPC_MODEL_FPR
inline Reg& Ctx::Gpr(int n) { return this->*kGprs[n]; }
inline Reg& Ctx::Fpr(int n) { return this->*kFprs[n]; }
static_assert(sizeof(Reg) == 8);

inline uint8_t* Raw(const Memory& m, uint32_t address) { return m.Host(address); }
inline uint8_t Load8(const Memory& m, uint32_t a) { return *m.Host(a); }
inline uint16_t Load16(const Memory& m, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, m.Host(a), 2);
  return std::byteswap(v);
}
inline uint32_t Load32(const Memory& m, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, m.Host(a), 4);
  return std::byteswap(v);
}
inline uint64_t Load64(const Memory& m, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, m.Host(a), 8);
  return std::byteswap(v);
}
inline void Store8(const Memory& m, uint32_t a, uint8_t v) { *m.Host(a) = v; }
inline void Store16(const Memory& m, uint32_t a, uint16_t v) {
  v = std::byteswap(v);
  std::memcpy(m.Host(a), &v, 2);
}
inline void Store32(const Memory& m, uint32_t a, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(m.Host(a), &v, 4);
}
inline void Store64(const Memory& m, uint32_t a, uint64_t v) {
  v = std::byteswap(v);
  std::memcpy(m.Host(a), &v, 8);
}

// The PowerPC ABI's register save and restore helpers.
inline void SaveGprLr(Ctx& ctx, const Memory& m, int first) {
  for (int i = first; i < 32; ++i) Store64(m, ctx.r1.u32 - 8 * (32 - i) - 8, ctx.Gpr(i).u64);
  Store32(m, ctx.r1.u32 - 8, ctx.r12.u32);
}
inline void RestGprLr(Ctx& ctx, const Memory& m, int first) {
  for (int i = first; i < 32; ++i) ctx.Gpr(i).u64 = Load64(m, ctx.r1.u32 - 8 * (32 - i) - 8);
  ctx.r12.u64 = Load32(m, ctx.r1.u32 - 8);
  ctx.lr = ctx.r12.u64;
}
inline void SaveFpr(Ctx& ctx, const Memory& m, int first) {
  for (int i = first; i < 32; ++i) Store64(m, ctx.r12.u32 - 8 * (32 - i), ctx.Fpr(i).u64);
}
inline void RestFpr(Ctx& ctx, const Memory& m, int first) {
  for (int i = first; i < 32; ++i) ctx.Fpr(i).u64 = Load64(m, ctx.r12.u32 - 8 * (32 - i));
}

// The byte masks the recompiled code shuffles unaligned vector loads and stores with (lvlx / lvrx and the like).
alignas(16) inline constexpr uint8_t VectorMaskL[256] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};
alignas(16) inline constexpr uint8_t VectorMaskR[256] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

__attribute__((target("ssse3"))) inline __m128i ShuffleEpi8(__m128i a, __m128i mask) {
  return _mm_shuffle_epi8(a, mask);
}

// vmaddfp: a * b + c rounded once, as the recompiler's: FMA3 when the processor has it, else exact double products,
// the error of the double sum, and that sum rounded to odd before it is rounded to float (the same bits).
namespace detail {
inline __m128d FusedToOdd(__m128d a, __m128d b, __m128d c) {
  const __m128d product = _mm_mul_pd(a, b);
  const __m128d sum = _mm_add_pd(product, c);
  const __m128d c_part = _mm_sub_pd(sum, product);
  const __m128d error = _mm_add_pd(_mm_sub_pd(product, _mm_sub_pd(sum, c_part)), _mm_sub_pd(c, c_part));
  const __m128i inexact =
      _mm_castpd_si128(_mm_andnot_pd(_mm_cmpeq_pd(error, _mm_setzero_pd()), _mm_cmpord_pd(error, error)));
  const __m128i bits = _mm_castpd_si128(sum);
  const __m128i one = _mm_set1_epi64x(1);
  const __m128i low_bit = _mm_and_si128(bits, one);
  // even: the last bit is 0 (64-bit compare from two 32-bit ones; the high half of (bits & 1) is 0)
  const __m128i even32 = _mm_cmpeq_epi32(low_bit, _mm_setzero_si128());
  const __m128i even = _mm_and_si128(even32, _mm_shuffle_epi32(even32, _MM_SHUFFLE(2, 3, 0, 1)));
  const __m128i opposite = _mm_srli_epi64(_mm_xor_si128(bits, _mm_castpd_si128(error)), 63);
  const __m128i step = _mm_sub_epi64(one, _mm_slli_epi64(opposite, 1));
  return _mm_castsi128_pd(_mm_add_epi64(bits, _mm_and_si128(step, _mm_and_si128(inexact, even))));
}
inline __m128 EmulatedVmaddfp(__m128 a, __m128 b, __m128 c) {
  const __m128d low = FusedToOdd(_mm_cvtps_pd(a), _mm_cvtps_pd(b), _mm_cvtps_pd(c));
  const __m128d high =
      FusedToOdd(_mm_cvtps_pd(_mm_movehl_ps(a, a)), _mm_cvtps_pd(_mm_movehl_ps(b, b)), _mm_cvtps_pd(_mm_movehl_ps(c, c)));
  return _mm_movelh_ps(_mm_cvtpd_ps(low), _mm_cvtpd_ps(high));
}
inline bool HostHasFma() {
  uint32_t regs[4];
  __asm__("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(1u), "c"(0u));
  constexpr uint32_t kFmaOsxsaveAvx = (1u << 12) | (1u << 27) | (1u << 28);
  if ((regs[2] & kFmaOsxsaveAvx) != kFmaOsxsaveAvx) return false;
  uint32_t xcr0[2];
  __asm__("xgetbv" : "=a"(xcr0[0]), "=d"(xcr0[1]) : "c"(0u));
  return (xcr0[0] & 6u) == 6u;
}
inline const bool kHostHasFma = HostHasFma();
}  // namespace detail

inline __m128 Vmaddfp(__m128 a, __m128 b, __m128 c) {
  if (detail::kHostHasFma) {
    __asm__("vfmadd213ps %2, %1, %0" : "+x"(a) : "x"(b), "x"(c));
    return a;
  }
  return detail::EmulatedVmaddfp(a, b, c);
}

}  // namespace any360::xdk::ppc_model
