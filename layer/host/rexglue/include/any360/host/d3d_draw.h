// The D3D runtime's draw flushes, native (docs/D3D.md, phase D2): DrawIndexedVertices. The call flushes the device's
// dirty state (each dirty group handed to the runtime's own upload functions: the bound ones run Any360's natives,
// the others the title's code, called through the title's table, Runtime) and writes the draw: a type-0 write of
// VGT 0x2102 and the draw itself, split as the runtime splits counts past 65535. The draw is written as DRAW_INDX
// (the runtime's own bytes) or, with any360_d3d_records, as Any360's draw record (any360/xdk/d3d_records.h): the
// command processor draws the same, without the PM4 packet. The occlusion-query variant (a block of the device's query
// list) stays the runtime's own code.
#pragma once

#include <bit>
#include <cstdint>

#include "any360/xdk/d3d_records.h"
#include "any360/xdk/d3d_state.h"
#include "rexglue.h"
#include "xdk_natives.h"

namespace any360::host::rexglue::d3d_draw {

// The title's runtime functions the native draw calls, and its table of primitive splits. A title fills it with the
// functions as its code calls them (bound ones included) before its first draw.
struct Runtime {
  GuestFunction make_room = nullptr;         // r3 device -> r3 the command buffer's cursor
  GuestFunction upload_constants = nullptr;  // r3 device, r4 mask, r5 first register, r6 source (D3dDirtyConstants)
  GuestFunction upload_words = nullptr;      // the same for registers of a word (D3dDirtyWords)
  GuestFunction flush_shaders = nullptr;     // r3 device, r4 dirty bits -> r3 the bits left (shader loads)
  GuestFunction flush_state = nullptr;       // r3 device, r4 dirty bits, r5 a device word -> r3 the bits left
  GuestFunction flush_misc = nullptr;        // r3 device, r4 the bits (the misc state's group)
  GuestFunction flush_waits = nullptr;       // r3 device, r4 the bits (the group with waits)
  uint32_t primitive_table = 0;              // guest address: [divisor, overlap] a primitive type
};
inline Runtime g_runtime;
// any360_d3d_native_draws and any360_d3d_records (d3d_draw.cpp).
bool NativeDraws();
bool DrawRecords();
// any360_d3d_native_draw_helpers off (a diagnostic): draws that need the title's helpers besides its uploads (shader
// loads, state, misc and wait groups) are declined.
bool DrawHelpers();

namespace detail {
using any360::xdk::detail::At32;
using any360::xdk::detail::Set32;
inline uint64_t At64(const uint8_t* p) { return any360::xdk::detail::ByteSwap64(any360::xdk::detail::Load64(p)); }
inline void Set64(uint8_t* p, uint64_t v) { any360::xdk::detail::Store64(p, any360::xdk::detail::ByteSwap64(v)); }
// rldicr: rotated left, the top me + 1 bits kept.
constexpr uint64_t Rldicr(uint64_t v, int sh, int me) { return std::rotl(v, sh) & (~uint64_t(0) << (63 - me)); }
inline uint64_t Call(PPCContext& ctx, uint8_t* base, GuestFunction f, uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0,
                     uint64_t r6 = 0) {
  ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
  f(ctx, base);
  return ctx.r3.u64;
}
inline uint32_t GpuAddress(uint32_t cpu, uint32_t low) { return (((cpu >> 20) + 512) & 0x1000u) + (cpu & low); }
}  // namespace detail

// The device's dirty groups as the draws flush them (Crackdown's runtime; offsets in the device): each group's
// 64-bit dirty word zeroed after its uploads.
inline void FlushDirtyState(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, uint32_t device) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  uint8_t* d = m.Host(device);
  const uint64_t vertex = At64(d + 0), misc = At64(d + 32), other = At64(d + 24), pixel = At64(d + 8);
  uint64_t state = At64(d + 16);
  const auto words = [&](uint64_t mask, uint64_t index, uint32_t source) {
    Call(ctx, base, rt.upload_words, device, mask, index, source);
  };
  if (vertex) {
    Call(ctx, base, rt.upload_constants, device, vertex, 16384, device + 1792);
    Set64(d + 0, 0);
  }
  if (pixel) {
    Call(ctx, base, rt.upload_constants, device, At64(d + 8), 17408, device + 5888);
    Set64(d + 8, 0);
  }
  if (state) {
    if (uint32_t(state) & 0x001E0000u) state = Call(ctx, base, rt.flush_shaders, device, state);
    if (At64(d + 40) & state) state = Call(ctx, base, rt.flush_state, device, state, At32(d + 10432));
    if (state & 0xFFFu) words(state << 52, 8704, device + 10420);
    if (uint32_t(state) & 0x0001F000u) words(Rldicr(state, 47, 4), 8576, device + 10400);
    if (state & 0x03FFFC0000000000ull) words(Rldicr(state, 6, 15), 8192, device + 10240);
    if (state & 0x000003FFFFE00000ull) words(Rldicr(state, 22, 20), 8448, device + 10316);
    Set64(d + 16, 0);
  }
  if (other) {
    if (uint32_t(other)) Call(ctx, base, rt.flush_misc, device, other << 32);
    if (other & 0x007FFFFC00000000ull) words(Rldicr(other, 9, 20), 8832, device + 10468);
    Set64(d + 24, 0);
  }
  if (misc) {
    if (misc & 0x0000003FFFFFFFFFull) words(misc << 26, 8960, device + 10552);
    if (misc & 0x00003FC000000000ull) words(Rldicr(misc, 18, 7), 9088, device + 10704);
    if (misc & 0x007E000000000000ull) Call(ctx, base, rt.flush_waits, device, Rldicr(misc, 9, 5));
    if (misc & (uint64_t(1) << 56)) words(0xFFFFFFFFFF000000ull, 18688, device + 9984);
    Set64(d + 32, 0);
  }
}

