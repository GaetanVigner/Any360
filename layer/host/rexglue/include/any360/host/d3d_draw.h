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
  GuestFunction allocate = nullptr;          // r3 device, r4 dwords, r5 alignment -> r3 the block (0: none) at the end
                                             // of the command buffer's segment (BeginVertices' vertices and indices)
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

namespace detail {
// What both draws decline before writing anything: memory, the occlusion-query variant, the helpers (diagnostic), a
// split the primitive table cannot give.
inline bool DrawAccepted(const any360::xdk::Memory& m, uint32_t device, uint64_t primitive, uint32_t count,
                         uint32_t& split_at) {
  const Runtime& rt = g_runtime;
  if (!rt.make_room || !any360::xdk::IsPlainMemory(device, 16384)) return false;
  const uint8_t* d = m.Host(device);
  if (d[10812] & 0x02) return false;
  if (!DrawHelpers()) {
    const uint64_t state = At64(d + 16), other = At64(d + 24), misc = At64(d + 32);
    if ((uint32_t(state) & 0x001E0000u) || (At64(d + 40) & state) || uint32_t(other) || (misc & 0x007E000000000000ull))
      return false;
  }
  split_at = rt.primitive_table + (uint32_t(primitive) << 3);
  return count <= 65535 || (any360::xdk::IsPlainMemory(split_at, 8) && At32(m.Host(split_at)) != 0);
}
// The vertices or indices a packet draws: all of them up to 65535, else the largest even multiple of the primitive's
// divisor that fits; the rest drawn after, from `overlap` before where this one ends.
inline uint32_t DrawnNow(const any360::xdk::Memory& m, uint32_t split_at, uint32_t count) {
  if (count <= 65535) return count;
  const uint32_t divisor = At32(m.Host(split_at));
  return ((65535u / divisor) & ~1u) * divisor;
}
// DRAW_INDX's visibility query dword: the device's query byte for its current query, bit 8 set when one is current.
inline uint32_t QueryWord(const uint8_t* d) {
  const uint32_t index = d[11907];
  const uint32_t query = d[11842 + index];
  return ((query & ~0x100u) | ((index != 0 ? 1u : 0u) << 8)) & 0x1FFu & ~0xC0u;
}
}  // namespace detail

