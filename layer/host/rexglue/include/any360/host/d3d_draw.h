// The D3D runtime's draw flushes, native (docs/D3D.md, phase D2): DrawIndexedVertices. The call flushes the device's
// dirty state (each dirty group handed to the runtime's own upload functions: the bound ones run Any360's natives,
// the others the title's code, called through the title's table, Runtime) and writes the draw: a type-0 write of
// VGT 0x2102 and the draw itself, split as the runtime splits counts past 65535. The draw is written as DRAW_INDX
// (the runtime's own bytes) or, with any360_d3d_records, as Any360's draw record (any360/xdk/d3d_records.h): the
// command processor draws the same, without the PM4 packet. The occlusion-query variant (a block of the device's query
// list) stays the runtime's own code.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
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
  GuestFunction query_occlusion = nullptr;   // r3 device, r4 the query, r5 1 begin / 0 end (occlusion queries)
  GuestFunction allocate = nullptr;          // r3 device, r4 dwords, r5 alignment -> r3 the block (0: none) at the end
                                             // of the command buffer's segment (BeginVertices' vertices and indices)
  GuestFunction clear_target = nullptr;      // the clear's draw of one target (or of the depth alone)
  GuestFunction restore_viewport = nullptr;  // r3 device, r4-r7 the viewport's corners (after a clear)
  GuestFunction surface_level = nullptr;     // the resolve: a texture's level and slice geometry (11 arguments)
  GuestFunction resolve_clear_color = nullptr;  // the resolve's color clear after the copy (v1 the color)
  GuestFunction resolve_clear_depth = nullptr;  // the resolve's depth clear after the copy (f1 the depth)
  GuestFunction release_block = nullptr;     // r3 device -> a new release-list block (the binders decline it)
  // The resolve's format table (a half-word a format), default destination point, copy pixel shader (15 dwords) and
  // vertex offset float: guest addresses in the title's image.
  uint32_t resolve_format_table = 0, resolve_default_point = 0, resolve_pixel_shader = 0, resolve_offset_float = 0;
  uint32_t primitive_table = 0;              // guest address: [divisor, overlap] a primitive type
  // The clear's vertex shader (27 dwords) and pixel shader (9 dwords), and the color it clears to when given none
  // (one float for the four channels): guest addresses in the title's image.
  uint32_t clear_vertex_shader = 0, clear_pixel_shader = 0, clear_default_color = 0;
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
using any360::xdk::detail::Overlap;
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
namespace detail {
// fctiwz as the recompiled code does it: NaN to 0x80000000, INT_MAX and above to INT_MAX, else truncated (0x80000000
// below INT_MIN).
inline int32_t Fctiwz(float f) {
  const double v = f;
  if (std::isnan(v)) return int32_t(0x80000000u);
  if (v >= 2147483647.0) return 2147483647;
  if (v < -2147483648.0) return int32_t(0x80000000u);
  return int32_t(v);
}
inline float AtFloat(const uint8_t* p) { return std::bit_cast<float>(At32(p)); }
}  // namespace detail

namespace detail {
// Writes a flush's packets after the command buffer's cursor (the last word written), as the runtime's PM4 or as
// Any360's records (any360/xdk/d3d_records.h): each call one packet.
struct Out {
  const any360::xdk::Memory& m;
  uint32_t cursor;
  bool records;
  void Word(uint32_t v) { cursor += 4, Set32(m.Host(cursor), v); }
  void Host(uint32_t v) { cursor += 4, std::memcpy(m.Host(cursor), &v, 4); }
  // Consecutive registers from `first` (type-0, or a register record; predicated for SET_CONSTANT's form).
  void Registers(uint32_t first, std::initializer_list<uint32_t> values) {
    const uint32_t n = uint32_t(values.size());
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::RegistersWords(n)));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kRegisters, first));
      for (uint32_t v : values) Host(v);
    } else {
      Word(((n - 1) << 16) | first);
      for (uint32_t v : values) Word(v);
    }
  }
  // SET_CONSTANT (predicated) of context register `index` (0x2000 + index).
  void ContextConstant(uint32_t index, uint32_t value) {
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::RegistersWords(1), true));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kRegisters, 0x2000 + index)), Host(value);
    } else {
      Word(0xC0012D01u), Word(0x00040000u | index), Word(value);
    }
  }
  // SET_BIN_MASK_LO (0), _HI (1), SET_BIN_SELECT_LO (2), _HI (3).
  void Bins(uint32_t which, uint32_t value) {
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::kBinsWords));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kBins, which)), Host(value);
    } else {
      Word(0xC0006000u + (which << 8)), Word(value);
    }
  }
  void Event(uint32_t opcode, std::initializer_list<uint32_t> words) {
    const uint32_t n = uint32_t(words.size());
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::EventWords(n)));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kEvent, opcode));
      for (uint32_t v : words) Host(v);
    } else {
      Word(0xC0000000u | ((n - 1) << 16) | (opcode << 8));
      for (uint32_t v : words) Word(v);
    }
  }
  void Invalidate(uint32_t value) {
    if (records) {
      Word(any360::xdk::record::Header(2));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kInvalidate, 0)), Host(value);
    } else {
      Word(0xC0003B00u), Word(value);
    }
  }
  // IM_LOAD_IMMEDIATE of `dwords` of microcode at `from` (as in memory) into shader `type`.
  void Shader(uint32_t type, uint32_t from, uint32_t dwords) {
    if (records) {
      Word(any360::xdk::record::Header(1 + dwords));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kShader, type));
    } else {
      Word(0xC0002B00u | ((dwords + 1) << 16)), Word(type), Word(dwords);
    }
    std::memcpy(m.Host(cursor + 4), m.Host(from), 4 * size_t(dwords));
    cursor += 4 * dwords;
  }
  void Wait(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    if (records) {
      Word(any360::xdk::record::Header(6));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kWait, 0));
      for (uint32_t v : {a, b, c, d, e}) Host(v);
    } else {
      Word(0xC0043C00u);
      for (uint32_t v : {a, b, c, d, e}) Word(v);
    }
  }
  // DRAW_INDX_2 with auto indices (`predicated` its header's bit 0).
  void AutoDraw(uint32_t initiator, bool predicated) {
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::DrawWords(false), predicated));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kDraw, 2)), Host(initiator);
    } else {
      Word(0xC0003600u | (predicated ? 1u : 0u)), Word(initiator);
    }
  }
};
}  // namespace detail

