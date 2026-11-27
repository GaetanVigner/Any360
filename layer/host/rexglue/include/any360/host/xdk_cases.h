// Self-test cases (d3d_selftest.h) for library functions Any360 replaces outside the D3D runtime: the C runtime's
// helpers on a double's bits, wcscmp and XAudio's fills. A title registers the ones it binds with ANY360_D3D_CASE and
// its own addresses, the same as the D3D cases; they run its recompiled function and the replacement on the same
// generated inputs, compare the scratch, the stack above the call and the registers named below, and time both.
// For functions a title's own runs do not call (any360_xdk_verify then sees none).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <random>

#include "any360/xdk/crt.h"
#include "any360/xdk/xaudio.h"
#include "d3d_selftest.h"

namespace any360::host::rexglue::d3d {
namespace xdk_case {

// Doubles as the helpers get them: zeros, infinities, quiet and signalling NaNs, denormals, the largest and smallest
// normals, either sign; else any 64 bits, or a normal value's bits.
inline uint64_t Double(std::mt19937_64& rng) {
  const uint64_t sign = (rng() & 1) ? 0x8000000000000000ull : 0;
  switch (rng() % 12) {
    case 0: return sign;
    case 1: return sign | 0x7FF0000000000000ull;
    case 2: return sign | 0x7FF8000000000000ull | (rng() & 0x0007FFFFFFFFFFFFull);
    case 3: return sign | 0x7FF0000000000000ull | (1 + rng() % 0x0007FFFFFFFFFFFFull);
    case 4: return sign | (1 + rng() % 0x000FFFFFFFFFFFFFull);
    case 5: return sign | 0x7FEFFFFFFFFFFFFFull;
    case 6: return sign | 0x0010000000000000ull;
    case 7:
    case 8: return rng();
    default: return std::bit_cast<uint64_t>(std::ldexp(double(rng() % 1000000) / 1000.0, int(rng() % 200) - 100)) | sign;
  }
}
// Floats as XAudio's blocks hold them: the same shapes in 32 bits.
inline uint32_t Float(std::mt19937_64& rng) {
  const uint32_t sign = (rng() & 1) ? 0x80000000u : 0;
  switch (rng() % 10) {
    case 0: return sign;
    case 1: return sign | 0x7F800000u;
    case 2: return sign | 0x7FC00000u | uint32_t(rng() & 0x3FFFFF);
    case 3: return sign | 0x7F800000u | uint32_t(1 + rng() % 0x3FFFFF);
    case 4: return sign | uint32_t(1 + rng() % 0x7FFFFF);
    case 5:
    case 6: return uint32_t(rng());
    default: return std::bit_cast<uint32_t>(float(rng() % 20001) / 10000.0f - 1.0f);
  }
}
inline void Put32(CallSetup& s, uint32_t address, uint32_t value) {
  uint8_t* p = s.Host(address);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * (3 - i)));
}
inline void Put16(CallSetup& s, uint32_t address, uint16_t value) {
  uint8_t* p = s.Host(address);
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}
inline const char* F1Differs(const PPCContext& a, const PPCContext& b) { return a.f1.u64 != b.f1.u64 ? "f1" : nullptr; }
inline const char* F0Differs(const PPCContext& a, const PPCContext& b) { return a.f0.u64 != b.f0.u64 ? "f0" : nullptr; }

}  // namespace xdk_case

// _finite, _isnan, _sptype (crt.h): f1 the double, the result in r3; each stores f1 at r1 + 16.
template <bool (*Function)(const any360::xdk::Memory&, uint32_t, uint64_t, uint64_t&)>
struct CrtDoubleToR3 {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint64_t r3;
    if (!Function(SelfTestMemory(), ctx.r1.u32, ctx.f1.u64, r3)) return false;
    ctx.fpscr.disableFlushMode();
    ctx.r3.u64 = r3;
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) { ctx.f1.u64 = xdk_case::Double(*s.rng); }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
};
// _add_exp (crt.h): f1 the double, r4 what to add to its exponent, the result in f1.
struct CrtAddExpCase {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint64_t f1;
    if (!any360::xdk::CrtAddExp(SelfTestMemory(), ctx.r1.u32, ctx.f1.u64, ctx.r4.u32, f1)) return false;
    ctx.fpscr.disableFlushMode();
    ctx.f1.u64 = f1;
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    ctx.f1.u64 = xdk_case::Double(rng);
    switch (rng() % 4) {
      case 0: ctx.r4.u64 = rng() % 64; break;
      case 1: ctx.r4.u64 = uint64_t(-int64_t(rng() % 64)); break;
      case 2: ctx.r4.u64 = rng() % 4096; break;
      default: ctx.r4.u64 = rng(); break;
    }
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &xdk_case::F1Differs;
};
// wcscmp (crt.h): two strings of big-endian 16-bit characters at r3 and r4, the sign of the first difference in r3.
// Equal strings, one a prefix of the other, a difference anywhere, characters past 0x7FFF, the same address.
struct CrtWcscmpCase {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint64_t r3;
    if (!any360::xdk::CrtWcscmp(SelfTestMemory(), ctx.r3.u32, ctx.r4.u32, r3)) return false;
    ctx.r3.u64 = r3;
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t length = rng() % 4 ? uint32_t(rng() % 24) : uint32_t(rng() % 600);
    const uint32_t left = s.scratch + 0x1000 + 2 * uint32_t(rng() % 64);
    const uint32_t right = s.scratch + 0x8000 + 2 * uint32_t(rng() % 64);
    const auto character = [&] {
      return uint16_t(rng() % 8 ? 1 + rng() % 0x7E : 1 + rng() % 0xFFFF);
    };
    for (uint32_t i = 0; i < length; ++i) {
      const uint16_t c = character();
      xdk_case::Put16(s, left + 2 * i, c);
      xdk_case::Put16(s, right + 2 * i, c);
    }
    xdk_case::Put16(s, left + 2 * length, 0);
    xdk_case::Put16(s, right + 2 * length, 0);
    switch (rng() % 5) {
      case 0: break;
      case 1: xdk_case::Put16(s, right + 2 * uint32_t(rng() % (length + 1)), character()); break;
      case 2: xdk_case::Put16(s, left + 2 * uint32_t(rng() % (length + 1)), character()); break;
      case 3: xdk_case::Put16(s, (rng() & 1 ? left : right) + 2 * length, character()); break;
      default: break;
    }
    ctx.r3.u64 = left;
    ctx.r4.u64 = rng() % 16 ? right : left;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
};
// XAudio's fill of a block's `Floats` floats (xaudio.h): r3 the block (its float at +4, the buffer from +8), f0 left
// the value as a double.
template <uint32_t Floats>
struct XAudioFillCase {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint32_t bits;
    if (!any360::xdk::XAudioFill(SelfTestMemory(), ctx.r3.u32, Floats, bits)) return false;
    ctx.fpscr.disableFlushMode();
    ctx.f0.f64 = double(std::bit_cast<float>(bits));
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    static_assert(8 + 4 * Floats + 0x1000 < kScratchBytes);
    const uint32_t block = s.scratch + 4 * uint32_t(rng() % 1024);
    xdk_case::Put32(s, block + 4, xdk_case::Float(rng));
    ctx.r3.u64 = block;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &xdk_case::F0Differs;
};

}  // namespace any360::host::rexglue::d3d