// DrawIndexedVertices: r3 device, r4 primitive type, r5 the value of VGT register 0x2102, r6 the first index, r7 the
// index count. False (nothing done): the runtime's own code runs it (the occlusion-query variant, memory that is not
// plain, no vertex declaration, a split the primitive table cannot give). `records`: the draw as Any360's record.
inline bool DrawIndexed(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32;
  const uint64_t primitive = ctx.r4.u64;
  const uint32_t vgt = ctx.r5.u32, caller_r1 = ctx.r1.u32;
  uint32_t first = ctx.r6.u32, count = ctx.r7.u32, split_at = 0;
  if (!DrawAccepted(m, device, primitive, count, split_at)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t declaration = At32(d + 12156);
  if (!declaration || !any360::xdk::IsPlainMemory(declaration, 28)) return false;
  ctx.r1.u64 = caller_r1 - 240;  // the function's own frame: its callees' frames below it, as the runtime's
  FlushDirtyState(ctx, base, m, device);
  const uint8_t* decl = m.Host(declaration);
  for (;;) {
    uint32_t cursor = At32(d + 48);
    if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
    const uint32_t drawn = DrawnNow(m, split_at, count);
    const uint32_t decl0 = At32(decl), index_base = At32(decl + 24);
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
      Set32(out + 8, record::Header(record::DrawWords(true), true));
      const uint32_t words[4] = {record::KindWord(record::kDraw, 0), initiator, address, size};
      std::memcpy(out + 12, words, 16);
    } else {
      Set32(out + 8, 0xC0032201u), Set32(out + 12, QueryWord(d)), Set32(out + 16, initiator), Set32(out + 20, address);
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

// DrawVertices: r3 device, r4 primitive type, r5 the first vertex (VGT 0x2102), r6 the vertex count. The same flush,
// then DRAW_INDX with auto indices (or its record), the first vertex moving on with each split.
inline bool DrawVertices(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32;
  const uint64_t primitive = ctx.r4.u64;
  const uint32_t caller_r1 = ctx.r1.u32;
  uint32_t first = ctx.r5.u32, count = ctx.r6.u32, split_at = 0;
  if (!DrawAccepted(m, device, primitive, count, split_at)) return false;
  uint8_t* d = m.Host(device);
  ctx.r1.u64 = caller_r1 - 208;
  FlushDirtyState(ctx, base, m, device);
  for (;;) {
    uint32_t cursor = At32(d + 48);
    if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
    const uint32_t drawn = DrawnNow(m, split_at, count);
    const uint32_t initiator = (drawn << 16) | (uint32_t(primitive) & 0x3F) | 0x80;  // auto indices
    uint8_t* out = m.Host(cursor + 4);
    Set32(out, 8450), Set32(out + 4, first);  // type-0: VGT 0x2102
    if (records) {
      Set32(out + 8, record::Header(record::DrawWords(false), true));
      const uint32_t words[2] = {record::KindWord(record::kDraw, 2), initiator};
      std::memcpy(out + 12, words, 8);
    } else {
      Set32(out + 8, 0xC0012201u), Set32(out + 12, QueryWord(d)), Set32(out + 16, initiator);
    }
    Set32(d + 48, cursor + 20);
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

namespace detail {
// What the Begin draws write before their draw: VGT 0x2102, the immediate vertices' fetch constant (registers
// 0x48BA-0x48BF: `words` the first four, then the block's GPU address with 3 and its size in dwords) and three zeros
// at 0x5000.
inline uint8_t* WriteBeginState(uint8_t* out, uint32_t vgt, const uint32_t (&words)[4], uint32_t vertices, uint32_t dwords) {
  Set32(out, 8450), Set32(out + 4, vgt);
  Set32(out + 8, 0x000548BAu);
  for (int i = 0; i < 4; ++i) Set32(out + 12 + 4 * i, words[i]);
  Set32(out + 28, GpuAddress(vertices, 0x1FFFFFFFu) | 3u);
  Set32(out + 32, 0x10000002u | ((dwords << 2) & 0x03FFFFFCu));
  Set32(out + 36, 0x00025000u), Set32(out + 40, 0), Set32(out + 44, 0), Set32(out + 48, 0);
  return out + 52;
}
// The stream 0 stride as the Begin draws set it for their vertices (the shader's stride bit when it differs from the
// vertex shader's), before the flush; the stride byte as it was, after.
inline uint8_t SetBeginStride(uint8_t* d, uint32_t stride) {
  const uint8_t saved = d[12248];
  const uint32_t words = (stride >> 2) & 0xFF;
  d[12248] = uint8_t(words);
  if (words != d[11552] && At32(d + 12416)) Set64(d + 16, At64(d + 16) | 0x80000);
  return saved;
}
inline bool BeginAccepted(const any360::xdk::Memory& m, uint32_t device) {
  uint32_t split_at;
  return g_runtime.allocate && DrawAccepted(m, device, 0, 0, split_at);
}
}  // namespace detail

// BeginVertices: r3 device, r4 primitive type, r5 the vertex count, r6 the stride in bytes. Flushes, takes a block for
// the vertices at the end of the command buffer's segment, writes the immediate vertices' fetch constant and an auto
// draw of them after the cursor (EndVertices moves the cursor past them: their end at +13076, the block at +13080 and
// its dwords at +13088), and returns the block (r3; 0 when there is none, the cursor kept).
inline bool BeginVertices(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32;
  if (!BeginAccepted(m, device)) return false;
  uint8_t* d = m.Host(device);
  const uint64_t primitive = ctx.r4.u64;
  const uint32_t count = ctx.r5.u32, stride = ctx.r6.u32, caller_r1 = ctx.r1.u32;
  const uint32_t dwords = (count * stride) >> 2;
  const uint8_t saved = SetBeginStride(d, stride);
  ctx.r1.u64 = caller_r1 - 176;
  FlushDirtyState(ctx, base, m, device);
  const uint32_t limit = At32(d + 56);
  uint32_t cursor = At32(d + 48);
  d[12248] = saved;
  if (cursor > limit) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
  const uint32_t vertices = uint32_t(Call(ctx, base, rt.allocate, device, dwords, 16));
  ctx.r1.u64 = caller_r1;
  if (!vertices) {
    Set32(d + 48, cursor);
    ctx.r3.u64 = 0;
    return true;
  }
  uint8_t* out = WriteBeginState(m.Host(cursor + 4), 0, {1, 0, 1, 0}, vertices, dwords);
  const uint32_t initiator = ((count << 16) & 0xFFFF0000u) | (uint32_t(primitive) & 0x3F) | 0x80;
  if (records) {
    Set32(out, record::Header(record::DrawWords(false), true));
    const uint32_t words[2] = {record::KindWord(record::kDraw, 2), initiator};
    std::memcpy(out + 4, words, 8);
  } else {
    Set32(out, 0xC0012201u), Set32(out + 4, QueryWord(d)), Set32(out + 8, initiator);
  }
  Set64(d + 24, At64(d + 24) | 1);
  Set32(d + 13076, cursor + 64), Set32(d + 13080, vertices), Set32(d + 13088, dwords);
  ctx.r3.u64 = vertices;
  return true;
}

// BeginIndexedVertices: r3 device, r4 primitive type, r5 VGT 0x2102, r6 the vertex count, r7 the index count, r8 flags
// (bit 2: 32-bit indices; bits 0-1: their endianness), r9 the stride, r10 where to write the index block, and the
// ninth argument (the caller's stack word at +84) where to write the vertex block. Takes both blocks, writes the fetch constant and an
// indexed draw of the index block, and returns 0 (S_OK), or E_OUTOFMEMORY with both pointers 0 and the cursor kept
// when either block cannot be had.
inline bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32;
  const uint32_t caller_r1 = ctx.r1.u32, index_out = ctx.r10.u32;
  if (!BeginAccepted(m, device) || !any360::xdk::IsPlainMemory(index_out, 4) ||
      !any360::xdk::IsPlainMemory(caller_r1 + 84, 4))
    return false;
  const uint32_t vertex_out = At32(m.Host(caller_r1 + 84));
  if (!any360::xdk::IsPlainMemory(vertex_out, 4)) return false;
  uint8_t* d = m.Host(device);
  const uint64_t primitive = ctx.r4.u64;
  const uint32_t vgt = ctx.r5.u32, indices = ctx.r7.u32, flags = ctx.r8.u32, stride = ctx.r9.u32;
  const uint32_t vertex_dwords = (ctx.r6.u32 * stride) >> 2;
  const bool wide = (flags & 4) != 0;
  const uint32_t index_dwords = wide ? indices : ((indices + 1) >> 1) & 0x7FFFFFFFu;
  const uint8_t saved = SetBeginStride(d, stride);
  ctx.r1.u64 = caller_r1 - 240;
  FlushDirtyState(ctx, base, m, device);
  const uint32_t limit = At32(d + 56);
  uint32_t cursor = At32(d + 48);
  d[12248] = saved;
  if (cursor > limit) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
  const uint32_t vertices = uint32_t(Call(ctx, base, rt.allocate, device, vertex_dwords, 16));
  const uint32_t index_block = uint32_t(Call(ctx, base, rt.allocate, device, index_dwords, 16));
  ctx.r1.u64 = caller_r1;
  if (!vertices || !index_block) {
    Set32(d + 48, cursor);
    Set32(m.Host(index_out), 0), Set32(m.Host(vertex_out), 0);
    ctx.r3.s64 = int32_t(0x8007000Eu);
    return true;
  }
  uint8_t* out = WriteBeginState(m.Host(cursor + 4), vgt, {0, 0, 0, 0}, vertices, vertex_dwords);
  uint32_t initiator = (indices << 16) | (uint32_t(primitive) & 0x3F);
  const uint32_t endian = std::rotl(flags, 30) & 0xC0000000u;
  uint32_t size = (indices & 0xFFFFFFu) | endian;
  if (wide) initiator |= 0x800, size = ((indices << 1) & 0x00FFFFFEu) | endian;
  const uint32_t address = GpuAddress(index_block, 0x1FFFFFFFu);
  if (records) {
    Set32(out, record::Header(record::DrawWords(true), true));
    const uint32_t words[4] = {record::KindWord(record::kDraw, 0), initiator, address, size};
    std::memcpy(out + 4, words, 16);
  } else {
    Set32(out, 0xC0032201u), Set32(out + 4, QueryWord(d)), Set32(out + 8, initiator), Set32(out + 12, address);
    Set32(out + 16, size);
  }
  Set64(d + 24, At64(d + 24) | 1);
  Set32(d + 13076, cursor + 72), Set32(d + 13080, vertices), Set32(d + 13084, index_block);
  Set32(d + 13088, vertex_dwords), Set32(d + 13092, index_dwords);
  Set32(m.Host(index_out), index_block), Set32(m.Host(vertex_out), vertices);
  ctx.r3.u64 = 0;
  return true;
}

// SetPredication: r3 device, r4 the tiles (bits) the next commands run for, 0 for all. The runtime keeps it (+12428:
// the mask written, -1 for all; +12436 set when a mask is; +10804; +12432 and +10808 cleared) and, with no mask, sets
// flag 0x02 of +10812 (the draws' predicated-query variant) when its tiling conditions hold; then writes the bin mask
// (SET_BIN_MASK_LO, or Any360's bins record).
inline bool SetPredication(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, tiles = ctx.r4.u32;
  if (!rt.make_room || !any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  if (tiles == 0) {
    const uint8_t flags = d[10812];
    Set32(d + 12436, 0);
    bool tiled = false;
    if (flags & 0x10) {
      tiled = true;
    } else if (flags & 0x20) {
      tiled = true;
      for (uint32_t k = 0; k < 5 && tiled; ++k) {
        const uint32_t bound = At32(d + 12160 + 4 * k);
        tiled = At32(d + 12448 + 4 * k) == bound || bound == 0;
      }
    }
    const bool query = tiled && !(flags & 0x08) && ((d[10815] & 0x40) || (d[11841] == 0 && d[11907] == 0));
    Set32(d + 12428, 0xFFFFFFFFu);
    d[10812] = uint8_t((flags & ~0x02) | (query ? 0x02 : 0));
  } else {
    Set32(d + 12428, tiles);
    Set32(d + 12436, 1);
    d[10812] &= 0xFD;
  }
  const uint32_t limit = At32(d + 56);
  uint32_t cursor = At32(d + 48);
  Set32(d + 12432, 0), Set32(d + 10804, tiles), Set32(d + 10808, 0);
  if (cursor > limit) {
    const uint32_t caller_r1 = ctx.r1.u32;
    ctx.r1.u64 = caller_r1 - 96;
    cursor = uint32_t(Call(ctx, base, rt.make_room, device));
    ctx.r1.u64 = caller_r1;
  }
  uint8_t* out = m.Host(cursor + 4);
  if (records) {
    Set32(out, record::Header(record::kBinsWords));
    const uint32_t words[2] = {record::KindWord(record::kBins, 0), At32(d + 12428)};
    std::memcpy(out + 4, words, 8);
    Set32(d + 48, cursor + 12);
  } else {
    Set32(out, 0xC0006000u), Set32(out + 4, At32(d + 12428));
    Set32(d + 48, cursor + 8);
  }
  return true;
}
inline void SetPredicationOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && SetPredication(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

// The same for DrawVertices.
inline void DrawVerticesOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && DrawVertices(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

// And for the Begin draws.
inline void BeginVerticesOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && BeginVertices(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}
inline void BeginIndexedVerticesOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && BeginIndexedVertices(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

}  // namespace any360::host::rexglue::d3d_draw