// The clear (the runtime's Clear under its rect wrapper): r3 device, r4 the flags (bits 0-3 color targets, 4-5 depth
// and stencil), r5 the rect (x1, y1, x2, y2), r6 the colors (4 floats; 0: the default color), f1 the depth, r8 the
// stencil. The rect clipped by the viewport and (when on) the scissor; nothing when empty or when only depth is asked
// with no depth surface. Uploads the dirty state the clear's draws use, loads the clear's shaders (IM_LOAD_IMMEDIATE),
// sets the state of a clear, then draws each target through the runtime's own target clear and restores the viewport.
// As PM4 or as Any360's records.
namespace detail {
// The runtime's 8-byte-aligned uploads (sub_826C5D40's fetch constants, sub_826C5BC0's group): for each run of set
// bits of `mask` (bit 63 first), `words` dwords a bit from `source` (the device's copy) to the registers from `first`
// on. PM4: a type-2 pad when the cursor is not on 8 bytes, the type-0 header, the data on 8 bytes. `extra`: the room
// test's words past the run (5 and 1). Declines, writing nothing, when a run lacks room (the runtime then writes it
// in pieces, making room) or the mask is 0 (it would loop for ever).
struct AlignedUpload {
  uint32_t words, first, source, extra;
};
inline bool AlignedUploadFits(const uint8_t* d, uint64_t mask, const AlignedUpload& u, uint32_t cursor, bool records,
                              uint32_t& final_cursor) {
  if (!mask) return false;
  const uint32_t end = At32(d + 52);
  uint64_t bits = mask;
  uint64_t at = cursor;
  while (bits) {
    const uint32_t skip = uint32_t(std::countl_zero(bits));
    bits <<= skip;
    const uint32_t run = uint32_t(std::countl_zero(~bits));
    bits = run == 64 ? 0 : bits << run;
    const uint64_t data = uint64_t(run) * u.words;
    if (records) {
      at += 8 + 4 * data;
      if (at >= end) return false;
    } else {
      if (4 * (data + u.extra) + at >= end) return false;  // the runtime's own test, before its pad
      at += (at & 7) + 4 + 4 * data;
    }
  }
  final_cursor = uint32_t(at);
  return at < 0xFFFFFFF0u;
}
inline void WriteAlignedUpload(const any360::xdk::Memory& m, uint8_t* d, uint64_t mask, const AlignedUpload& u, Out& out) {
  uint64_t bits = mask;
  uint32_t index = u.first;
  const uint8_t* from = d + u.source;
  while (bits) {
    const uint32_t skip = uint32_t(std::countl_zero(bits));
    bits <<= skip;
    const uint32_t run = uint32_t(std::countl_zero(~bits));
    bits = run == 64 ? 0 : bits << run;
    from += size_t(skip) * 4 * u.words, index += skip * u.words;
    const uint32_t data = run * u.words;
    if (out.records) {
      out.Word(any360::xdk::record::Header(any360::xdk::record::RegistersWords(data)));
      out.Host(any360::xdk::record::KindWord(any360::xdk::record::kRegisters, index));
      uint8_t* to = m.Host(out.cursor + 4);
      for (uint32_t w = 0; w < data; ++w) any360::xdk::detail::Store32(to + 4 * w, std::byteswap(any360::xdk::detail::Load32(from + 4 * w)));
    } else {
      Set32(m.Host(out.cursor + 4), 0x80000000u);
      out.cursor += out.cursor & 7;
      out.Word(((data - 1) << 16) | index);
      std::memcpy(m.Host(out.cursor + 4), from, 4 * size_t(data));
    }
    out.cursor += 4 * data, from += 4 * data, index += data;
  }
}
}  // namespace detail