// DrawIndexedVertices: r3 device, r4 primitive type, r5 the value of VGT register 0x2102, r6 the first index, r7 the
// index count. False (nothing done): the runtime's own code runs it (the occlusion-query variant, memory that is not
// plain, no vertex declaration, a split the primitive table cannot give). `records`: the draw as Any360's record.
inline bool DrawIndexed(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32;
  if (!rt.make_room || !any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  if (d[10812] & 0x02) return false;
  const uint64_t primitive = ctx.r4.u64;
  const uint32_t vgt = ctx.r5.u32, caller_r1 = ctx.r1.u32;
  uint32_t first = ctx.r6.u32, count = ctx.r7.u32;
  const uint32_t declaration = At32(d + 12156);
  if (!declaration || !any360::xdk::IsPlainMemory(declaration, 28)) return false;
  const uint32_t split_at = rt.primitive_table + (uint32_t(primitive) << 3);
  if (!DrawHelpers()) {
    const uint64_t state = detail::At64(d + 16), other = detail::At64(d + 24), misc = detail::At64(d + 32);
    if ((uint32_t(state) & 0x001E0000u) || (detail::At64(d + 40) & state) || uint32_t(other) ||
        (misc & 0x007E000000000000ull))
      return false;
  }
  if (count > 65535 && (!any360::xdk::IsPlainMemory(split_at, 8) || At32(m.Host(split_at)) == 0)) return false;
  ctx.r1.u64 = caller_r1 - 240;  // the function's own frame: its callees' frames below it, as the runtime's
  FlushDirtyState(ctx, base, m, device);
  const uint8_t* decl = m.Host(declaration);
  for (;;) {
    uint32_t cursor = At32(d + 48);
    if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
    uint32_t drawn = count;
    if (count > 65535) {
      const uint32_t divisor = At32(m.Host(split_at));
      drawn = ((65535u / divisor) & ~1u) * divisor;
    }
    const uint32_t decl0 = At32(decl), index_base = At32(decl + 24);
    const uint32_t query_index = d[11907];
    uint32_t initiator = (drawn << 16) | (uint32_t(primitive) & 0x3F);
    const uint32_t endian = std::rotl(decl0, 1) & 0xC0000000u;
    uint32_t size = (drawn & 0xFFFFFFu) | endian;
    uint32_t address = GpuAddress((first << 1) + index_base, 0x1FFFFFFFu);
    if (decl0 & 0x80000000u) {
      size = ((drawn << 1) & 0x00FFFFFEu) | endian;
      initiator |= 0x800;
      address = GpuAddress((first << 2) + index_base, 0x1FFFFFFFu);
    }
    uint8_t* out = m.Host(cursor + 4);
    Set32(out, 8450), Set32(out + 4, vgt);  // type-0: VGT 0x2102
    if (records) {
      Set32(out + 8, record::Header(record::DrawWords(true)));
      const uint32_t words[4] = {record::KindWord(record::kDraw, 0), initiator, address, size};
      std::memcpy(out + 12, words, 16);
    } else {
      uint32_t query = d[11842 + query_index];
      query = ((query & ~0x100u) | ((query_index != 0 ? 1u : 0u) << 8)) & 0x1FFu & ~0xC0u;
      Set32(out + 8, 0xC0032201u), Set32(out + 12, query), Set32(out + 16, initiator), Set32(out + 20, address);
      Set32(out + 24, size);
    }
    Set32(d + 48, cursor + 28);
    const uint32_t left = count - drawn;
    if (left == 0) break;
    const uint32_t overlap = At32(m.Host(split_at + 4));
    count = overlap + left;
    first += drawn - overlap;
  }
  ctx.r1.u64 = caller_r1;
  return true;
}

// What a title's binding of DrawIndexedVertices runs: the native draw when any360_d3d_native_draws is on and guest
// memory is resolved, else (or when it declines) the runtime's own code.
inline void DrawIndexedOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && DrawIndexed(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

}  // namespace any360::host::rexglue::d3d_draw
