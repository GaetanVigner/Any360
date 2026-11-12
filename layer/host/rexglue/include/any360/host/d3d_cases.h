// The D3D self-test's cases (d3d_selftest.h): for each D3D function Any360 replaces, its replacement on the
// registers of a call and the inputs of one call, generated (random within what the titles pass). A title registers
// the cases of the functions it binds with ANY360_D3D_CASE and its own addresses and device offsets.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>

#include "any360/xdk/d3d.h"
#include "d3d_selftest.h"
#include "xdk_natives.h"

namespace any360::host::rexglue::d3d {

const any360::xdk::Memory& SelfTestMemory();

// Big-endian words into guest memory.
inline void Put32(CallSetup& s, uint32_t address, uint32_t value) {
  uint8_t* p = s.Host(address);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * (3 - i)));
}

// A mask of the shapes the runtime gets: one bit, one run, all set, sparse, random. Never 0: the runtime's callers
// never pass one, and its upload functions would loop through memory for ever on it.
inline uint64_t RandomMask(std::mt19937_64& rng) {
  uint64_t m = 0;
  switch (rng() % 6) {
    case 0: m = uint64_t(1) << (rng() % 64); break;
    case 1: m = ~uint64_t(0); break;
    case 2: m = ~uint64_t(0) << (rng() % 64); break;
    case 3: {
      const uint32_t low = uint32_t(rng() % 64), length = 1 + uint32_t(rng() % (64 - low));
      m = (length == 64 ? ~uint64_t(0) : ((uint64_t(1) << length) - 1)) << low;
      break;
    }
    case 4: m = rng() & rng(); break;
    default: m = rng(); break;
  }
  return m ? m : 1;
}