// sub_826C5D40 (the draws' misc group): r3 device, r4 the fetch constants' dirty bits; their 6 dwords a slot from the
// device's copy (+1024) to the fetch constant registers (0x4800 on), then the three registers at 0x5000 (0, 0x25000,
// 0, as the runtime writes them).
inline bool FlushFetchConstants(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32;
  if (!any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  const AlignedUpload upload{6, 0x4800, 1024, 5};
  const uint32_t cursor = At32(d + 48);
  uint32_t final_cursor = 0;
  if (!AlignedUploadFits(d, ctx.r4.u64, upload, cursor, records, final_cursor) ||
      !any360::xdk::IsPlainMemory(cursor + 4, final_cursor - cursor + 32))
    return false;
  Out out{m, cursor, records};
  WriteAlignedUpload(m, d, ctx.r4.u64, upload, out);
  out.Registers(0x5000, {0, 0x00025000u, 0});
  Set32(d + 48, out.cursor);
  return true;
}
// sub_826C5BC0 (the draws' wait group): r3 device, r4 its dirty bits. VGT 0x2007 from the device, the copy registers
// cleared, a wait for the copy, then the group's 4 dwords a bit from the device (+10144) to the registers from 0x2388.
// Declined when the cursor needs room first (the runtime's make-room).
inline bool FlushWaitGroup(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32;
  if (!any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t cursor = At32(d + 48);
  if (cursor > At32(d + 56)) return false;
  // Everything checked before anything is written: the fixed packets (13 dwords as PM4, 17 as records), then the runs.
  const AlignedUpload upload{4, 0x2388, 10144, 1};
  uint32_t final_cursor = 0;
  if (!AlignedUploadFits(d, ctx.r4.u64, upload, cursor + (records ? 68 : 52), records, final_cursor) ||
      !any360::xdk::IsPlainMemory(cursor + 4, final_cursor - cursor))
    return false;
  Out out{m, cursor, records};
  out.Registers(0x2007, {At32(d + 10268)}), out.Registers(0xA31, {0x10000}), out.Registers(0xA2F, {0, 4096});
  out.Wait(3, 2609, 0, 0x80000000u, 8);
  WriteAlignedUpload(m, d, ctx.r4.u64, upload, out);
  Set32(d + 48, out.cursor);
  return true;
}
inline void FlushFetchConstantsOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && FlushFetchConstants(ctx, m, DrawRecords())) return;
  original(ctx, base);
}
inline void FlushWaitGroupOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && FlushWaitGroup(ctx, m, DrawRecords())) return;
  original(ctx, base);
}
// sub_826C5768 (the draws' state group): r3 device, r4 the state's dirty bits, r5 the device's word +10432 (the
// 0x2203 register's value); r3 back: the bits it did not handle.
// - Bit 8: register 0x2203 from r5. When the device tiles (10815 bit 6, then 10812 bit 4, or bit 5 with the five
//   words at +12160 matching those at +12448 or 0), one predicated SET_CONSTANT a tile (+12468 of them, last first)
//   under its bin mask (3 << 2 * tile), the tile's word (+12832 on) in its top 15 bits and bit 0 kept as +11572's
//   mode says; then r5 itself to bin 0 when tile 0's value lost bit 0 (10812 bit 6), and the device's bins back
//   (+12428, +12432). Else a plain write, bit 0 cleared when +11572's bits 17-19 are 0.
// - Bit 57 (with the device's +40 mask): register 0x2000 (+10240); bit 37: 0x2104 (+10332). The values these change
//   are then written again under the bin mask 0x15555555 (predicated) and the bins put back.
// Declines when the cursor needs room first, when a tiled write would pass the room's limit (the runtime makes room
// in the middle) or the tile count is 0 (the runtime reads an unset stack word) or over 16.
inline bool FlushState(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32;
  if (!any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t cursor = At32(d + 48), limit = At32(d + 56);
  if (cursor > limit) return false;
  uint64_t bits = ctx.r4.u64;
  const uint32_t value = ctx.r5.u32;
  uint32_t count = 0;
  bool tiled = false;
  if (bits & 0x100) {
    if (d[10815] & 0x40) {
      const uint8_t flags = d[10812];
      if (flags & 0x10) {
        tiled = true;
      } else if (flags & 0x20) {
        tiled = true;
        for (uint32_t k = 0; k < 5 && tiled; ++k) {
          const uint32_t want = At32(d + 12160 + 4 * k);
          tiled = At32(d + 12448 + 4 * k) == want || want == 0;
        }
      }
    }
    if (tiled) {
      count = At32(d + 12468);
      if (count == 0 || count > 16) return false;
      if (uint64_t(cursor) + 4ull * ((records ? 3 : 2) + (records ? 6 : 5) * count) > limit) return false;
    }
  }
  // Every word this writes (a bound), in the room: the runtime writes past its limit after the tiles' loop.
  const uint32_t most = 4 * ((records ? 36 : 28) + (records ? 6 : 5) * count);
  if (!any360::xdk::IsPlainMemory(cursor + 4, most) || (records && uint64_t(cursor) + most > At32(d + 52)))
    return false;
  Out out{m, cursor, records};
  uint32_t again_2203 = ~0u, again_2000 = ~0u, again_2104 = ~0u;
  if (bits & 0x100) {
    if (tiled) {
      out.Bins(1, 0);
      const uint32_t mode = (At32(d + 11572) >> 17) & 7;
      uint32_t tile_value = 0, last = 0;
      for (uint32_t i = count; i-- > 0;) {
        tile_value = (value & 0x1FFFFu) | (At32(d + 12832 + 4 * i) << 17);
        const bool keep = mode != 0 && (mode != 2 || (i == 0 && (d[10815] & 0x20)));
        last = keep ? tile_value : tile_value & ~1u;
        out.Bins(0, 3u << (2 * i));
        out.ContextConstant(0x203, last);
      }
      if (last != tile_value && (d[10812] & 0x40)) out.Bins(0, 1), out.ContextConstant(0x203, tile_value);
      out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
    } else {
      const uint32_t written = (At32(d + 11572) & 0xE0000u) ? value : value & ~1u;
      out.Registers(0x2203, {written});
      if (written != value && (d[10812] & 0x40)) again_2203 = value;
    }
    bits &= ~uint64_t(0x100);
  }
  const uint64_t handled = At64(d + 40) & bits;
  if (handled & (uint64_t(1) << 57)) {
    const uint32_t v = At32(d + 10240);
    out.Registers(0x2000, {v});
    bits &= ~(uint64_t(1) << 57);
    again_2000 = (At32(d + 12896) & 0x3FFFu) | (v & 0xFFFFC000u);
  }
  if (handled & (uint64_t(1) << 37)) {
    out.Registers(0x2104, {At32(d + 10332)});
    bits &= ~(uint64_t(1) << 37);
    again_2104 = 0;
  }
  if ((again_2104 & again_2000 & again_2203) != ~0u) {
    out.Bins(1, 0), out.Bins(0, 0x15555555u);
    if (again_2203 != ~0u) out.ContextConstant(0x203, again_2203);
    if (again_2000 != ~0u) out.ContextConstant(0x000, again_2000);
    if (again_2104 != ~0u) out.ContextConstant(0x104, again_2104);
    out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
  }
  Set32(d + 48, out.cursor);
  ctx.r3.u64 = bits;
  return true;
}
inline void FlushStateOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && FlushState(ctx, m, DrawRecords())) return;
  original(ctx, base);
}

// The state of a clear and of a copy: VGT 0x2100-0x2102, 0x2204-0x2206, 0x2312 and 0x200D.
inline void WriteClearState(detail::Out& out) {
  out.Registers(0x2100, {0xFFFF, 0, 0}), out.Registers(0x2204, {0x10000, 0x10000, 768});
  out.Registers(0x2312, {0xFFFF}), out.Registers(0x200D, {0});
}
inline bool Clear(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, rect = ctx.r5.u32, caller_r1 = ctx.r1.u32;
  uint32_t flags = ctx.r4.u32;
  if (!rt.make_room || !rt.clear_target || !rt.restore_viewport || !rt.clear_vertex_shader ||
      !any360::xdk::IsPlainMemory(device, 16384) || !any360::xdk::IsPlainMemory(rect, 16) ||
      !any360::xdk::IsPlainMemory(caller_r1 - 256, 256))
    return false;
  uint8_t* d = m.Host(device);
  const double depth = ctx.f1.f64;
  const uint32_t stencil = ctx.r8.u32;
  uint32_t colors = ctx.r6.u32;
  const uint32_t frame = caller_r1 - 240;  // the function's own frame (its callees' ninth argument at +92)
  if (!colors) {
    colors = frame + 112;
    const uint32_t c = At32(m.Host(rt.clear_default_color));
    for (int i = 0; i < 4; ++i) Set32(m.Host(colors + 4 * i), c);
  }
  if (At32(d + 12176) == 0) {
    flags &= ~0x30u;
    if (!flags) return true;
  }
  const uint8_t* r = m.Host(rect);
  const int32_t vx = Fctiwz(AtFloat(d + 12368)), vy = Fctiwz(AtFloat(d + 12372));
  const int32_t vw = Fctiwz(AtFloat(d + 12376)), vh = Fctiwz(AtFloat(d + 12380));
  int32_t left = std::max(int32_t(At32(r)), vx), top = std::max(int32_t(At32(r + 4)), vy);
  int32_t right = std::min(int32_t(At32(r + 8)), int32_t(uint32_t(vw) + uint32_t(vx)));
  int32_t bottom = std::min(int32_t(At32(r + 12)), int32_t(uint32_t(vh) + uint32_t(vy)));
  if (At32(d + 11576) != 0) {
    left = std::max(left, int32_t(At32(d + 12396))), top = std::max(top, int32_t(At32(d + 12400)));
    right = std::min(right, int32_t(At32(d + 12404))), bottom = std::min(bottom, int32_t(At32(d + 12408)));
  }
  if (right <= left || bottom <= top) return true;
  ctx.r1.u64 = frame;
  // The dirty state the clear's draws use, the bits uploaded cleared (not the whole words).
  if (const uint64_t state = At64(d + 16)) {
    if (At64(d + 40) & state) Set64(d + 16, Call(ctx, base, rt.flush_state, device, state, At32(d + 10432)));
    uint64_t s = At64(d + 16);
    if (s & 0xFFFu) Call(ctx, base, rt.upload_words, device, s << 52, 8704, device + 10420), Set64(d + 16, At64(d + 16) & ~0xFFFull);
    s = At64(d + 16);
    if (uint32_t(s) & 0x0001F000u)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 47, 4), 8576, device + 10400), Set64(d + 16, At64(d + 16) & ~0x1F000ull);
    s = At64(d + 16);
    if (s & 0x03FFFC0000000000ull)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 6, 15), 8192, device + 10240),
          Set64(d + 16, At64(d + 16) & ~0x03FFFC0000000000ull);
    s = At64(d + 16);
    if (s & 0x000003FFFFE00000ull)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 22, 20), 8448, device + 10316),
          Set64(d + 16, At64(d + 16) & ~0x000003FFFFE00000ull);
  }
  if (const uint64_t other = At64(d + 24); other & 0x007FFFFC00000000ull)
    Call(ctx, base, rt.upload_words, device, Rldicr(other, 9, 20), 8832, device + 10468),
        Set64(d + 24, At64(d + 24) & ~0x007FFFFC00000000ull);
  if (const uint64_t misc = At64(d + 32); misc & 0x0000003FFFFFFFFFull)
    Call(ctx, base, rt.upload_words, device, misc << 26, 8960, device + 10552),
        Set64(d + 32, At64(d + 32) & ~0x0000003FFFFFFFFFull);
  const auto room = [&] {
    const uint32_t cursor = At32(d + 48);
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  // The bin mask, an invalidation, the clear's vertex shader.
  Out out{m, room(), records};
  out.Bins(0, At32(d + 12428)), out.Invalidate(768), out.Shader(0, rt.clear_vertex_shader, 27);
  Set32(d + 48, out.cursor);
  // Its pixel shader, and VGT 0x2180-0x2181.
  out.cursor = room();
  out.Shader(1, rt.clear_pixel_shader, 9), out.Registers(0x2180, {0x10010001u, 0});
  Set32(d + 48, out.cursor);
  // The state of a clear.
  out.cursor = room();
  WriteClearState(out);
  Set64(d + 16, At64(d + 16) | (uint64_t(1) << 41) | (uint64_t(1) << 40) | (uint64_t(1) << 44) | 0xE0 | 0x180000);
  Set64(d + 32, At64(d + 32) | 0x80000);
  Set32(d + 48, out.cursor);
  const auto clear_target = [&](uint32_t target) {
    Set32(m.Host(frame + 92), stencil);
    // The rect as the runtime holds it: words loaded, zero-extended.
    ctx.r3.u64 = device, ctx.r4.u64 = flags, ctx.r5.s64 = int32_t(target), ctx.r6.u64 = uint32_t(left);
    ctx.r7.u64 = uint32_t(top), ctx.r8.u64 = uint32_t(right), ctx.r9.u64 = uint32_t(bottom), ctx.r10.u64 = colors;
    ctx.f1.f64 = depth;
    rt.clear_target(ctx, base);
  };
  if (!(flags & 0xF)) {
    clear_target(0xFFFFFFFFu);
  } else {
    out.cursor = room();
    out.Registers(0x2104, {0xFFFF}), out.Registers(0x2202, {0}), out.Registers(0x2201, {0x00010001u});
    Set32(d + 48, out.cursor);
    Set64(d + 16, At64(d + 16) | (uint64_t(1) << 37) | 512 | 1024);
    for (uint32_t target = 0; target < 4; ++target) {
      if (!((1u << target) & flags) || !At32(d + 12160 + 4 * target)) continue;
      clear_target(target);
      flags &= ~0x30u;
    }
  }
  // The viewport's corners: two signed 15-bit fields of each of two device words.
  const uint32_t vy2 = At32(d + 10312), vx2 = At32(d + 10308);
  ctx.r3.u64 = device;
  ctx.r4.s64 = int32_t(vx2 << 17) >> 17, ctx.r5.s64 = int32_t(vx2 << 1) >> 17;
  ctx.r6.s64 = int32_t(vy2 << 17) >> 17, ctx.r7.s64 = int32_t(vy2 << 1) >> 17;
  rt.restore_viewport(ctx, base);
  d[10813] |= 8;
  Set64(d + 16, At64(d + 16) | 0x20000);
  ctx.r1.u64 = caller_r1;
  return true;
}
inline void ClearOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && Clear(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

namespace detail {
// What the clear and the resolve upload of the dirty state before their own draws: the state group's helper and four
// word uploads, the misc group's and other's first uploads, each clearing only the bits it uploaded.
inline void FlushOverlayState(PPCContext& ctx, uint8_t* base, uint32_t device, uint8_t* d) {
  const Runtime& rt = g_runtime;
  if (const uint64_t state = At64(d + 16)) {
    if (At64(d + 40) & state) Set64(d + 16, Call(ctx, base, rt.flush_state, device, state, At32(d + 10432)));
    uint64_t s = At64(d + 16);
    if (s & 0xFFFu) Call(ctx, base, rt.upload_words, device, s << 52, 8704, device + 10420), Set64(d + 16, At64(d + 16) & ~0xFFFull);
    s = At64(d + 16);
    if (uint32_t(s) & 0x0001F000u)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 47, 4), 8576, device + 10400), Set64(d + 16, At64(d + 16) & ~0x1F000ull);
    s = At64(d + 16);
    if (s & 0x03FFFC0000000000ull)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 6, 15), 8192, device + 10240),
          Set64(d + 16, At64(d + 16) & ~0x03FFFC0000000000ull);
    s = At64(d + 16);
    if (s & 0x000003FFFFE00000ull)
      Call(ctx, base, rt.upload_words, device, Rldicr(s, 22, 20), 8448, device + 10316),
          Set64(d + 16, At64(d + 16) & ~0x000003FFFFE00000ull);
  }
  if (const uint64_t other = At64(d + 24); other & 0x007FFFFC00000000ull)
    Call(ctx, base, rt.upload_words, device, Rldicr(other, 9, 20), 8832, device + 10468),
        Set64(d + 24, At64(d + 24) & ~0x007FFFFC00000000ull);
  if (const uint64_t misc = At64(d + 32); misc & 0x0000003FFFFFFFFFull)
    Call(ctx, base, rt.upload_words, device, misc << 26, 8960, device + 10552),
        Set64(d + 32, At64(d + 32) & ~0x0000003FFFFFFFFFull);
}
// The device's render targets tile together (the bound surfaces the tiled ones, or the device always tiling).
inline bool TiledTargets(const uint8_t* d) {
  const uint8_t flags = d[10812];
  if (flags & 0x10) return true;
  if (!(flags & 0x20)) return false;
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t bound = At32(d + 12160 + 4 * k);
    if (At32(d + 12448 + 4 * k) != bound && bound != 0) return false;
  }
  return true;
}
// The viewport restore after the clear and the resolve: the device's two words of signed 15-bit corners.
inline void RestoreViewport(PPCContext& ctx, uint8_t* base, uint32_t device, const uint8_t* d) {
  const uint32_t vy2 = At32(d + 10312), vx2 = At32(d + 10308);
  ctx.r3.u64 = device;
  ctx.r4.s64 = int32_t(vx2 << 17) >> 17, ctx.r5.s64 = int32_t(vx2 << 1) >> 17;
  ctx.r6.s64 = int32_t(vy2 << 17) >> 17, ctx.r7.s64 = int32_t(vy2 << 1) >> 17;
  g_runtime.restore_viewport(ctx, base);
}
inline float Frsp(double v) { return float(v); }
}  // namespace detail