// D3D's copy of the title's shader constants into the device (xdk::D3dSetShaderConstants).
struct SetShaderConstants {
  static bool Native(PPCContext& ctx, uint8_t*) {
    return any360::xdk::D3dSetShaderConstants(SelfTestMemory(), ctx.r3.u64, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                              ctx.r7.u64, ctx.r1.u32);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = rng() % 256;                                   // the first register
    ctx.r5.u64 = s.scratch + 0x100 + uint32_t(rng() % 4096);    // any alignment
    ctx.r6.u64 = rng() % 4 == 0 ? rng() % 64 : rng() % 9;       // most copies are a few vectors
    ctx.r7.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
};

// D3D's upload of the dirty shader constants (xdk::D3dDirtyConstants / D3dDirtyWords): the device's command buffer
// pointer and end (at kCursor) set into the scratch, so the call writes there and nowhere in the title's own buffer.
// Always room: out of room, the runtime's own code submits the buffer to the ring (the replacement declines those
// runs: the unit tests cover it).
inline const char* R3R4R5Differ(const PPCContext& a, const PPCContext& b) {
  if (a.r3.u64 != b.r3.u64) return "r3";
  if (a.r4.u64 != b.r4.u64) return "r4";
  if (a.r5.u64 != b.r5.u64) return "r5";
  return nullptr;
}
template <uint32_t kCursor, bool kWords>
struct DirtyUpload {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint64_t index = ctx.r5.u64, cursor = 0;
    const bool taken = kWords ? any360::xdk::D3dDirtyWords(SelfTestMemory(), ctx.r3.u32, kCursor, ctx.r4.u64, index,
                                                           ctx.r6.u32, cursor)
                              : any360::xdk::D3dDirtyConstants(SelfTestMemory(), ctx.r3.u32, kCursor, ctx.r4.u64,
                                                               index, ctx.r6.u32, cursor);
    if (!taken) return false;
    ctx.r4.u64 = cursor;
    ctx.r5.u64 = index;
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    const uint32_t end = s.scratch + 0x30000;
    Put32(s, s.device + kCursor, cursor);
    Put32(s, s.device + kCursor + 4, end);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = RandomMask(rng);
    ctx.r5.u64 = rng() % 0x4400;
    ctx.r6.u64 = s.scratch + 0x34000 + (kWords ? uint32_t(rng() % 64) * 4 : uint32_t(rng() % 64) * 16);
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &R3R4R5Differ;
};

// A state setter of xdk_natives.h (xdk::d3d_state: Native on the registers, in the title's layout) with the inputs
// of `Inputs` (a static Setup).
inline const char* NoRegisterDiffers(const PPCContext&, const PPCContext&) { return nullptr; }
template <class Kind, class Inputs>
struct State {
  static bool Native(PPCContext& ctx, uint8_t*) { return Kind::Native(SelfTestMemory(), ctx); }
  static void Setup(CallSetup& s, PPCContext& ctx) { Inputs::Setup(s, ctx); }
  // A function that returns nothing leaves r3 as its code last used it: not compared.
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) =
      xdk::KeepsR3<Kind>() ? nullptr : &NoRegisterDiffers;
};
namespace state {
using xdk::g_d3d_layout;
// A value as titles pass them (0, 1, a small enumeration) and now and then any 64 bits.
inline uint64_t Value(std::mt19937_64& rng) {
  switch (rng() % 5) {
    case 0: return 0;
    case 1: return 1;
    case 2:
    case 3: return rng() % 32;
    default: return rng();
  }
}
// The blend setters: the device's blend flags and word changed now and then (both paths of each setter).
struct Blend {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    if (rng() % 3 == 0) Put32(s, s.device + g_d3d_layout.blend_flags, uint32_t(rng()));
    if (rng() % 3 == 0) Put32(s, s.device + g_d3d_layout.blend_control, uint32_t(rng()));
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = Value(rng);
  }
};
// The bool and integer constants: registers and counts within the bank and now and then past it, a source in the
// scratch at any alignment. (A count of 0 never: the runtime's code loops on it.)
template <uint32_t kRegisters, uint32_t kBytes>
struct Constants {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t first = rng() % 8 ? uint32_t(rng() % kRegisters) : uint32_t(rng() % (4 * kRegisters));
    const uint32_t most = rng() % 8 ? std::max<uint32_t>(kRegisters - first % kRegisters, 1) : 2 * kRegisters;
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = first;
    ctx.r5.u64 = s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = 1 + rng() % most;
    (void)kBytes;
  }
};
// The resource binders: the old resource in the slot (none or a header in the scratch), a fence pending or not, the
// busy mask, the release list in the scratch with room (never full: the runtime would take a new block from the
// command buffer, which in the device copy is the title's own).
inline void Release(CallSetup& s, uint32_t slot) {
  std::mt19937_64& rng = *s.rng;
  const any360::xdk::D3dLayout& l = g_d3d_layout;
  Put32(s, s.device + slot, rng() % 4 == 0 ? 0 : s.scratch + 0x20000 + uint32_t(rng() % 0x1000));
  Put32(s, s.device + l.resource_fence, rng() % 3 == 0 ? uint32_t(rng()) : 0);
  Put32(s, s.device + l.resource_busy_mask, rng() % 4 == 0 ? 0 : uint32_t(rng()));
  const uint32_t cursor = s.scratch + 0x28000 + 8 * uint32_t(rng() % 256);
  Put32(s, s.device + l.release_cursor, cursor);
  Put32(s, s.device + l.release_end, cursor + 64);
}
struct Texture {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t stage = uint32_t(rng() % 32);
    Release(s, g_d3d_layout.texture_bound + 4 * stage);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = stage;
    ctx.r5.u64 = rng() % 4 == 0 ? 0 : s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
};
struct Stream {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t stream = uint32_t(rng() % 16);
    Release(s, g_d3d_layout.stream_bound + 4 * stream);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = stream;
    ctx.r5.u64 = rng() % 4 == 0 ? 0 : s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = rng() % 3 == 0 ? 0 : rng() % 0x10000;
    ctx.r7.u64 = rng() % 8 == 0 ? rng() : 4 * (rng() % 32);
    ctx.r8.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
};
// SetPixelShaderConstantF: as SetShaderConstants.
struct PixelFloats {
  static void Setup(CallSetup& s, PPCContext& ctx) { SetShaderConstants::Setup(s, ctx); }
};
}  // namespace state

}  // namespace any360::host::rexglue::d3d