// The resolve (D3DDevice_Resolve): r3 device, r4 flags (bits 0-2 the source: color target 0-3 or 4 the depth;
// 0x70 the surface's EDRAM format, completed from the surface when none; 0x100 clear the color after, 0x200 the
// depth; 0x2000 ...), r5 the source rect or 0, r6 the destination texture, r7 the destination point or 0, r8 its
// level, r9 its slice, r10 the clear color or 0, f1 the clear depth, the caller's words at +92 and +100 the clear's
// stencil arguments. The copy registers from the texture's format, pitch and the point; the clears; the viewport
// widened over the rect; the dirty state; the tiles' copy addresses when tiled; the copy shader, the state of a copy,
// a rect of three vertices taken through the runtime's allocator and drawn in copy mode; the copy's cache events and
// wait; the texture's fence or release. As PM4 or as Any360's records.
inline bool Resolve(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  using any360::xdk::IsPlainMemory;
  const uint32_t device = ctx.r3.u32, caller_r1 = ctx.r1.u32, frame = caller_r1 - 384;
  uint32_t flags = ctx.r4.u32, rect = ctx.r5.u32;
  const uint32_t texture = ctx.r6.u32, level = ctx.r8.u32, slice = ctx.r9.u32;
  uint32_t point = ctx.r7.u32, colors = ctx.r10.u32;
  const double depth = ctx.f1.f64;
  if (!rt.make_room || !rt.surface_level || !rt.resolve_clear_color || !rt.resolve_clear_depth || !rt.allocate ||
      !rt.restore_viewport || !rt.release_block || !IsPlainMemory(device, 16384) || !IsPlainMemory(texture, 52) ||
      !IsPlainMemory(frame, 384 + 104) || Overlap(texture, 52, device, 16384) ||
      (rect && !IsPlainMemory(rect, 16)) || (point && !IsPlainMemory(point, 8)) || (colors && !IsPlainMemory(colors & ~0xFu, 16)))
    return false;
  uint8_t* d = m.Host(device);
  uint8_t* f = m.Host(frame);
  const uint8_t* t = m.Host(texture);
  const uint32_t source = flags & 7;
  if (!(flags & 0x70)) {
    const uint32_t surface = source == 4 ? At32(d + 12176) : At32(d + 12160 + 4 * source);
    if (!IsPlainMemory(surface + 24, 2)) return false;
    const uint32_t kind = (uint32_t(m.Host(surface + 24)[0]) << 8 | m.Host(surface + 25)[0]) & 3;
    flags |= kind == 0 ? 16 : kind == 1 ? 80 : 112;
  }
  // The texture's format word, size and tiling.
  const uint32_t t48 = At32(t + 48), t40 = At32(t + 40), t36 = At32(t + 36), t32 = At32(t + 32), t28 = At32(t + 28);
  const uint32_t tiling = (t48 >> 9) & 3;
  const uint32_t extra = ((t40 >> 31) << 1) + 1;
  uint32_t samples, width, height;
  if (tiling == 2) samples = 8, width = t36 & 0x7FF, height = (t36 >> 11) & 0x7FF;
  else samples = 1, width = t36 & 0x1FFF, height = (t36 >> 13) & 0x1FFF;
  height += extra, width += extra;
  const uint32_t format = t32 & 0x3F;
  uint32_t pitch = ((t28 >> 22) & 0x1FF) << 5;
  const uint32_t format_table_at = rt.resolve_format_table + 2 * format;
  if (!IsPlainMemory(format_table_at, 2)) return false;
  const uint32_t bytes = (uint32_t(m.Host(format_table_at)[0]) << 8 | m.Host(format_table_at + 1)[0]) >> 3 & 0x1F;
  if ((level || slice) && bytes == 0) return false;  // the runtime traps (twllei)
  // The function's own frame, as the runtime fills it (the level helper writes into it, and the release reads it).
  Set32(m.Host(caller_r1 + 44), texture);
  Set32(f + 132, source), Set32(f + 124, samples), Set32(f + 112, height), Set32(f + 116, width);
  Set32(f + 120, t32 & 0xFFFFF000u), Set32(f + 140, bytes), Set32(f + 136, rt.resolve_offset_float);
  const float default_color = AtFloat(m.Host(rt.clear_default_color));
  ctx.r1.u64 = frame;
  if (!colors) {
    colors = frame + 192;
    for (int i = 0; i < 4; ++i) Set32(f + 192 + 4 * i, std::bit_cast<uint32_t>(default_color));
  }
  uint32_t address_base = t32 & 0xFFFFF000u;
  if (level || slice) {
    Set32(f + 84, frame + 112), Set32(f + 92, frame + 144), Set32(f + 100, frame + 120);
    ctx.r3.u64 = texture, ctx.r4.u64 = slice & ~(samples - 1), ctx.r5.u64 = level, ctx.r6.u64 = frame + 128;
    ctx.r7.u64 = frame + 176, ctx.r8.u64 = frame + 160, ctx.r9.u64 = frame + 152, ctx.r10.u64 = frame + 116;
    rt.surface_level(ctx, base);
    height = At32(f + 112);
    address_base = At32(f + 120);
    pitch = At32(f + 128) / bytes;
  }
  if (!rect) {
    rect = frame + 176;
    Set32(f + 176, 0), Set32(f + 180, 0), Set32(f + 188, height), Set32(f + 184, At32(f + 116));
  }
  if (!point) point = rt.resolve_default_point;
  const uint8_t* r = m.Host(rect);
  const uint8_t* p = m.Host(point);
  const uint32_t px = At32(p), py = At32(p + 4), rx = At32(r), ry = At32(r + 4);
  const uint32_t rows_left = height - py;
  Set32(f + 128, rows_left);
  const uint32_t texture_kind = (t28 >> 2) & 3;
  const uint32_t address = ((py - ry) * pitch + (((px - rx) << 5) & ~0x1Fu)) * bytes +
                           ((((address_base >> 20) & 0xFFF) + 512) & 0x1000u) + (address_base & 0x1FFFFFFFu);
  const uint32_t endian_bits = t40 & 0xE;
  uint32_t copy_format = format;
  switch (format) {
    case 54: copy_format = 7; break;
    case 55: copy_format = 16; break;
    case 56: copy_format = 17; break;
    case 27: copy_format = 30; break;
    case 28: copy_format = 31; break;
    case 29: copy_format = 32; break;
    case 22: case 23: copy_format = 6; break;
    default: break;
  }
  uint32_t swap = texture_kind == 1 ? 1 : 0;
  if ((t40 & 1) == 1) swap += 2;
  switch (copy_format) {
    case 30: case 31: case 32: case 36: case 37: case 38: case 57: case 63: swap = 7; break;
    default: break;
  }
  uint32_t mode = ((flags >> 26) & 0x3F) | ((endian_bits != 0 ? 1u : 0u) << 8);
  swap = (swap & 7) | (((mode & 0x13F) << 3) & ~7u);
  uint32_t dest_info = (((copy_format & 0x3F) | ((swap << 6) & ~0x3Fu)) << 7) & ~0x7Fu;
  dest_info |= (t32 >> 6) & 3;
  const uint32_t dest_pitch = ((rows_left << 16) & 0x3FFF0000u) | (pitch & 0x3FFF);
  if (tiling == 2) {
    Set32(d + 10692, pitch * height);
    dest_info |= (((samples - 1) & slice) << 4) & 0x70;
    Set64(d + 32, At64(d + 32) | 4);
    dest_info |= 8;
  }
  const uint32_t sample_select = (flags & 0x2000) ? 3 : (source == 4 ? 0 : 1);
  const uint32_t copy_control = ((sample_select << 20) & 0x300000u) | ((flags & 0x377) - 16);
  Set32(d + 10660, dest_info), Set32(d + 10652, address), Set32(d + 10656, dest_pitch), Set32(d + 10648, copy_control);
  Set64(d + 32, At64(d + 32) | 0x3C00);
  const bool clear_depth = (flags & 0x200) != 0;
  if (clear_depth) {
    ctx.r3.u64 = device, ctx.r4.u64 = At32(m.Host(caller_r1 + 100)), ctx.r5.u64 = t40 & 1;
    ctx.r6.u64 = At32(m.Host(caller_r1 + 92)), ctx.f1.f64 = depth;
    rt.resolve_clear_depth(ctx, base);
  }
  if (flags & 0x100) {
    const uint8_t* c = m.Host(colors & ~0xFu);
    for (int i = 0; i < 16; ++i) ctx.v1.u8[i] = c[15 - i];
    ctx.r3.u64 = device, ctx.r4.u64 = At32(m.Host(caller_r1 + 100)), ctx.r5.u64 = copy_control & 7;
    rt.resolve_clear_color(ctx, base);
  }
  // The viewport widened over the rect, rounded to 8 pixels.
  const int32_t x1 = int32_t(At32(r)), y1 = int32_t(At32(r + 4));
  const int32_t x2 = int32_t((At32(r + 8) + 7) & ~7u), y2 = int32_t((At32(r + 12) + 7) & ~7u);
  bool viewport = false;
  {
    const uint32_t v1 = At32(d + 10308), v2 = At32(d + 10312);
    if (x1 < (int32_t(v1 << 17) >> 17) || y1 < (int32_t(v1 << 1) >> 17) || x2 > (int32_t(v2 << 17) >> 17) ||
        y2 > (int32_t(v2 << 1) >> 17)) {
      ctx.r3.u64 = device, ctx.r4.u64 = uint32_t(x1), ctx.r5.u64 = uint32_t(y1), ctx.r6.u64 = uint32_t(x2), ctx.r7.u64 = uint32_t(y2);
      rt.restore_viewport(ctx, base);
      viewport = true;
    }
  }
  FlushOverlayState(ctx, base, device, d);
  const auto room = [&](uint32_t cursor) {
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  Out out{m, At32(d + 48), records};
  if (TiledTargets(d)) {
    out.cursor = room(out.cursor);
    out.Bins(1, 0);
    for (uint32_t tile = 0; tile < At32(d + 12468); ++tile) {
      const uint32_t tx = At32(d + 12712 + 8 * tile), ty = At32(d + 12712 + 8 * tile + 4);
      out.Bins(0, 3u << (2 * tile));
      out.ContextConstant(0x319, (ty * pitch + ((tx << 5) & ~0x1Fu)) * bytes + address);
      Set32(d + 48, out.cursor);
      out.cursor = room(out.cursor);
    }
    out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
    Set32(d + 48, out.cursor);
  }
  out.cursor = room(At32(d + 48));
  if (d[10814] & 0x10) {
    for (int i = 0; i < 3; ++i) out.Event(0x46, {6});
    d[10814] &= 0xEF;
  }
  out.Invalidate(256), out.Shader(0, rt.resolve_pixel_shader, 15);
  out.Registers(0x2180, {0x00010002u, 0}), out.Registers(0x2208, {6}), out.Registers(0x2200, {0});
  uint32_t depth_state = 0;
  if (clear_depth) {
    const uint8_t b = d[10814];
    d[10813] |= 8;
    const uint32_t mixed = (b & ~0x10u) | ((b >> 2) & 0x10u);
    depth_state = ((mixed >> 4) & 7 & ~2u) | (At32(d + 10432) & ~0xFu);
    Set64(d + 16, At64(d + 16) | 0x20000);
  }
  out.Registers(0x5C8, {0x00020000u});
  Set32(d + 48, out.cursor);
  Call(ctx, base, rt.flush_state, device, 256, depth_state);
  out.cursor = room(At32(d + 48));
  WriteClearState(out);
  Set64(d + 16, At64(d + 16) | (uint64_t(1) << 41) | (uint64_t(1) << 40) | (uint64_t(1) << 44) | 0xE0);
  Set64(d + 32, At64(d + 32) | 0x80000);
  Set32(d + 48, out.cursor);
  if (texture_kind == 3 && source != 4) {
    const uint32_t target = source == 0 ? 0 : source + 1;
    const uint32_t info = At32(d + 10244 + 4 * target);
    if ((info & 0x000F0000u) == 0x10000u) {
      out.cursor = room(out.cursor);
      out.Registers(0x2001 + target, {(info & 0x03FFFFFFu) & ~0x000FF000u});
      Set64(d + 16, At64(d + 16) | ((uint64_t(1) << 63) >> ((target + 7) & 0xFF)));
      Set32(d + 48, out.cursor);
    }
  }
  out.cursor = room(out.cursor);
  const uint32_t vertices = uint32_t(Call(ctx, base, rt.allocate, device, 6, 4));
  if (!vertices) {
    Set32(d + 48, out.cursor);
    ctx.r1.u64 = caller_r1;
    return true;
  }
  const float offset = (At32(d + 10560) & 1) ? default_color : AtFloat(m.Host(rt.resolve_offset_float));
  const float fx = Frsp(double(int64_t(x1))), fy = Frsp(double(int64_t(y1)));
  const float fw = Frsp(double(int64_t(int32_t(uint32_t(x2) - uint32_t(x1))))), fh = Frsp(double(int64_t(int32_t(uint32_t(y2) - uint32_t(y1)))));
  const float left = float(double(fx) - double(offset)), top = float(double(fy) - double(offset));
  const float right = float(double(fw) + double(left)), bottom = float(double(fh) + double(top));
  uint8_t* vx = m.Host(vertices);
  const float v[6] = {left, top, right, top, right, bottom};
  for (int i = 0; i < 6; ++i) Set32(vx + 4 * i, std::bit_cast<uint32_t>(v[i]));
  const uint32_t vertex_address = GpuAddress(vertices, 0x1FFFFFFFu) | 3u;
  out.Registers(0x4800, {vertex_address, 0x1000001Au, 0, 0, 0, 0}), out.Registers(0x5000, {0, 0, 0});
  out.AutoDraw(0x00030088u, (At32(d + 12436) & 1) != 0);
  Set32(d + 48, out.cursor);
  out.cursor = room(out.cursor);
  out.Event(0x46, {6}), out.Registers(0x2007, {At32(d + 10268)});
  Set32(d + 48, out.cursor);
  if (viewport) RestoreViewport(ctx, base, device, d);
  const uint32_t copied = At32(f + 128) * pitch * At32(f + 140) * At32(f + 124);
  uint8_t* tex = m.Host(texture);
  Set32(tex + 8, At32(d + 10780));
  Set64(d + 16, At64(d + 16) | 0x80000 | 8);
  Set64(d + 24, At64(d + 24) | 0x80000000ull);
  Set64(d + 16, At64(d + 16) | 0x18000 | 2048 | 256);
  out.cursor = room(At32(d + 48));
  out.Registers(0xA31, {0x03000100u}), out.Registers(0xA2F, {(copied + 8191) & ~0xFFFu, address & ~0xFFFu});
  out.Wait(3, 2609, 0, 0x80000000u, 8);
  Set32(d + 10648, 0), Set32(d + 10660, 0);
  out.Registers(0x2318, {0}), out.Registers(0x231B, {0});
  Set32(d + 48, out.cursor);
  const uint32_t fence = At32(d + 10780);
  if (fence) {
    Set32(tex + 8, fence), Set32(tex + 12, fence);
  } else if (At32(d + 10784) & At32(tex)) {
    uint32_t record = At32(d + 13148);
    if (record >= At32(d + 13152)) record = uint32_t(Call(ctx, base, rt.release_block, device));
    const uint32_t first = (At32(f + 176) & 0x80000000u) | 0x40000000u | (texture >> 2);
    Set32(f + 176, first), Set32(f + 180, 0xFFFFFFFFu);
    Set32(m.Host(record), first), Set32(m.Host(record + 4), 0xFFFFFFFFu);
    Set32(d + 13148, record + 8);
  }
  ctx.r1.u64 = caller_r1;
  return true;
}
inline void ResolveOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && Resolve(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

// IDirect3DQuery9::Issue: r3 the query (+0 its device, +4 its type, +16 its state, +20 its fence, +24 its result
// blocks, +144 how many), r4 D3DISSUE_BEGIN (2) or END (1). Begin: the block count (the device's tile count when it
// tiles), the blocks marked unwritten (0xFFFFFEED), and an occlusion query (type 9) begun by the runtime's helper or
// an event query (10) as EVENT_WRITE 25; state 2. End: the occlusion query ended by the helper or the event query's
// EVENT_WRITE_EXT 26 into its block; state 1 (not for type 8) and the device's pending fence. The events as PM4 or as
// Any360's event records.
inline bool QueryIssue(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  namespace record = any360::xdk::record;
  const Runtime& rt = g_runtime;
  const uint32_t query = ctx.r3.u32, flags = ctx.r4.u32, caller_r1 = ctx.r1.u32;
  if (!rt.make_room || !rt.query_occlusion || !any360::xdk::IsPlainMemory(query, 148)) return false;
  uint8_t* q = m.Host(query);
  const uint32_t device = At32(q);
  if (!any360::xdk::IsPlainMemory(device, 16384) || Overlap(query, 148, device, 16384) ||
      !any360::xdk::IsPlainMemory(caller_r1 - 48, 12))
    return false;
  uint8_t* d = m.Host(device);
  const uint32_t type = At32(q + 4);
  const bool begin = (flags & 2) != 0, end = !begin && (flags & 1);
  const uint32_t blocks = begin ? ((d[10812] & 0x20) ? At32(d + 12468) : 1) : 0;
  if (begin && type == 9) {
    if (blocks > 30) return false;
    for (uint32_t i = 0; i < blocks; ++i) {
      const uint32_t block = At32(q + 24 + 4 * i);
      if (!any360::xdk::IsPlainMemory(block, 16) || Overlap(block, 16, query, 148) || Overlap(block, 16, device, 16384))
        return false;
    }
  }
  const uint32_t event_block = At32(q + 24);
  if (begin && type == 10 &&
      (!any360::xdk::IsPlainMemory(event_block, 4) || Overlap(event_block, 4, query, 148) ||
       Overlap(event_block, 4, device, 16384)))
    return false;
  if (!begin && !end) return true;
  Set32(m.Host(caller_r1 - 48), device), Set32(m.Host(caller_r1 - 44), query);  // the function's own stack words
  ctx.r1.u64 = caller_r1 - 128;
  const auto cursor_now = [&] {
    const uint32_t cursor = At32(d + 48);
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  if (begin) {
    Set32(q + 144, blocks);
    if (type == 9) {
      for (uint32_t i = 0; i < blocks; ++i) {
        uint8_t* block = m.Host(At32(q + 24 + 4 * i));
        for (int k = 0; k < 4; ++k) Set32(block + 4 * k, 0xFFFFFEEDu);
      }
      Call(ctx, base, rt.query_occlusion, device, query, 1);
    } else if (type == 10) {
      Set32(m.Host(event_block), 0xFFFFFEEDu);
      Set32(m.Host(caller_r1 - 40), event_block);
      const uint32_t cursor = cursor_now();
      uint8_t* out = m.Host(cursor + 4);
      if (records) {
        Set32(out, record::Header(record::EventWords(1)));
        const uint32_t words[2] = {record::KindWord(record::kEvent, 0x46), 25};
        std::memcpy(out + 4, words, 8);
        Set32(d + 48, cursor + 12);
      } else {
        Set32(out, 0xC0004600u), Set32(out + 4, 25);
        Set32(d + 48, cursor + 8);
      }
      d[10814] |= 1;
    }
    Set32(q + 16, 2);
  } else {
    if (type != 8) {
      if (type == 9) {
        Call(ctx, base, rt.query_occlusion, device, query, 0);
      } else if (type == 10) {
        const uint32_t cursor = cursor_now();
        const uint32_t address = GpuAddress(At32(q + 24), 0x1FFFFFFEu) | 1u;
        uint8_t* out = m.Host(cursor + 4);
        if (records) {
          Set32(out, record::Header(record::EventWords(2)));
          const uint32_t words[3] = {record::KindWord(record::kEvent, 0x5A), 26, address};
          std::memcpy(out + 4, words, 12);
          Set32(d + 48, cursor + 16);
        } else {
          Set32(out, 0xC0015A00u), Set32(out + 4, 26), Set32(out + 8, address);
          Set32(d + 48, cursor + 12);
        }
        d[10814] &= 0xFE;
      }
      Set32(q + 16, 1);
    }
    Set32(q + 20, At32(d + 10780));
  }
  ctx.r1.u64 = caller_r1;
  return true;
}
inline void QueryIssueOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && QueryIssue(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
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
