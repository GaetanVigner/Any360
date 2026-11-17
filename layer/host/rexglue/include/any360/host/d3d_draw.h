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
#include <cstring>
#include <thread>

#include "any360/xdk/d3d_records.h"
#include "any360/xdk/d3d_state.h"
#include "any360/xdk/d3d_tiling.h"
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
  GuestFunction constant_list = nullptr;     // r3 device, r4 a shader part, r5 its data's base: the part's literal
                                             // constants (LOAD_ALU_CONSTANT; a title may hook it)
  // The vertex fetch patcher's swizzle table (8 half-words) and fetch format sizes (64 bytes): guest addresses.
  uint32_t fetch_swizzles = 0, fetch_sizes = 0;
  // The target clear's color packer (r3 the format kind, r4 its bits 20-25, r5/r6 the packed words out, v1 the
  // color), and the doubles that make a fast clear's depth from its packed color: guest addresses.
  GuestFunction clear_color_pack = nullptr;
  GuestFunction list_block = nullptr;  // r3 one of the device's lists (+12960, +12980) -> r3 its new block's cursor
  uint32_t swap_word = 0;              // the swap packet's fence word (a title address the runtime passes)
  uint32_t device_slot = 0;            // the title's global that leads to the device (**slot)
  GuestFunction block_on_fence = nullptr;  // r3 device, r4 a fence, r5 an argument: waits until the GPU passed it
  GuestFunction ring_space = nullptr;  // r3 device, r4 the ring's write index, r5 dwords: waits for that ring space
  GuestFunction ring_submit = nullptr;     // r3 device, r4 the segment's {dwords, address}, r5 1 (RingSubmit's)
  GuestFunction segment_submit = nullptr;  // r3 device, r4 end, r5 address, r6 dwords, r7 count, r8 list (SegmentSubmit's)
  GuestFunction segment_room = nullptr;    // r3 device, r4 0: a new segment from the allocator (the command buffer full)
  GuestFunction pending_segment = nullptr;  // r3 device, r4 -> its address, r5 -> its dwords: the segment written so far
  GuestFunction fence = nullptr;            // r3 device, r4 cursor, r5 flags, r6 r7 the interrupt's words (Fence's)
  uint32_t tiled_callback = 0;              // the interrupt callback a tiled recording's end fence names (its lists)
  // BeginTiling / EndTiling's helpers (the runtime's or Any360's where bound).
  GuestFunction set_predication = nullptr;     // r3 device, r4 the predication mask
  GuestFunction tile_samples = nullptr;        // r3 device, r4 the samples' bits (+11572)
  GuestFunction resolve = nullptr;             // the resolve (D3D Resolve's)
  GuestFunction viewport_of = nullptr;         // r3 device, r4 a surface: the viewport and scissor set to it
  GuestFunction scissor_of = nullptr;          // r3 device, r4 a rect
  GuestFunction set_viewport = nullptr;        // r3 device, f1-f6 and r10 the viewport (+12368's copy)
  GuestFunction clear = nullptr;               // the clear's rect wrapper
  GuestFunction tiling_failed = nullptr;       // r3 device, r4 0: after a tiled recording the device could not end
  GuestFunction begin_tiled_recording = nullptr, end_tiled_recording = nullptr;
  uint32_t tiling_surface = 0, tiling_rect = 0;  // the runtime's whole-screen surface and rect
  uint32_t pix_slot = 0, pix_slot_2 = 0;         // the profiler's globals (a callback set there: the runtime's)
  // The tile ops interpreter's (any360/xdk/d3d_tiling.h).
  GuestFunction tile_select = nullptr;     // r3 the interpreter's state (TileSelect's)
  GuestFunction tile_resources = nullptr;  // r3 the header, r4 a resource list, r5 1: flushed (sub_826CD9F8)
  GuestFunction tile_wait = nullptr;       // r3 the interpreter's state: the GPU waited for (sub_826CD950)
  GuestFunction lock_l2 = nullptr, unlock_l2 = nullptr;  // KeLockL2, KeUnlockL2
  uint32_t clear_depth_offset = 0, clear_depth_scale = 0;
  // The resolve's format table (a half-word a format), default destination point, copy pixel shader (15 dwords) and
  // vertex offset float: guest addresses in the title's image.
  uint32_t resolve_format_table = 0, resolve_default_point = 0, resolve_pixel_shader = 0, resolve_offset_float = 0;
  uint32_t primitive_table = 0;              // guest address: [divisor, overlap] a primitive type
  // The clear's vertex shader (27 dwords) and pixel shader (9 dwords), and the color it clears to when given none
  // (one float for the four channels): guest addresses in the title's image.
  uint32_t clear_vertex_shader = 0, clear_pixel_shader = 0, clear_default_color = 0;
  GuestFunction destroy_resource = nullptr;  // r3 a resource whose count reached 0: destroyed (D3DResource_Release's)
};
inline Runtime g_runtime;
// any360_d3d_native_draws and any360_d3d_records (d3d_draw.cpp).
bool NativeDraws();
bool DrawRecords();
bool DirectSubmit();  // any360_direct_submit (d3d_draw.cpp)
bool NativeSegments();  // any360_native_segments (d3d_draw.cpp)
bool NativeTiling();  // any360_native_tiling (d3d_draw.cpp)
bool NativeRefCounts();  // any360_d3d_native_refcounts (d3d_draw.cpp)
void CountBracket(bool native);  // a BeginTiling taken by Any360 or left to the runtime (TilingBrackets)
void CountTileRun(bool native);  // a tile ops run, Any360's or the runtime's (TileOpRuns)
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
  // SET_CONSTANT (predicated) of context registers from `index` (0x2000 + index).
  void ContextConstants(uint32_t index, std::initializer_list<uint32_t> values) {
    const uint32_t n = uint32_t(values.size());
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::RegistersWords(n), true));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kRegisters, 0x2000 + index));
      for (uint32_t v : values) Host(v);
    } else {
      Word(0xC0002D01u | (n << 16)), Word(0x00040000u | index);
      for (uint32_t v : values) Word(v);
    }
  }
  void ContextConstant(uint32_t index, uint32_t value) { ContextConstants(index, {value}); }
  // IM_LOAD: the microcode at `address_type` (its low 2 bits the shader type), `dwords` of it.
  void ShaderLoad(uint32_t address_type, uint32_t dwords, bool predicated) {
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::kShaderLoadWords, predicated));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kShaderLoad, 0)), Host(address_type), Host(dwords);
    } else {
      Word(0xC0012700u | (predicated ? 1u : 0u)), Word(address_type), Word(dwords);
    }
  }
  // LOAD_ALU_CONSTANT.
  void ConstantLoad(uint32_t address, uint32_t offset_type, uint32_t dwords) {
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::kConstantLoadWords));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kConstantLoad, 0)), Host(address), Host(offset_type),
          Host(dwords);
    } else {
      Word(0xC0022F00u), Word(address), Word(offset_type), Word(dwords);
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
  void Event(uint32_t opcode, std::initializer_list<uint32_t> words, bool predicated = false) {
    const uint32_t n = uint32_t(words.size());
    if (records) {
      Word(any360::xdk::record::Header(any360::xdk::record::EventWords(n), predicated));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kEvent, opcode));
      for (uint32_t v : words) Host(v);
    } else {
      Word(0xC0000000u | ((n - 1) << 16) | (opcode << 8) | (predicated ? 1u : 0u));
      for (uint32_t v : words) Word(v);
    }
  }
  // INDIRECT_BUFFER of `dwords` at `address`, its header's other bits `bits` (bit 0 the predicate; any other: PM4).
  void Indirect(uint32_t address, uint32_t dwords, uint32_t bits) {
    if (records && !(bits & ~1u)) {
      Word(any360::xdk::record::Header(any360::xdk::record::kIndirectWords, bits & 1));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kIndirect, 0)), Host(address), Host(dwords);
    } else {
      Word(0xC0013F00u | bits), Word(address), Word(dwords);
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
  void Wait(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, bool predicated = false) {
    if (records) {
      Word(any360::xdk::record::Header(6, predicated));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kWait, 0));
      for (uint32_t v : {a, b, c, d, e}) Host(v);
    } else {
      Word(0xC0043C00u | (predicated ? 1u : 0u));
      for (uint32_t v : {a, b, c, d, e}) Word(v);
    }
  }
  // INTERRUPT of the CPUs `cpus` names.
  void Interrupt(uint32_t cpus, bool predicated) {
    if (records) {
      Word(any360::xdk::record::Header(2, predicated));
      Host(any360::xdk::record::KindWord(any360::xdk::record::kInterrupt, 0)), Host(cpus);
    } else {
      Word(0xC0005400u | (predicated ? 1u : 0u)), Word(cpus);
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

// sub_826C05C8 (the occlusion query's begin and end): r3 device, r4 the query, r5 the slot (0 end, 1 begin). The
// query's mode registers (0x2208 = 6, 0x2000-0x2001), the address its sample count goes to (0x2325: the query's
// buffer, +24 on, a tile each when it has more than one (+144), each predicated under its tile's bins, the device's
// bins put back after; 32 bytes a slot), an EVENT_WRITE_ZPD (21), then the state groups it changed marked dirty.
// Declines when the cursor needs room first or would during the tiles, or the query has more than 16 tiles.
inline bool QueryOcclusion(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  using any360::xdk::IsPlainMemory;
  const uint32_t device = ctx.r3.u32, query = ctx.r4.u32, slot = ctx.r5.u32 << 5;
  if (!IsPlainMemory(device, 16384) || !IsPlainMemory(query, 148)) return false;
  uint8_t* d = m.Host(device);
  const uint8_t* q = m.Host(query);
  const uint32_t cursor = At32(d + 48), limit = At32(d + 56), tiles = At32(q + 144);
  if (cursor > limit || tiles > 16 || (tiles > 1 && !IsPlainMemory(query + 24, 4 * tiles))) return false;
  const uint32_t per_tile = records ? 6 : 5, most = 4 * (32 + per_tile * tiles);
  if (tiles > 1 && uint64_t(cursor) + 4 * ((records ? 3 : 2) + 4 + per_tile * tiles) > limit) return false;
  if (uint64_t(cursor) + most > At32(d + 52) || !IsPlainMemory(cursor + 4, most)) return false;
  Out out{m, cursor, records};
  out.Registers(0x2208, {6}), out.Registers(0x2000, {0x20000, 0});
  if (tiles <= 1) {
    out.Registers(0x2325, {GpuAddress(At32(q + 24) + slot, 0x1FFFFFFFu)});
  } else {
    out.Bins(1, 0);
    for (uint32_t i = 0; i < tiles; ++i)
      out.Bins(0, 3u << (2 * i)), out.ContextConstant(0x325, GpuAddress(At32(q + 24 + 4 * i) + slot, 0x1FFFFFFFu));
    out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
  }
  out.Event(0x5B, {21});
  Set32(d + 48, out.cursor);
  Set64(d + 16, At64(d + 16) | 8 | uint64_t(1) << 57 | uint64_t(1) << 56);
  Set64(d + 32, At64(d + 32) | 0x2000);
  return true;
}
inline void QueryOcclusionOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && QueryOcclusion(ctx, m, DrawRecords())) return;
  original(ctx, base);
}

namespace detail {
// sub_826C6A50's constant list of a shader part at `part` (its +20: the list's offset from the part; the list: +16
// its entries' bytes, the entries from +20, 8 bytes each: u16 the first constant (in 4-dword units), u16 the dwords
// (0 ends the list), u32 the values' address from `base`). The entries it writes, or -1 when its memory is not plain
// or it is longer than 4096.
inline int ConstantListEntries(const any360::xdk::Memory& m, uint32_t part) {
  if (!any360::xdk::IsPlainMemory(part + 20, 4)) return -1;
  const uint32_t offset = At32(m.Host(part + 20));
  if (!offset) return 0;
  const uint32_t list = part + offset;
  if (!any360::xdk::IsPlainMemory(list + 16, 4)) return -1;
  uint32_t at = list + 20;
  const uint32_t end = at + At32(m.Host(list + 16));
  int entries = 0;
  for (; at < end; at += 8, ++entries) {
    if (entries > 4096 || !any360::xdk::IsPlainMemory(at, 8)) return -1;
    if (!(m.Host(at)[2] | m.Host(at)[3])) break;
  }
  return entries;
}
// The list as LOAD_ALU_CONSTANT packets (or records).
inline void WriteConstantList(const any360::xdk::Memory& m, uint32_t part, uint32_t base, Out& out) {
  const uint32_t offset = At32(m.Host(part + 20));
  if (!offset) return;
  const uint32_t list = part + offset;
  uint32_t at = list + 20;
  const uint32_t end = at + At32(m.Host(list + 16));
  for (; at < end; at += 8) {
    const uint8_t* e = m.Host(at);
    const uint32_t first = uint32_t(e[0]) << 8 | e[1], dwords = uint32_t(e[2]) << 8 | e[3];
    if (!dwords) break;
    out.ConstantLoad(GpuAddress(At32(e + 4) + base, 0x1FFFFFFFu), first << 2, dwords);
  }
}
// sub_826C5580 (the shader helper's bit 17): bit 0 of the device's +10432 from +10814 and +10420, bit 1 from the
// pixel shader's and +11572's, +10420's modes; when bit 1 is on and +10813's bit 3 was, an EVENT_WRITE 15 (bit 3
// cleared); else bit 3 set for the modes that need it.
inline void UpdateShaderControl(const any360::xdk::Memory& m, uint8_t* d, Out& out) {
  uint32_t pixel_bit = 0;
  if (const uint32_t ps = At32(d + 12412)) pixel_bit = (At32(m.Host(ps + At32(m.Host(ps + 64)) + 48)) >> 27) & 1;
  const uint32_t control = At32(d + 11572) >> 20, state = At32(d + 10420), shifted = state >> 1;
  const uint32_t word = (At32(d + 10432) & ~1u) | ((uint32_t(d[10814]) >> 6) & shifted & 1);
  Set32(d + 10432, word);
  uint32_t bit;
  if (!(control & 2)) {
    bit = control & 1;
  } else {
    const uint32_t mode = (state >> 4) & 7;
    const bool fits = (word & 0x10) ? (mode == 4 || mode == 6 || mode == 2) : (mode == 1 || mode == 3 || mode == 2);
    bit = (word & 1) & (pixel_bit == 0) & (shifted & 1) & uint32_t(fits);
    if (state & 1) {
      bit &= uint32_t(!(state & 0x3800u)) & uint32_t(!(state & 0xE0000u));
      if (state & 0x80) bit &= uint32_t(!(state & 0xE0000000u)) & uint32_t(!(state & 0x03800000u));
    }
  }
  const uint32_t now = (At32(d + 10432) & ~2u) | (bit << 1);
  Set32(d + 10432, now);
  if (now & 2) {
    if (d[10813] & 8) d[10813] &= 0xF7, out.Event(0x46, {15});
  } else if ((shifted & 1) && (state & 4)) {
    const uint32_t mode = (state >> 4) & 7;
    if (mode == 7 || mode == 5 || ((now & 0x10) ? (mode == 1 || mode == 3) : (mode == 4 || mode == 6))) d[10813] |= 8;
  }
}
}  // namespace detail

// sub_826C6A50 (the shaders' literal constants): r3 device, r4 a shader part, r5 its data's base; the part's constant
// list as LOAD_ALU_CONSTANT packets or records. Declines when the list's memory is not plain or the cursor would need
// room.
inline bool ConstantList(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32;
  if (!any360::xdk::IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  const int entries = ConstantListEntries(m, ctx.r4.u32);
  if (entries < 0) return false;
  if (!entries) return true;
  const uint32_t cursor = At32(d + 48), bytes = 20 * uint32_t(entries);
  if (cursor > At32(d + 56) || uint64_t(cursor) + bytes > At32(d + 56) || !any360::xdk::IsPlainMemory(cursor + 4, bytes))
    return false;
  Out out{m, cursor, records};
  WriteConstantList(m, ctx.r4.u32, ctx.r5.u32, out);
  Set32(d + 48, out.cursor);
  return true;
}
inline void ConstantListOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && ConstantList(ctx, m, DrawRecords())) return;
  original(ctx, base);
}

namespace detail {
// sub_826C6B00's check of a vertex shader's patched copy `which` (the shader's +which*416: +40 the patch set it was
// made for, +48/+56 the device state it was made for, +64 the GPU frame it was last used): kHit when it was made for
// the device's patch set (+11540, its +48) and state (+12248, +12256, under the patch set's masks +32, +40); kBusy when
// not, and the GPU still reads it (its frame after the frame the GPU is done with: +10780 now, *+10768 done); kPatch
// otherwise (the runtime patches the microcode again: sub_826C6010).
enum PatchCache { kHit, kBusy, kPatch };
inline PatchCache CheckPatchCache(const any360::xdk::Memory& m, const uint8_t* d, uint32_t vs, uint32_t which) {
  const uint8_t* e = m.Host(vs + which * 416);
  const uint8_t* patch = m.Host(At32(d + 11540));
  if (At32(e + 40) == At32(patch + 48) &&
      !(((At64(e + 48) ^ At64(d + 12248)) & At64(patch + 32)) | ((At64(e + 56) ^ At64(d + 12256)) & At64(patch + 40))))
    return kHit;
  if (const uint32_t used = At32(e + 64)) {
    const uint32_t now = At32(d + 10780), done = At32(m.Host(At32(d + 10768)));
    if (now - used < now - done) return kBusy;
  }
  return kPatch;
}
// Its hit: the copy marked used this frame, the device's patched state (+11552) the state it was made for.
inline void UsePatchedCopy(const any360::xdk::Memory& m, uint8_t* d, uint32_t vs, uint32_t which) {
  Set32(m.Host(vs + which * 416 + 64), At32(d + 10780));
  std::memcpy(d + 11552, d + 12248, 16);
}
}  // namespace detail

namespace detail {
// PowerPC's rotate-and-mask (mb..me in its bit order, bit 0 the top; mb > me wraps).
constexpr uint32_t Mask32(int mb, int me) {
  const uint32_t low = ~0u >> mb, high = ~0u << (31 - me);
  return mb <= me ? (low & high) : (low | high);
}
constexpr uint32_t Rlwinm(uint32_t v, int sh, int mb, int me) { return std::rotl(v, sh) & Mask32(mb, me); }
constexpr uint32_t Rlwimi(uint32_t a, uint32_t s, int sh, int mb, int me) {
  return (std::rotl(s, sh) & Mask32(mb, me)) | (a & ~Mask32(mb, me));
}

// A vertex shader's fetch instructions as sub_826C6010 makes them: three words a fetch, in the order of its list.
struct VertexFetches {
  static constexpr uint32_t kMost = 41;  // the runtime's stack buffer (480 bytes)
  uint32_t count = 0, list = 0;
  uint32_t words[kMost][3];
};
// sub_826C6010 (r3 vs, r4 destination, r5 the patch set, r6 the device's state bytes (+12248), r7 which copy): each
// fetch of the shader's copy `which` (its original three words at vs + which * 416 + 68 on, its list at its part
// (+896 + 8 * which) + 36 + 4 * part[+24], part[+28] of them) remade from the patch set's element of the same usage
// (+52 on, 12 bytes each, +24 of them; none: the default), the swizzle through the title's table, then each group
// of fetches (between list words with bits 20-21) sorted by offset and merged into mini-fetches. `frame`: the
// runtime's stack frame (it reads an unset halfword at +80, of which one bit reaches the swizzle masks: read here as
// the runtime reads it). False when the inputs are not plain memory or there are more fetches than its buffer holds.
inline bool ComputeVertexFetches(const any360::xdk::Memory& m, uint32_t vs, uint32_t patch, uint32_t state,
                                 uint32_t which, uint32_t frame, VertexFetches& out) {
  using any360::xdk::IsPlainMemory;
  const Runtime& rt = g_runtime;
  const auto L32 = [&](uint32_t a) { return At32(m.Host(a)); };
  const auto L16 = [&](uint32_t a) { const uint8_t* p = m.Host(a); return uint32_t(p[0]) << 8 | p[1]; };
  const auto L8 = [&](uint32_t a) { return uint32_t(m.Host(a)[0]); };
  if (!rt.fetch_swizzles || !rt.fetch_sizes || !IsPlainMemory(rt.fetch_swizzles, 16) || !IsPlainMemory(rt.fetch_sizes, 64) ||
      !IsPlainMemory(patch + 24, 4) || !IsPlainMemory(vs + 896 + 8 * which, 4))
    return false;
  const uint32_t elements = L32(patch + 24);
  if (elements > 1024 || (elements && !IsPlainMemory(patch + 52, 12 * elements))) return false;
  const uint32_t part = vs + L32(vs + 896 + 8 * which) + 872;
  if (!IsPlainMemory(part + 24, 8)) return false;
  const uint32_t n = L32(part + 28), list = part + 4 * (L32(part + 24) + 9), originals = vs + which * 416 + 68;
  if (n > VertexFetches::kMost) return false;
  if (n && (!IsPlainMemory(list, 4 * n) || !IsPlainMemory(originals, 12 * n) || !IsPlainMemory(frame + 80, 2))) return false;
  for (uint32_t i = 0; i < elements; ++i)
    if (!IsPlainMemory(state + L16(patch + 52 + 12 * i), 1)) return false;
  if (!IsPlainMemory(state, 1)) return false;
  out.count = n, out.list = list;
  const auto swizzle = [&](uint32_t offset) { return L16(rt.fetch_swizzles + offset); };
  const auto size = [&](uint32_t format) { return L8(rt.fetch_sizes + (format & 0x3F)); };
  uint32_t carry = n ? L16(frame + 80) : 0;  // the runtime's r8: the previous fetch's mask bits
  for (uint32_t k = 0; k < n; ++k) {
    uint32_t w0 = L32(originals + 12 * k), w1 = L32(originals + 12 * k + 4), w2 = L32(originals + 12 * k + 8);
    const uint32_t word = L32(list + 4 * k);
    uint32_t element = 0;
    for (uint32_t i = 0; i < elements && !element; ++i) {
      const uint32_t e = patch + 52 + 12 * i;
      if (L8(e + 9) == ((word >> 12) & 0xF) && L8(e + 10) == ((word >> 16) & 0xF)) element = e;
    }
    uint32_t bits, control, last;
    if (!element) {
      bits = (carry & 0xF) | 0x9250;
      w0 = Rlwimi(w0, 95, 20, 2, 11);
      control = (w1 & 0xBFC6CFFFu) | 0x60000;
      last = (w2 & 0x80000000u) | L8(state);
    } else {
      const uint32_t format = L32(element + 4);
      uint32_t t = Rlwinm(format, 22, 26, 28) | Rlwinm(format, 16, 29, 31);
      t = Rlwinm(t, 3, 0, 28) | Rlwinm(format, 13, 29, 31);
      t = Rlwinm(t, 1, 0, 30) | Rlwinm(format, 0, 16, 21);
      bits = (Rlwinm(t, 3, 0, 28) | (carry & 0xF)) & 0xFFFF;
      if (Rlwinm(format, 0, 24, 25) == 64) {
        uint32_t v = bits;
        if ((v & 0xE000) <= 0x6000) bits = ((v & 0xE000) ^ 0x2000) | (v & 0x1FFF);
        v = bits;
        if ((v & 0x1C00) <= 0xC00) bits = ((v & 0x1C00) ^ 0x400) | (v & 0xE3FF);
        v = bits;
        if ((v & 0x380) <= 0x180) bits = ((v & 0x380) ^ 0x80) | (v & 0xFC7F);
        v = bits;
        if ((v & 0x70) <= 0x30) bits = ((v & 0x70) ^ 0x10) | (v & 0xFF8F);
      }
      const uint32_t offset = L16(element), stride = L16(element + 2);
      control = Rlwinm(Rlwinm(format, 12, 14, 19) | Rlwinm(format, 0, 22, 23), 4, 0, 27) | (w1 & 0xBFC0CFFFu);
      const uint32_t product = (95 - offset) * 0x01560056u;
      last = Rlwinm(stride, 6, 1, 23) | L8(state + offset) | (w2 & 0x80000000u);
      w0 = Rlwinm(Rlwimi(w0, Rlwimi(product, product, 11, 8, 12), 1, 5, 11), 0, 5, 1);
    }
    bits &= 0xFFFF;
    const uint32_t mask = (bits | 14) & 0xFFFF;
    carry = bits | 14;
    // The four swizzle fields through the title's table, each its bits spread over the field.
    uint32_t x = swizzle(Rlwinm(control, 1, 28, 30)) & mask;
    uint32_t y = (x >> 3) | x;
    y = (y >> 3) | x, y = (y >> 3) | x, y = (y >> 3) | x;
    control = Rlwimi(control, y, 31, 29, 31);
    const uint32_t first = control;
    x = swizzle(Rlwinm(first, 30, 28, 30)) & mask;
    y = (x >> 3) | x, y = (y >> 3) | x, y = (y >> 3) | x;
    control = Rlwimi((y >> 1) | (x << 2), first, 0, 29, 25);
    x = swizzle(Rlwinm(control, 27, 28, 30)) & mask;
    y = (x >> 3) | x, y = (y >> 3) | x;
    const uint32_t third = Rlwimi((y >> 1) | (((x << 3) | x) << 2), control, 0, 26, 22);
    x = swizzle(Rlwinm(third, 24, 28, 30)) & mask;
    const uint32_t fourth = ((((x << 3) | x) << 3 | x) << 2) | (((x >> 3) | x) >> 1);
    out.words[k][0] = w0, out.words[k][1] = Rlwimi(fourth, third, 0, 23, 19), out.words[k][2] = last;
  }
  // The groups: from a list word with bits 20-21 to the next one, sorted then merged.
  const auto group_bit = [&](uint32_t k) { return (L32(list + 4 * k) & 0x300000u) != 0; };
  const auto key = [](uint32_t w) { return Rlwinm(w, 14, 25, 29) | Rlwinm(w, 7, 30, 31); };
  const auto offset_of = [](uint32_t w) { return uint32_t(int32_t(w << 1) >> 9); };
  uint32_t at = 0;
  while (at < n) {
    while (at < n && !group_bit(at)) ++at;
    if (at >= n || at + 1 >= n) break;
    const uint32_t start = at;
    uint32_t end = at + 1;
    while (end < n && !group_bit(end)) ++end;
    if (end >= n) break;
    const uint32_t span = end - start + 1;
    at = end + 1;
    int32_t fixed = -1;
    uint32_t sorted = span, passes = span - 1;
    if ((L32(list + 4 * end) & 0x300000u) == 0x300000u) {
      fixed = int32_t(key(out.words[end][0]));
      sorted = span - 1, passes = span - 2;
    }
    for (uint32_t pass = 1, i = start; passes && pass <= passes; ++pass, ++i) {
      if (pass >= sorted) continue;
      for (uint32_t j = i + 1, left = sorted - pass; left; --left, ++j) {
        const uint32_t a = key(out.words[i][0]), b = key(out.words[j][0]);
        int32_t order = int32_t(b - a);
        bool swap;
        if (order == 0) {
          order = int32_t(offset_of(out.words[i][2]) - offset_of(out.words[j][2]));
          if (order == 0) order = int32_t(size(out.words[i][1] >> 16)) - int32_t(size(out.words[j][1] >> 16));
          swap = order > 0;
        } else if (fixed == int32_t(a)) {
          swap = true;
        } else if (fixed == int32_t(b)) {
          swap = false;
        } else {
          swap = order > 0;
        }
        if (swap) std::swap(out.words[i], out.words[j]);
      }
    }
    for (uint32_t base = start, i = start + 1; i < start + span; ++i) {
      const uint32_t a = out.words[base][0], b = out.words[i][0];
      bool merged = false;
      if ((Rlwimi(a, a, 25, 12, 13) & Mask32(7, 13)) == (Rlwimi(b, b, 25, 12, 13) & Mask32(7, 13))) {
        const uint32_t from = offset_of(out.words[base][2]), to = offset_of(out.words[i][2]);
        if (from <= to) {
          uint32_t reach = size(out.words[i][1] >> 16) - from + to;
          if (reach <= 8) {
            const uint32_t now = Rlwinm(a, 5, 29, 31);
            reach -= 1;
            if (now > reach) reach = now;
            out.words[base][0] = Rlwimi(a, reach, 27, 2, 4);
            out.words[i][1] |= 0x40000000u;
            merged = true;
          }
        }
      }
      if (!merged) base = i;
    }
  }
  return true;
}
// Whether every fetch's destination (its list word's low 12 bits, 12 bytes each from `to`) is plain memory within
// `bytes` of `to`.
inline bool VertexFetchesFit(const any360::xdk::Memory& m, const VertexFetches& f, uint32_t to, uint32_t bytes) {
  for (uint32_t k = 0; k < f.count; ++k) {
    const uint32_t index = At32(m.Host(f.list + 4 * k)) & 0xFFF;
    if (12 * index + 12 > bytes || !any360::xdk::IsPlainMemory(to + 12 * index, 12)) return false;
  }
  return true;
}
inline void WriteVertexFetches(const any360::xdk::Memory& m, uint32_t to, const VertexFetches& f) {
  for (uint32_t k = 0; k < f.count; ++k) {
    uint8_t* p = m.Host(to + 12 * (At32(m.Host(f.list + 4 * k)) & 0xFFF));
    Set32(p, f.words[k][0]), Set32(p + 4, f.words[k][1]), Set32(p + 8, f.words[k][2]);
  }
}
}  // namespace detail

// sub_826C6010 as a function (the self-test's): r3 vs, r4 the destination, r5 the patch set, r6 the state bytes,
// r7 which copy.
inline bool PatchVertexFetches(PPCContext& ctx, const any360::xdk::Memory& m) {
  detail::VertexFetches f;
  if (!detail::ComputeVertexFetches(m, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r1.u32 - 592, f) ||
      !detail::VertexFetchesFit(m, f, ctx.r4.u32, ~0u))
    return false;
  detail::WriteVertexFetches(m, ctx.r4.u32, f);
  return true;
}

namespace detail {
// sub_826C67A0 (the shaders' link, on a vertex shader copy at `code`, `bytes` of it): `part` the vertex shader's part
// (+20: its outputs' count in bits 5-9, bit 18 none to link; its outputs' words after its fetch list: byte 3 the
// semantic, bits 8-11 the register, the high half's low 12 bits the offset of the list of export instructions that
// write it), `pixel` the pixel shader's part or 0 (+20: its inputs' count in bits 5-9, the last interpolator + 1 in
// bits 0-4, bit 17 none to link; its inputs' words from +32). Each output whose semantic no input reads has its export
// instructions emptied (sub_826C6618); each input's output has its exports' register made the input's
// (sub_826C66E8); the outputs past the last input are emptied; the program word gets the last interpolator in bits
// 20-23. `write` false: only checks that every instruction it would touch is in the copy and every word it reads is
// plain memory (nothing written).
inline bool LinkShaders(const any360::xdk::Memory& m, uint32_t part, uint32_t code, uint32_t bytes, uint32_t pixel,
                        uint32_t& program, bool write) {
  using any360::xdk::IsPlainMemory;
  const auto L32 = [&](uint32_t a) { return At32(m.Host(a)); };
  if (!IsPlainMemory(part + 20, 12) || (pixel && !IsPlainMemory(pixel + 20, 4))) return false;
  const uint32_t flags = L32(part + 20);
  if ((flags & 0x40000u) || (pixel && (L32(pixel + 20) & 0x20000u))) return true;
  const uint32_t outputs = (flags >> 5) & 0x1F, words = L32(part + 28) + L32(part + 24) + 9;
  // An output's export instructions: the list at the output word's offset, to its word with bit 12.
  const auto exports = [&](uint32_t output, auto&& each) {
    uint32_t at = part + 4 * (((L32(output) >> 16) & 0xFFF) + outputs + words);
    for (int k = 0; k < 4096; ++k, at += 4) {
      if (!IsPlainMemory(at, 4)) return false;
      const uint32_t w = L32(at), index = w & 0xFFF;
      if (12 * index + 12 > bytes) return false;
      each(code + 12 * index);
      if (w & 0x1000) return true;
    }
    return false;
  };
  const auto empty = [&](uint32_t output) {
    return exports(output, [&](uint32_t at) {
      if (write) Set32(m.Host(at), 0xC8000000u), Set32(m.Host(at + 4), 0), Set32(m.Host(at + 8), 0x02000000u);
    });
  };
  const auto remap = [&](uint32_t output, uint32_t input) {
    const uint32_t reg = (L32(input) >> 8) & 0xF;
    return exports(output, [&](uint32_t at) {
      if (write) Set32(m.Host(at), (At32(m.Host(at)) & ~0xFu) | reg);
    });
  };
  uint32_t output = part + 4 * words, input = pixel ? pixel + 32 : 0, inputs = 0, last = 0, done = 0;
  if (pixel) {
    const uint32_t pf = L32(pixel + 20);
    last = pf & 0x1F, inputs = (pf >> 5) & 0x1F;
    if (last) --last;
  }
  for (uint32_t i = 0; i < inputs; ++i, output += 4, input += 4, ++done) {
    if (!IsPlainMemory(input, 4)) return false;
    const uint32_t semantic = m.Host(input)[3];
    for (; done < outputs; ++done, output += 4) {
      if (!IsPlainMemory(output, 4)) return false;
      if (m.Host(output)[3] >= semantic) break;
      if (!empty(output)) return false;
    }
    if (!IsPlainMemory(output, 4)) return false;
    if (((L32(output) ^ L32(input)) & 0xF00u) && !remap(output, input)) return false;
  }
  for (; done < outputs; ++done, output += 4)
    if (!empty(output)) return false;
  if (write) program = (program & ~0x00F00000u) | ((last << 20) & 0x00F00000u);
  return true;
}
}  // namespace detail

// sub_826C67A0 as a function (the self-test's): r3 the vertex shader's part, r4 the copy, r5 the program word's
// address, r6 the pixel shader's part or 0. Declines what it cannot check (an instruction past 64 KB).
inline bool LinkShaders(PPCContext& ctx, const any360::xdk::Memory& m) {
  uint32_t program = 0;
  if (!any360::xdk::IsPlainMemory(ctx.r4.u32, 0x10000) || !any360::xdk::IsPlainMemory(ctx.r5.u32, 4) ||
      !detail::LinkShaders(m, ctx.r3.u32, ctx.r4.u32, 0x10000, ctx.r6.u32, program, false))
    return false;
  program = detail::At32(m.Host(ctx.r5.u32));
  detail::LinkShaders(m, ctx.r3.u32, ctx.r4.u32, 0x10000, ctx.r6.u32, program, true);
  detail::Set32(m.Host(ctx.r5.u32), program);
  return true;
}

// sub_826C6C48 (the draws' shader flush): r3 device, r4 the state's dirty bits (bits 17-20 the shaders'); r3 back:
// the bits left. The vertex shader (+12416; none: nothing) and the pixel shader (+12412, may be none): the pixel
// shader's constant list and IM_LOAD (bit 20), the vertex shader's (bit 19), the interpolation control 0x2208 and the
// shader programs 0x2180-0x2181 (under the bins when the device tiles, the vertex shader's second program for the
// odd tiles), the device's copies of them (+10400, +10404, +10408, +10452, +12420). Bit 17 first: the shader control
// (detail::UpdateShaderControl). The constant lists through the title's function (Runtime::constant_list: titles hook
// it).
// The vertex shader's patched copies (+11540 a patch set) native: used when cached (detail::CheckPatchCache); when
// the cached one is still read by the GPU, a new copy patched into the command buffer (sub_826C68E0: an
// INVALIDATE_STATE, the microcode as IM_LOAD_IMMEDIATE, its fetches remade by detail::ComputeVertexFetches). Declines,
// before anything is written, when the shader's own copy must be patched again (sub_826C6B00's patch) or linked
// from the runtime's link path (+10812 bit 7), when the cursor would need room, or when the shaders' memory is not
// plain. The vertex shader's outputs differing from the pixel shader's inputs (or +10813 bit 0): a copy into the
// command buffer, linked (detail::LinkShaders) then patched.
inline bool FlushShaders(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  using any360::xdk::IsPlainMemory;
  const uint32_t device = ctx.r3.u32;
  if (!g_runtime.constant_list || !IsPlainMemory(device, 16384)) return false;
  uint8_t* d = m.Host(device);
  const auto R = [&](uint32_t a) { return At32(m.Host(a)); };
  const uint32_t cursor = At32(d + 48), limit = At32(d + 56);
  if (cursor > limit) return false;
  uint64_t bits = ctx.r4.u64;
  const uint32_t vs = At32(d + 12416), ps = At32(d + 12412);
  // What the runtime will do, read first.
  if (ps && (!IsPlainMemory(ps, 68) || !IsPlainMemory(ps + R(ps + 64) + 40, 32))) return false;
  int entries = 0;
  uint32_t part = 0;  // the vertex shader's part the interpolators come from
  bool patched = false, odd_patched = false;  // the patched copies used (sub_826C6B00's cache hits)
  bool copied = false;                         // the first copy busy: a new one in the command buffer
  bool linked_copy = false, link = false;      // the shaders differ: a copy linked into the command buffer
  uint32_t copy_from = 0, copy_dwords = 0;
  if (vs) {
    if (!IsPlainMemory(vs, 912)) return false;
    const uint32_t first = vs + R(vs + 896), second = vs + R(vs + 904);
    if (!IsPlainMemory(first + 872, 32) || !IsPlainMemory(second + 872, 32)) return false;
    part = (!ps && (R(vs + 872) & 0x20)) ? second + 872 : first + 872;
    uint64_t will = bits | ((bits & 0x20000) ? 0x100 : 0);
    const uint32_t mode = At32(d + 10452) & 7;
    if (!ps && mode != 5) will |= 0x80008;
    if (ps && (will & 0x100000) && mode != 4) will |= 0x80008;
    const uint32_t mismatch =
        ps ? (R(ps + R(ps + 64) + 40 + 20) ^ R(part + 20)) & 0x7FFFFu : R(part + 20) & 0x1Fu;
    const bool linked = (d[10813] & 1) || mismatch;
    if (linked && (will & 0x180000)) {
      // sub_826C68E0 with the link (when they differ): the copy, its link, its fetches patched.
      linked_copy = true, link = mismatch != 0;
      const uint32_t which = (!ps && (R(vs + 872) & 0x20)) ? 1 : 0;
      const uint32_t entry = vs + R(vs + 896 + 8 * which);
      const uint32_t bytes = R(entry + 876);
      copy_dwords = bytes >> 2;
      copy_from = GpuAddress(R(entry + 872) + R(vs + 32), 0x1FFFFFFFu) + 0xC0000000u;
      VertexFetches fetches;
      uint32_t program = 0;
      if ((bytes & 3) || copy_dwords > 0x3FFE || !IsPlainMemory(copy_from, bytes) ||
          !ComputeVertexFetches(m, vs, At32(d + 11540), device + 12248, which, ctx.r1.u32 - 1040, fetches) ||
          !VertexFetchesFit(m, fetches, copy_from, bytes) ||
          (link && (m.Host(part + 8)[0] & 7) != 7 &&
           !LinkShaders(m, part, copy_from, bytes, ps ? ps + R(ps + 64) + 40 : 0, program, false)))
        return false;
    }
    const bool own_list = !linked && (will & 0x80000);
    if (own_list && At32(d + 11540) && !(R(vs + 872) & 0x40)) {
      // The patched copies (sub_826C6B00): native when they are cached; a patch or a link is the runtime's.
      if ((d[10812] & 0x80) || !IsPlainMemory(At32(d + 11540) + 32, 24) || !IsPlainMemory(At32(d + 10768), 4))
        return false;
      const uint32_t which = (!ps && (R(vs + 872) & 0x20)) ? 1 : 0;
      const PatchCache first = CheckPatchCache(m, d, vs, which);
      if (first == kPatch) return false;
      patched = first == kHit, copied = first == kBusy;
      if (copied) {
        const uint32_t entry = vs + R(vs + 896 + 8 * which);
        const uint32_t bytes = R(entry + 876);
        copy_dwords = bytes >> 2;
        copy_from = GpuAddress(R(entry + 872) + R(vs + 32), 0x1FFFFFFFu) + 0xC0000000u;
        VertexFetches fetches;
        if ((bytes & 3) || copy_dwords > 0x3FFE || !IsPlainMemory(copy_from, bytes) ||
            !ComputeVertexFetches(m, vs, At32(d + 11540), device + 12248, which, ctx.r1.u32 - 1040, fetches) ||
            !VertexFetchesFit(m, fetches, copy_from, bytes))
          return false;
      }
      // The interpolation mode as the runtime will have it (its odd tiles' copy wanted when it changes).
      uint32_t before = At32(d + 10452);
      if (!ps && (before & 7) != 5) before = (before & ~7u) | 5;
      if (ps && (will & 0x100000) && (before & 7) != 4) before = (before & ~7u) | 4;
      bool five = (d[10812] & 0x40) != 0;
      if (five && ps) {
        const uint32_t f = R(ps + R(ps + 64) + 40 + 28);
        five = !(At32(d + 10428) & 8) && !(f & 0x10) && !(f & 0x20);
      }
      if (five && (before & 7) != 5 && (R(vs + 872) & 0x20)) {
        const PatchCache odd = which == 1 ? first : CheckPatchCache(m, d, vs, 1);
        if (odd == kPatch) return false;
        odd_patched = odd == kHit;
      }
    }
    const int pixel_entries = ps && (will & 0x100000) ? ConstantListEntries(m, ps + 40) : 0;
    const int vertex_entries = own_list ? ConstantListEntries(m, vs + 872) : 0;
    if (pixel_entries < 0 || vertex_entries < 0) return false;
    entries = pixel_entries + vertex_entries;
  }
  const uint32_t most = 4 * (64 + 5 * uint32_t(entries) + (copied || linked_copy ? copy_dwords + 5 : 0));
  if (uint64_t(cursor) + most > std::min(limit, At32(d + 52)) || !IsPlainMemory(cursor + 4, most)) return false;

  Out out{m, cursor, records};
  const auto constant_list = [&](uint32_t part, uint32_t data) {
    Set32(d + 48, out.cursor);
    Call(ctx, base, g_runtime.constant_list, device, part, data);
    out.cursor = At32(d + 48);
  };
  if (bits & 0x20000) UpdateShaderControl(m, d, out), bits |= 0x100;
  if (!vs) {
    Set32(d + 48, out.cursor);
    ctx.r3.u64 = bits;
    return true;
  }
  constexpr uint32_t kNone = ~0u;
  uint32_t vertex_control = kNone, vertex_load = kNone, odd_load = kNone;
  uint32_t pixel_part = 0, pixel_program = 0, pixel_extra = 0;
  if (!ps) {
    const uint32_t v = At32(d + 10452);
    if ((v & 7) != 5) Set32(d + 10452, (v & ~7u) | 5), bits |= 0x80008;
  } else {
    const uint32_t ps_body = ps + R(ps + 64);
    pixel_part = ps_body + 40;
    pixel_program = R(pixel_part + 8), pixel_extra = R(pixel_part + 12);
    if ((pixel_program & 0x70000000u) > 0x10000000u) d[10814] |= 0x10;
    if (bits & 0x100000) {
      constant_list(ps + 40, R(ps + 24));
      bits |= 0x4000;
      out.ShaderLoad(GpuAddress(R(ps_body + 40) + R(ps + 24), 0x1FFFFFFEu) | 1, R(ps_body + 44) >> 2, false);
      Set32(d + 10408, R(pixel_part + 24));
      const uint32_t v = At32(d + 10452);
      if ((v & 7) != 4) bits |= 0x80008, Set32(d + 10452, (v & ~7u) | 4);
    }
  }
  const uint32_t mismatch = ps ? (R(pixel_part + 20) ^ R(part + 20)) & 0x7FFFFu : R(part + 20) & 0x1Fu;
  const uint32_t before = At32(d + 10452);
  uint32_t interpolation = before;
  uint32_t vertex_program = R(part + 8);
  const uint32_t vertex_extra = R(part + 12);
  if (d[10812] & 0x40) {
    bool five = true;
    if (ps) {
      const uint32_t f = R(pixel_part + 28);
      five = !(At32(d + 10428) & 8) && !(f & 0x10) && !(f & 0x20);
    }
    if (five) interpolation = (interpolation & ~7u) | 5;
    if (interpolation != At32(d + 12420) || (bits & 8)) bits |= 8, Set32(d + 12420, interpolation), vertex_control = interpolation;
  }
  if (!((d[10813] & 1) || mismatch) && (bits & 0x80000)) {
    constant_list(vs + 872, R(vs + 32));
    d[10814] &= 0x7F;
    vertex_load = (R(vs + 872) & 0x20) && !ps ? 1 : 0;
    if (patched) {
      UsePatchedCopy(m, d, vs, vertex_load);
      if (odd_patched) UsePatchedCopy(m, d, vs, 1), odd_load = 1, d[10814] |= 0x80;
    } else if (copied) {
      // sub_826C68E0: the copy loaded from the command buffer, not from the shader (no IM_LOAD of it after).
      const uint32_t which = vertex_load;
      vertex_load = kNone;
      out.Invalidate(0x100);
      out.Shader(0, copy_from, copy_dwords);
      d[10813] &= 0xFE;
      VertexFetches fetches;
      ComputeVertexFetches(m, vs, At32(d + 11540), device + 12248, which, ctx.r1.u32 - 1040, fetches);
      WriteVertexFetches(m, out.cursor - 4 * copy_dwords + 4, fetches);
      std::memcpy(d + 11552, d + 12248, 16);
      if (odd_patched) UsePatchedCopy(m, d, vs, 1), odd_load = 1, d[10814] |= 0x80;
    } else if (((interpolation ^ before) & 7) && (R(vs + 872) & 0x20)) {
      odd_load = 1, d[10814] |= 0x80;
    }
  } else if (linked_copy) {
    // sub_826C68E0 with the link: the vertex shader's literals, its copy into the command buffer, linked to the pixel
    // shader (the program word's interpolators), its fetches patched.
    d[10814] &= 0x7F;
    constant_list(vs + 872, R(vs + 32));
    const uint32_t which = (!ps && (R(vs + 872) & 0x20)) ? 1 : 0;
    out.Invalidate(0x100);
    out.Shader(0, copy_from, copy_dwords);
    const uint32_t code = out.cursor - 4 * copy_dwords + 4;
    if (link && (m.Host(part + 8)[0] & 7) != 7)
      LinkShaders(m, part, code, 4 * copy_dwords, pixel_part, vertex_program, true);
    d[10813] = uint8_t((d[10813] & 0xFE) | (link ? 1 : 0));
    VertexFetches fetches;
    ComputeVertexFetches(m, vs, At32(d + 11540), device + 12248, which, ctx.r1.u32 - 1040, fetches);
    WriteVertexFetches(m, code, fetches);
    std::memcpy(d + 11552, d + 12248, 16);
  }
  uint32_t program = kNone, program_extra = kNone;
  if (bits & 0x180000) {
    bits |= 0x18000;
    Set32(d + 10404, vertex_extra | pixel_extra), Set32(d + 10400, vertex_program | pixel_program);
    if (d[10814] & 0x80) {
      const uint32_t second = vs + R(vs + 904);
      program = R(second + 880) | pixel_program, program_extra = R(second + 884) | pixel_extra;
    }
  }
  const auto load = [&](uint32_t which, bool predicated) {
    const uint32_t entry = vs + R(vs + 896 + 8 * which);
    out.ShaderLoad(GpuAddress(R(entry + 872) + R(vs + 32), 0x1FFFFFFFu), R(entry + 876) >> 2, predicated);
  };
  if (d[10812] & 0x40) {
    out.Bins(0, 0x15555555u), out.Bins(1, 0);
    if (bits & 8) out.Registers(0x2208, {At32(d + 10452)}), bits &= ~uint64_t(8);
    if (vertex_control != kNone) out.ContextConstant(0x208, vertex_control);
    if (bits & 0x10000) bits &= ~uint64_t(0x18000), out.Registers(0x2180, {At32(d + 10400), At32(d + 10404)});
    if (program != kNone) out.ContextConstants(0x180, {program, program_extra});
    if (odd_load != kNone) {
      load(odd_load, true);
      if (vertex_load != kNone) out.Bins(0, 0x2AAAAAAAu), load(vertex_load, true);
    } else if (vertex_load != kNone) {
      load(vertex_load, false);
    }
    out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
  } else if (vertex_load != kNone) {
    load(vertex_load, false);
  }
  Set32(d + 48, out.cursor);
  ctx.r3.u64 = bits;
  return true;
}
inline void FlushShadersOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && FlushShaders(ctx, base, m, DrawRecords())) return;
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
namespace detail {
// The clear's draw arguments the runtime passes down (sub_826BF8C0 -> sub_826BF670 -> sub_826BF380).
struct ClearDrawArguments {
  uint32_t colors;       // 4 floats
  uint32_t control;      // 0x2000's value (its bits 16-17: 0x2301's mode)
  double depth;
  uint32_t format;       // 0x2001's value (the target's format word)
  uint32_t edges_2200;   // 0x2200 for the edges (and a rect the tiles cannot align)
  uint32_t inner_2200;   // 0x2200 for the aligned inside
  uint32_t edges_2208;   // 0x2208 for the edges
  uint32_t inner_2208;   // 0x2208 for the aligned inside
};
// sub_826BF380: `count` rects (x1, y1, x2, y2) as rect-list triangles (3 vertices of 7 floats each: x, y less the
// half-pixel bias, depth, the color) in a block of the runtime's allocator; fetch constant 0 at them, 0x5000-0x5002,
// 0x2000-0x2001, 0x2301, 0x2200, 0x2208, a predicated auto draw; the groups it changed marked dirty. No block (the
// allocator failed): nothing drawn. `frame`: its stack frame (the allocator's and make-room's caller).
inline void ClearRects(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records, uint32_t frame,
                       uint32_t device, uint32_t count, const int32_t (*rects)[4], const ClearDrawArguments& a,
                       uint32_t control, uint32_t value_2200, uint32_t value_2208) {
  const Runtime& rt = g_runtime;
  uint8_t* d = m.Host(device);
  const float bias =
      std::bit_cast<float>(At32(m.Host((At32(d + 10560) & 1) ? rt.clear_default_color : rt.resolve_offset_float)));
  ctx.r1.u64 = frame;
  uint32_t cursor = At32(d + 48);
  if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
  const uint32_t block = uint32_t(Call(ctx, base, rt.allocate, device, 21 * count, 4));
  if (!block) {
    Set32(d + 48, cursor);
    return;
  }
  // The color loaded and stored as floats (lfs, stfs): a signaling NaN comes out quiet, as the title's code makes it.
  uint32_t color[4];
  for (int k = 0; k < 4; ++k) {
    color[k] = At32(m.Host(a.colors + 4 * k));
    if ((color[k] & 0x7F800000u) == 0x7F800000u && (color[k] & 0x7FFFFFu)) color[k] |= 0x400000u;
  }
  const uint32_t z = std::bit_cast<uint32_t>(float(a.depth));
  uint8_t* v = m.Host(block);
  for (uint32_t i = 0; i < count; ++i, v += 84) {
    const uint32_t x1 = std::bit_cast<uint32_t>(float(rects[i][0]) - bias), y1 = std::bit_cast<uint32_t>(float(rects[i][1]) - bias);
    const uint32_t x2 = std::bit_cast<uint32_t>(float(rects[i][2]) - bias), y2 = std::bit_cast<uint32_t>(float(rects[i][3]) - bias);
    const uint32_t corners[3][2] = {{x1, y1}, {x2, y1}, {x2, y2}};
    for (int c = 0; c < 3; ++c) {
      uint8_t* p = v + 28 * c;
      Set32(p, corners[c][0]), Set32(p + 4, corners[c][1]), Set32(p + 8, z);
      for (int k = 0; k < 4; ++k) Set32(p + 12 + 4 * k, color[k]);
    }
  }
  const uint32_t mode = (control >> 16) & 3;
  Out out{m, cursor, records};
  out.Registers(0x4800, {GpuAddress(block, 0x1FFFFFFFu) | 3, 0x10000002u | ((21 * count << 2) & 0x03FFFFFCu), 0, 0, 0, 0});
  out.Registers(0x5000, {0, 0, 0});
  out.Registers(0x2000, {control, a.format});
  out.Registers(0x2301, {mode == 0 ? 0u : mode == 1 ? 0x8001u : 0xC003u});
  out.Registers(0x2200, {value_2200});
  out.Registers(0x2208, {value_2208});
  out.AutoDraw(((count * 0x30000u) & 0xFFFF0000u) | 0x88, true);
  Set32(d + 48, out.cursor);
  Set64(d + 24, At64(d + 24) | 0x80000000u);
  Set64(d + 16, At64(d + 16) | uint64_t(1) << 57 | uint64_t(1) << 56 | 2048 | 8);
  Set64(d + 32, At64(d + 32) | uint64_t(1) << 36);
}
// sub_826BF670: the rect's inside aligned to the EDRAM tiles (80x16, 80x8 or 40x8 pixels by the multisampling in
// `control` bits 16-17) drawn at the tiles' scale (the control's sizes and mode changed to match), the edges outside it
// drawn as they are (up to four rects); a rect too small for a tile drawn as it is.
inline void ClearDraw(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records, uint32_t frame,
                      uint32_t device, int32_t x1, int32_t y1, int32_t x2, int32_t y2, const ClearDrawArguments& a) {
  const uint32_t mode = (a.control >> 16) & 3;
  const uint32_t shift_x = mode <= 1 ? 1 : 0, shift_y = mode == 0 ? 1 : 0;
  const uint32_t w = 40u << shift_x, h = 8u << shift_y;
  const int32_t right = int32_t((uint32_t(x2) / w) * w), left = int32_t(((uint32_t(x1) + w - 1) / w) * w);
  const int32_t bottom = int32_t((uint32_t(y2) / h) * h), top = int32_t(((uint32_t(y1) + h - 1) / h) * h);
  const uint32_t inner_frame = frame - 224;
  if (left >= right || top >= bottom) {
    const int32_t whole[1][4] = {{x1, y1, x2, y2}};
    ClearRects(ctx, base, m, records, inner_frame, device, 1, whole, a, a.control, a.edges_2200, a.edges_2208);
    return;
  }
  int32_t edges[4][4];
  uint32_t n = 0;
  if (left > x1) edges[n][0] = x1, edges[n][1] = y1, edges[n][2] = left, edges[n][3] = y2, ++n;
  if (right < x2) edges[n][0] = right, edges[n][1] = y1, edges[n][2] = x2, edges[n][3] = y2, ++n;
  if (top > y1) edges[n][0] = left, edges[n][1] = y1, edges[n][2] = right, edges[n][3] = top, ++n;
  if (bottom < y2) edges[n][0] = left, edges[n][1] = bottom, edges[n][2] = right, edges[n][3] = y2, ++n;
  if (n) ClearRects(ctx, base, m, records, inner_frame, device, n, edges, a, a.control, a.edges_2200, a.edges_2208);
  const int32_t inside[1][4] = {{left >> shift_x, top >> shift_y, right >> shift_x, bottom >> shift_y}};
  uint32_t control = (a.control & ~0x30000u) | 0x20000u;
  control = (control & 0xFFFFC000u) | ((control & 0x3FFFu) >> shift_x);
  control = (control & 0x3FFFFu) | ((((control >> 18) & 0x3FFFu) >> shift_x) << 18);
  ClearRects(ctx, base, m, records, inner_frame, device, 1, inside, a, control, a.inner_2200, a.inner_2208);
}
}  // namespace detail

// sub_826BF8C0 (a clear's draw of one target, -1 the depth alone): r3 device, r4 the clear's flags, r5 the target,
// r6-r9 the rect, r10 the colors, f1 the depth, the stencil value its tenth argument (the caller's +92). The stencil
// reference (flag 0x20), the state group through the title's helper, VGT 0x2080-0x2082; a target with a format the
// EDRAM clears fast and a rect of a tile or more: its color packed by the title's sub_826BE110 into 0x2002/0x210D and
// the depth derived from it; then the draws (detail::ClearDraw), under each tile's bins when the device tiles. The
// title's helpers (make room, state, allocator, color packing) called from the frames the runtime's own calls have.
inline bool ClearTarget(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  using any360::xdk::IsPlainMemory;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, colors = ctx.r10.u32, entry = ctx.r1.u32;
  const int32_t target = ctx.r5.s32, x1 = ctx.r6.s32, y1 = ctx.r7.s32, x2 = ctx.r8.s32, y2 = ctx.r9.s32;
  const double depth = ctx.f1.f64;  // the title's helpers called below may change f1
  if (!rt.make_room || !rt.flush_state || !rt.allocate || !rt.clear_color_pack || !rt.clear_depth_offset ||
      !rt.clear_depth_scale || !IsPlainMemory(device, 16384) || !IsPlainMemory(entry - 1200, 1300) ||
      !IsPlainMemory(colors & ~15u, 16) || (target != -1 && uint32_t(target) >= 4))
    return false;
  uint8_t* d = m.Host(device);
  // Records take more room than PM4: near the segment's top the runtime's allocator (a block above the cursor + 164)
  // could refuse a rect's block the PM4 would have had; there, PM4.
  if (records && (!At32(d + 14540) || At32(d + 14540) < At32(d + 48) + 0x4000)) records = false;
  const uint32_t control = At32(d + 10240);
  uint32_t format = 0, count = 5;
  if (target != -1) {
    const uint32_t surface = At32(d + 12160 + 4 * uint32_t(target));
    if (!IsPlainMemory(surface + 28, 4)) return false;
    count = 4;
    format = At32(m.Host(surface + 28));
    if ((format & 0xF0000u) == 0xA0000u) format = (format & ~0xF0000u) | 0x20000u;
    if ((format & 0xF0000u) == 0xC0000u) format = (format & ~0xF0000u) | 0x30000u;
  }
  if ((d[10812] & 0x20) && At32(d + 12468) > 16) return false;
  // The fast clear (no stencil or depth flags, a format the EDRAM clears fast, a rect of a tile or more): the title's
  // packer returns an unset stack word for formats 8, 9, 11 and 13 (a register some earlier callee saved there), which
  // only the runtime's own calls reproduce: those are its.
  const uint32_t kind = (format >> 16) & 0xF, multisample = (control >> 16) & 3;
  const bool fast = !(flags & 0x30) && kind != 5 && kind != 7 && kind != 15 &&
                    int32_t(uint32_t(x2) - uint32_t(x1)) >= (multisample >= 2 ? 40 : 80) &&
                    int32_t(uint32_t(y2) - uint32_t(y1)) >= (multisample == 0 ? 16 : 8);
  if (fast && (kind == 8 || kind == 9 || kind == 11 || kind == 13)) return false;
  // The rect kept in the caller's parameter area, as the runtime keeps it (+44, +52, +60, +68).
  Set32(m.Host(entry + 44), uint32_t(x1)), Set32(m.Host(entry + 52), uint32_t(y1));
  Set32(m.Host(entry + 60), uint32_t(x2)), Set32(m.Host(entry + 68), uint32_t(y2));
  const uint32_t frame = entry - 304;  // the runtime's frame: its callees' (the color packer writes its caller's +32)
  ctx.r1.u64 = frame;
  const auto room = [&] {
    ctx.r1.u64 = frame;
    const uint32_t cursor = At32(d + 48);
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  uint32_t state = At32(d + 10432) & 0xFFFFFFF0u, edges_2200 = 0;
  if (flags & 0x10) edges_2200 = 118, state |= 1;
  if (flags & 0x20) {
    edges_2200 |= 0x8701, state |= 4;
    Out out{m, room(), records};
    out.Registers(0x210D, {0x00FF0000u | (At32(m.Host(entry + 92)) & 0xFF)});
    Set32(d + 48, out.cursor);
    Set64(d + 16, At64(d + 16) | 0x10000000u);
  }
  ctx.r1.u64 = frame;
  Call(ctx, base, rt.flush_state, device, 256, state);
  Out out{m, room(), records};
  out.Registers(0x2080, {0, 0, 0x20002000u});
  ClearDrawArguments a{colors, control, depth, format, edges_2200, edges_2200, count, count};
  const uint32_t clearing = flags & 0x30;
  if (fast) {
    // The color as the target holds it (sub_826BE110, v1 the colors as lvx128 loads them).
    const uint8_t* c = m.Host(colors & ~15u);
    for (int i = 0; i < 16; ++i) ctx.v1.u8[i] = c[15 - i];
    ctx.r1.u64 = frame;
    Call(ctx, base, rt.clear_color_pack, kind, (format >> 20) & 0x3F, frame + 132, frame + 136);
    const uint32_t packed = At32(m.Host(frame + 132));
    out.Registers(0x2203, {0}), out.Registers(0x2002, {format & 0xFFF});
    out.Registers(0x210D, {0x00FF0000u | (packed & 0xFF)});
    const double offset = std::bit_cast<double>(At64(m.Host(rt.clear_depth_offset)));
    const double scale = std::bit_cast<double>(At64(m.Host(rt.clear_depth_scale)));
    a.depth = double(float((double(packed >> 8) - offset) * scale));
    a.inner_2200 = 0x8777, a.inner_2208 = 5;
    Set64(d + 16, At64(d + 16) | 256 | uint64_t(1) << 55 | 0x10000000u);
  }
  Set32(d + 48, out.cursor);
  const uint32_t draw_frame = frame - 288;
  if (!(d[10812] & 0x20)) {
    ClearDraw(ctx, base, m, records, draw_frame, device, x1, y1, x2, y2, a);
    ctx.r1.u64 = entry;
    return true;
  }
  uint32_t bins = At32(d + 12428);
  if ((d[10815] & 0x40) && (d[10812] & 0x40) && (bins & 0x15555555u)) {
    if (clearing) {
      Out first{m, room(), records};
      first.Bins(0, bins & 0x15555555u);
      Set32(d + 48, first.cursor);
      const uint32_t shifted = (control & 0x3FFFFu) | (At32(d + 12892) << 18);
      ClearDrawArguments whole = a;
      whole.control = (shifted & 0xFFFFC000u) | (At32(d + 12896) & 0x3FFFu);
      whole.inner_2200 = edges_2200, whole.edges_2208 = 5, whole.inner_2208 = 5;
      const int32_t ox = int32_t(At32(d + 12712)), oy = int32_t(At32(d + 12716));
      ClearDraw(ctx, base, m, records, draw_frame, device, x1 - ox, y1 - oy, x2 - ox, y2 - oy, whole);
    }
    bins &= 0xEAAAAAAAu;
  }
  for (uint32_t i = 0; i < At32(d + 12468); ++i) {
    const uint8_t* r = d + 12472 + 16 * i;
    const int32_t tx1 = std::max(x1, int32_t(At32(r))), ty1 = std::max(y1, int32_t(At32(r + 4)));
    const int32_t tx2 = std::min(x2, int32_t(At32(r + 8))), ty2 = std::min(y2, int32_t(At32(r + 12)));
    const uint32_t mask = (2 * i < 32 ? 3u << (2 * i) : 0u) & bins;
    if (tx1 >= tx2 || ty1 >= ty2 || !mask) continue;
    Out tile{m, room(), records};
    tile.Bins(0, mask);
    Set32(d + 48, tile.cursor);
    const int32_t ox = int32_t(At32(d + 12712 + 8 * i)), oy = int32_t(At32(d + 12716 + 8 * i));
    ClearDraw(ctx, base, m, records, draw_frame, device, tx1 - ox, ty1 - oy, tx2 - ox, ty2 - oy, a);
  }
  ctx.r1.u64 = entry;
  return true;
}
inline void ClearTargetOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && ClearTarget(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

// sub_826C3750 (D3D's RunCommandBuffer): r3 device, r4 a recorded command buffer, r5 the bins it selects (0: all).
// The device's dirty state as the buffer leaves it to the device (its +24..+56 masks the groups, then the draws'
// flush), then: the bin select (unless the device records tiles, +10812 bit 7), each recorded segment (the buffer's
// +116 list: next, count, then address and size pairs) as an INDIRECT_BUFFER (predicated as +12436 says), or, for an
// occlusion-predicated buffer (+10812 bit 1, the buffer's +108 bit 11), under all bins with its start kept in the
// device's +12960 list and an EVENT_WRITE_EXT 26 / EVENT_WRITE 25 after; the device's bins back when the buffer
// changed them (bit 6); a buffer +108 bit 5: only room made. Then the tile recording's flags, or the buffer's fence
// (+120, kept in the +12980 list once a frame) and frame (+8), the frame stamped in the resources it uses (+112 list),
// the viewport put back unless the buffer kept it, the dirty masks the buffer's +64..+96 inverted. The title's
// helpers (make room, the flush's uploads, the lists' new blocks, the viewport) called from the runtime's frame.
// Declines what it cannot check: a segment list empty or with an empty node, memory not plain.
inline bool ReplayCommandBuffer(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  using any360::xdk::IsPlainMemory;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, buffer = ctx.r4.u32, select = ctx.r5.u32, entry = ctx.r1.u32;
  if (!rt.make_room || !rt.restore_viewport || !rt.list_block || !rt.upload_constants || !rt.upload_words ||
      !rt.flush_shaders || !rt.flush_state || !rt.flush_misc || !rt.flush_waits || !IsPlainMemory(device, 16384) ||
      !IsPlainMemory(buffer, 124) || !IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  uint8_t* b = m.Host(buffer);
  const uint32_t flags = At32(b + 108);
  // What it will read, checked first.
  if (!(flags & 0x20)) {
    uint32_t node = At32(b + 116);
    if (!node) return false;
    for (int n = 0; node; ++n, node = At32(m.Host(node))) {
      if (n > 4096 || !IsPlainMemory(node, 8)) return false;
      const uint32_t count = At32(m.Host(node + 4));
      if (!count || count > 0x10000 || !IsPlainMemory(node + 8, 8 * count)) return false;
    }
  }
  for (const uint32_t list : {device + 12960, device + 12980})
    if (!IsPlainMemory(At32(m.Host(list + 8)), 16)) return false;
  if ((d[10812] & 0x80) && !IsPlainMemory(At32(d + 13140) + 108, 4)) return false;
  {
    uint32_t node = At32(b + 112);
    for (int n = 0; node; ++n, node = At32(m.Host(node))) {
      if (n > 4096 || !IsPlainMemory(node, 8)) return false;
      const uint32_t count = At32(m.Host(node + 4)) & 0x7FFFFFFF;
      if (count > 0x10000 || !IsPlainMemory(node + 8, 8 * count)) return false;
      for (uint32_t i = 0; i < count; ++i)
        if (const uint32_t object = At32(m.Host(node + 8 + 8 * i)) << 2; object && !IsPlainMemory(object + 8, 8))
          return false;
    }
  }
  const uint32_t frame = entry - 176;
  ctx.r1.u64 = frame;
  for (uint32_t k = 0; k < 5; ++k) Set64(d + 8 * k, At64(d + 8 * k) & At64(b + 24 + 8 * k));
  FlushDirtyState(ctx, base, m, device);
  const auto room = [&] {
    ctx.r1.u64 = frame;
    const uint32_t cursor = At32(d + 48);
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  if (flags & 0x20) {
    ctx.r1.u64 = frame;
    Call(ctx, base, rt.make_room, device);
  } else {
    Out out{m, room(), records};
    // The occlusion-predicated replay keeps where its bin select's value is (the device's +12960 list, given to the
    // GPU at the tiles' end): that select stays PM4, its value a big-endian word as the runtime leaves it.
    const bool occlusion = (d[10812] & 2) && (flags & 0x800);
    if (!(d[10812] & 0x80)) {
      Out select_out{m, out.cursor, records && !occlusion};
      select_out.Bins(3, select);
      out.cursor = select_out.cursor;
    }
    const uint32_t start = out.cursor;
    uint32_t node = At32(b + 116), index = 0;
    // The next segment; false when it was the last.
    const auto segment = [&](uint32_t predicate) {
      const uint8_t* e = m.Host(node + 8 * (index + 1));
      out.Indirect(At32(e), At32(e + 4), predicate);
      if (++index == At32(m.Host(node + 4))) {
        node = At32(m.Host(node)), index = 0;
        if (!node) return false;
      }
      Set32(d + 48, out.cursor);
      out.cursor = room();
      return true;
    };
    if (occlusion) {
      uint32_t list = At32(d + 12968);
      if (list + 16 > At32(d + 12972)) {
        ctx.r1.u64 = frame;
        list = uint32_t(Call(ctx, base, rt.list_block, device + 12960));
      }
      Set32(m.Host(list), start);
      Set32(d + 12968, list + 16);
      out.Bins(0, ~0u);
      while (segment(1)) {
      }
      out.Bins(0, 0x80000000u);
      out.Event(0x5A, {26, GpuAddress(list + 4, 0x1FFFFFFEu) | 1}, true);
      out.Event(0x46, {25}, true);
    } else {
      const uint32_t predicate = At32(d + 12436);
      while (segment(predicate)) {
      }
    }
    if (flags & 0x40) out.Bins(0, At32(d + 12428)), out.Bins(1, At32(d + 12432));
    Set32(d + 48, out.cursor);
  }
  if (d[10812] & 0x80) {
    uint8_t* tiles = m.Host(At32(d + 13140));
    Set32(tiles + 108, ((At32(tiles + 108) | 0x20) & ~0x40u) | (flags & 0x40));
  } else {
    const uint32_t fence = At32(b + 120);
    if (fence && (d[10812] & 0x20) && At32(b + 8) != At32(d + 10780)) {
      uint32_t list = At32(d + 12988);
      if (list + 4 > At32(d + 12992)) {
        ctx.r1.u64 = frame;
        list = uint32_t(Call(ctx, base, rt.list_block, device + 12980));
      }
      Set32(m.Host(list), fence);
      Set32(d + 12988, list + 4);
    }
    const uint32_t now = At32(d + 10780), wanted = select ? select : ~0u;
    Set32(b + 8, now);
    for (uint32_t node = At32(b + 112); node; node = At32(m.Host(node))) {
      const uint32_t count = At32(m.Host(node + 4)) & 0x7FFFFFFF;
      for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = m.Host(node + 8 + 8 * i);
        const uint32_t object = At32(e) << 2;
        if (!object) continue;
        if (At32(e + 4) & wanted) Set32(m.Host(object + 8), now);
        if (At32(e) & 0x40000000u) Set32(m.Host(object + 12), now);
      }
    }
  }
  if ((At64(b + 96) & 0x0001C00000000000ull) != 0x0001C00000000000ull) {
    const uint32_t vy2 = At32(d + 10312), vx2 = At32(d + 10308);
    ctx.r1.u64 = frame;
    ctx.r3.u64 = device;
    ctx.r4.s64 = int32_t(vx2 << 17) >> 17, ctx.r5.s64 = int32_t(vx2 << 1) >> 17;
    ctx.r6.s64 = int32_t(vy2 << 17) >> 17, ctx.r7.s64 = int32_t(vy2 << 1) >> 17;
    rt.restore_viewport(ctx, base);
  }
  for (uint32_t k = 0; k < 5; ++k) Set64(d + 8 * k, ~At64(b + 64 + 8 * k));
  ctx.r1.u64 = entry;
  return true;
}
inline void ReplayCommandBufferOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && ReplayCommandBuffer(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}
// sub_826BB5C8 (a fence): r3 device, r4 the cursor to write after, r5 the flags (bit 1 the fence, bit 2 the wait
// after it, neither: both; bit 0 without the 0x5C8 write; bits 24-29 the value, 0: 4), r6/r7 two words for
// 0x57C-0x57D; r3 back: the cursor after. The fence: the scratch value (0x578), a predicated wait for the GPU's
// counter (+10772) to reach it, a predicated INTERRUPT, 0x45E; the wait after: the counter at 0, 0x57C = 0x0BADF00D.
// The device's +10815 bit 4 kept as the runtime keeps it. Declines when the room after the cursor is not plain.
namespace detail {
// The fence's words as they lie in memory, for one form (written at once: no writer per word).
template <bool kRecords>
inline uint32_t FenceWords(uint32_t* w, uint8_t* d, uint32_t flags, uint32_t first, uint32_t second) {
  namespace record = any360::xdk::record;
  uint32_t n = 0;
  const auto registers = [&](uint32_t index, uint32_t count, uint32_t a, uint32_t b) {
    if constexpr (kRecords) {
      w[n++] = std::byteswap(record::Header(record::RegistersWords(count)));
      w[n++] = record::KindWord(record::kRegisters, index);
      w[n++] = a;
      if (count > 1) w[n++] = b;
    } else {
      w[n++] = std::byteswap(((count - 1) << 16) | index);
      w[n++] = std::byteswap(a);
      if (count > 1) w[n++] = std::byteswap(b);
    }
  };
  const auto wait = [&](uint32_t counter, uint32_t reference) {
    const uint32_t v[5] = {19, counter, reference, 0xFFFFFFFFu, 256};
    if constexpr (kRecords) {
      w[n++] = std::byteswap(record::Header(6, true));
      w[n++] = record::KindWord(record::kWait, 0);
      for (uint32_t x : v) w[n++] = x;
    } else {
      w[n++] = std::byteswap(0xC0043C01u);
      for (uint32_t x : v) w[n++] = std::byteswap(x);
    }
  };
  const uint32_t value = (flags >> 24) & 0x3F ? (flags >> 24) & 0x3F : 4;
  if (!(flags & 6)) flags |= 6;
  const uint32_t counter = GpuAddress(At32(d + 10772), 0x1FFFFFFFu) | 2;
  if (flags & 2) {
    d[10815] |= 0x10;
    if (!(flags & 1)) registers(0x5C8, 1, 0x20000, 0);
    registers(0x57C, 2, first, second);
    registers(0x578, 1, value, 0);
    wait(counter, value);
    if constexpr (kRecords) {
      w[n++] = std::byteswap(record::Header(2, true)), w[n++] = record::KindWord(record::kInterrupt, 0), w[n++] = value;
    } else {
      w[n++] = std::byteswap(0xC0005401u), w[n++] = std::byteswap(value);
    }
    registers(0x45E, 1, value, 0);
  }
  if (flags & 4) {
    d[10815] &= 0xEF;
    wait(counter, 0);
    registers(0x57C, 1, 0x0BADF00Du, 0);
  }
  return n;
}
}  // namespace detail
inline bool Fence(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32, cursor = ctx.r4.u32;
  if (!any360::xdk::IsPlainMemory(device, 16384) || !any360::xdk::IsPlainMemory(cursor + 4, 200)) return false;
  uint32_t words[48];
  uint8_t* d = m.Host(device);
  const uint32_t n = records ? FenceWords<true>(words, d, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32)
                             : FenceWords<false>(words, d, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  std::memcpy(m.Host(cursor + 4), words, 4 * size_t(n));
  ctx.r3.u64 = cursor + 4 * n;
  return true;
}
inline void FenceOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && Fence(ctx, m, DrawRecords())) return;
  original(ctx, base);
}

// sub_826BC568 (the swap's packet): r3 device, r4 1 without the waits. The GPU timestamps around it (sub_826C8488,
// when the device profiles: +21272 2, an EVENT_WRITE_SHD of 0xDEADBEEF into the +21268 ring's next slot, its two
// halves the start and the end), the previous frame's wait (scratch 0x579 = 1, the counter's second word at 1), the
// fence with the present mode (+13220) and the title's swap word, the wait for the second word back at 0; +10813 bit
// 5. The title's make-room called where the runtime calls it. Declines a present mode it does not know (the runtime
// reads an unset stack word there).
inline bool SwapPacket(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, quiet = ctx.r4.u32, entry = ctx.r1.u32;
  if (!rt.make_room || !rt.swap_word || !any360::xdk::IsPlainMemory(device, 21284) ||
      !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  uint32_t mode;
  switch (At32(d + 13220)) {
    case 0: case 1: mode = 1; break;
    case 2: mode = 2; break;
    case 4: mode = 3; break;
    case 0x80000000u: mode = 0; break;
    default: return false;
  }
  if (At32(d + 21272) == 2 && !any360::xdk::IsPlainMemory(At32(d + 21268), 32)) return false;
  const uint32_t frame = entry - 128;
  const auto room = [&] {
    ctx.r1.u64 = frame;
    const uint32_t cursor = At32(d + 48);
    return cursor > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : cursor;
  };
  const auto timestamp = [&](bool start) {
    if (At32(d + 21272) != 2) return;
    const uint32_t slot = At32(d + 21280) == 3 ? 0 : At32(d + 21280) + 1;
    if (slot == At32(d + 21276)) return;
    Out out{m, room(), records};
    out.Event(0x58, {0x80000003u, GpuAddress(At32(d + 21268) + 4 * (2 * slot + (start ? 0 : 1)), 0x1FFFFFFFu) | 2,
                     0xDEADBEEFu});
    Set32(d + 48, out.cursor);
  };
  timestamp(true);
  Out out{m, room(), records};
  const uint32_t second = GpuAddress(At32(d + 10772) + 4, 0x1FFFFFFFu) | 2;
  if (!quiet) out.Registers(0x579, {1}), out.Wait(19, second, 1, 0xFFFFFFFFu, 256);
  {
    // The fence as the title's bound fence writes it (any360_d3d_records).
    uint32_t words[48];
    const uint32_t second_word = ((At32(d + 11572) >> 23) & 0x7F) | (mode << 8) | quiet;
    const uint32_t n = DrawRecords() ? FenceWords<true>(words, d, 0, rt.swap_word, second_word)
                                     : FenceWords<false>(words, d, 0, rt.swap_word, second_word);
    std::memcpy(m.Host(out.cursor + 4), words, 4 * size_t(n));
    out.cursor += 4 * n;
  }
  if (!quiet) out.Wait(19, second, 0, 0xFFFFFFFFu, 256);
  Set32(d + 48, out.cursor);
  timestamp(false);
  d[10813] |= 0x20;
  ctx.r1.u64 = entry;
  return true;
}
inline void SwapPacketOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && SwapPacket(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}
namespace detail {
// A big-endian guest word changed atomically (as the runtime's lwarx/stwcx. and ldarx/stdcx. loops do).
template <class T, class F>
inline void AtomicBigEndian(uint8_t* p, F change) {
  auto& word = *reinterpret_cast<std::atomic<T>*>(p);
  T old = word.load(std::memory_order_relaxed);
  for (;;) {
    const T now = sizeof(T) == 8 ? T(any360::xdk::detail::ByteSwap64(uint64_t(old))) : T(std::byteswap(uint32_t(old)));
    const T next = change(now);
    const T stored = sizeof(T) == 8 ? T(any360::xdk::detail::ByteSwap64(uint64_t(next))) : T(std::byteswap(uint32_t(next)));
    if (word.compare_exchange_weak(old, stored, std::memory_order_acq_rel)) return;
  }
}
}  // namespace detail

// sub_826AC8F0 (D3D's resource lock for the CPU): r3 the resource, r4 the fence wait's argument, r5-r7 the resource's
// bases, r8 the address locked, r9 its size, r10 the flags, the caller's +84 the coherency control. The device the
// title's global (Runtime::device_slot). Waits for the resource's fence (+8, or +12 for a write lock; a texture's
// parent's) through the title's helper; unless the flags say no flush (bits 1, 4): on the device's own thread a
// cache flush of the range's pages (COHER_SIZE/BASE 0xA2F, 0xA31 the control, a wait for 0xA31), from another thread
// the range merged into the device's pending one (+11544, atomically). Then (flag 4 or the resource's bit 21) the
// locked lines (128 bytes) widened in the resource's +20 (+24 for a mip tail), the address turned into the GPU view
// (flag 4); the lock count (+0, 256 a lock) raised. r3 back: the address.
inline bool ResourceLock(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  using any360::xdk::IsPlainMemory;
  const Runtime& rt = g_runtime;
  const uint32_t resource = ctx.r3.u32, wait = ctx.r4.u32, has_tail = ctx.r5.u32, head_base = ctx.r6.u32,
                 tail_base = ctx.r7.u32, size = ctx.r9.u32, flags = ctx.r10.u32, entry = ctx.r1.u32;
  uint32_t address = ctx.r8.u32;
  if (!rt.device_slot || !rt.block_on_fence || !rt.make_room || !IsPlainMemory(resource, 28) ||
      !IsPlainMemory(entry + 84, 4) || !IsPlainMemory(entry - 1024, 1024) || !IsPlainMemory(rt.device_slot, 4))
    return false;
  const uint32_t slot = At32(m.Host(rt.device_slot));
  if (!IsPlainMemory(slot, 4) || !IsPlainMemory(At32(m.Host(slot)), 16384) || !IsPlainMemory(ctx.r13.u32 + 256, 4) ||
      !IsPlainMemory(At32(m.Host(ctx.r13.u32 + 256)) + 332, 4))
    return false;
  uint8_t* r = m.Host(resource);
  uint32_t holder = resource;
  if ((At32(r) & 0xF) == 4 && (At32(r) & 0x40000000u) && At32(r + 24)) holder = At32(r + 24);
  if (!IsPlainMemory(holder + 8, 8)) return false;
  const uint32_t frame = entry - 176;
  if (const uint32_t fence = At32(m.Host(holder + ((flags & 0x1010) ? 12 : 8)))) {
    ctx.r1.u64 = frame;
    Call(ctx, base, rt.block_on_fence, At32(m.Host(At32(m.Host(rt.device_slot)))), fence, wait);
  }
  if (!(flags & 0x12)) {
    const uint32_t device = At32(m.Host(At32(m.Host(rt.device_slot))));
    uint8_t* d = m.Host(device);
    const uint32_t start = GpuAddress(address, 0x1FFFFFFFu), end = start + size;
    const uint32_t thread = At32(m.Host(At32(m.Host(ctx.r13.u32 + 256)) + 332));
    if (At32(d + 10760) != thread) {
      AtomicBigEndian<uint64_t>(d + 11544, [&](uint64_t pending) {
        const uint32_t low = std::min(start, uint32_t(pending)), high = std::max(end, uint32_t(pending >> 32));
        return uint64_t(high) << 32 | low;
      });
    } else {
      ctx.r1.u64 = frame;
      const uint32_t cursor = At32(d + 48) > At32(d + 56) ? uint32_t(Call(ctx, base, rt.make_room, device)) : At32(d + 48);
      Out out{m, cursor, records};
      const uint32_t first = start & 0xFFFFF000u, last = (end + 4095) & 0xFFFFF000u;
      out.Registers(0xA31, {At32(m.Host(entry + 84))});
      out.Registers(0xA2F, {last - first, first});
      out.Wait(3, 0xA31, 0, 0x80000000u, 8);
      Set32(d + 48, out.cursor);
    }
  }
  const bool gpu_view = flags & 0x10;
  if (gpu_view || (At32(r) & 0x00200000u)) {
    if (!(flags & 1)) {
      const bool tail = has_tail && tail_base;
      const uint32_t offset = address - (tail ? tail_base : head_base);
      uint8_t* lines = r + (tail ? 24 : 20);
      const uint32_t first = offset >> 7, last = (offset + size + 127) >> 7, now = At32(lines);
      Set32(lines, (std::min(first, now >> 16) << 16) | std::max(last, now & 0xFFFF));
    }
    if (gpu_view) address = GpuAddress(address, 0x1FFFFFFFu) + 0xC0000000u;
  }
  AtomicBigEndian<uint32_t>(r, [](uint32_t count) { return count + 256; });
  ctx.r3.u64 = address;
  ctx.r1.u64 = entry;
  return true;
}
inline void ResourceLockOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && ResourceLock(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}
// sub_826BAD58 (the kick's submit to the primary ring): r3 device, r4 the segment (+0 its dwords, +4 its address). The
// segment handed to the command processor directly at the title's ring write index (+10824), which stays: no ring
// space waited for, no INDIRECT_BUFFER in the ring, no CP_RB_WPTR write (D6). Declines (the runtime writes the ring)
// with any360_direct_submit off, when the device records for a capture tool (+10813 bit 2) or has its callbacks
// (+21156, +21296), or when the command processor does not take it (ring words not kicked yet, a capture of the ring).
inline bool RingSubmit(PPCContext& ctx, const any360::xdk::Memory& m) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32, segment = ctx.r4.u32, entry = ctx.r1.u32;
  if (!DirectSubmit() || !any360::xdk::IsPlainMemory(device, 21300) || !any360::xdk::IsPlainMemory(segment, 8) ||
      !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  if ((d[10813] & 4) || At32(d + 21156) || At32(d + 21296)) return false;
  // Handed over at the title's ring write index, which stays (no room waited for: the ring is not written).
  const uint32_t dwords = At32(m.Host(segment)), address = At32(m.Host(segment + 4));
  return SubmitIndirect(At32(d + 10824), address, dwords);
}
inline void RingSubmitOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && RingSubmit(ctx, m)) return;
  original(ctx, base);
}

// The segments of the command buffer (D6, docs/D3D.md). The kick (Crackdown sub_826BB408) closes the segment being
// written (SegmentClose, sub_826BB1A0): its end packets (SegmentEnd, sub_826BB0C0), its submit (SegmentSubmit,
// sub_826BB348: straight to the ring's submit, or chained after the segments still pending, ChainSegment,
// sub_826BA5C8, under the kernel's lock), then the next segment opened 32-byte aligned after it, or a new one from the
// allocator when the command buffer is full (the runtime's sub_826BAAD0).
namespace detail {
// SegmentEnd's words: INVALIDATE_STATE 0x7FFF, then two EVENT_WRITE_SHD of event 3: `end` (the segment's end) into
// the device's scratch + 4, `counter` (the segments so far) into the scratch.
template <bool kRecords>
inline uint32_t SegmentEndWords(uint32_t* w, uint32_t scratch, uint32_t end, uint32_t counter) {
  namespace record = any360::xdk::record;
  const uint32_t first = GpuAddress(scratch + 4, 0x1FFFFFFFu) | 2, second = GpuAddress(scratch, 0x1FFFFFFFu) | 2;
  if constexpr (kRecords) {
    w[0] = std::byteswap(record::Header(2)), w[1] = record::KindWord(record::kInvalidate, 0), w[2] = 0x7FFF;
    w[3] = std::byteswap(record::Header(record::EventWords(3))), w[4] = record::KindWord(record::kEvent, 0x58);
    w[5] = 3, w[6] = first, w[7] = end;
    w[8] = w[3], w[9] = w[4], w[10] = 3, w[11] = second, w[12] = counter;
    return 13;
  } else {
    const uint32_t v[10] = {0xC0003B00u, 0x7FFF, 0xC0025800u, 3, first, end, 0xC0025800u, 3, second, counter};
    for (uint32_t i = 0; i < 10; ++i) w[i] = std::byteswap(v[i]);
    return 10;
  }
}
}  // namespace detail
// SegmentEnd (sub_826BB0C0): r3 the device, r4 where the words go -> r3 past them. Also: +12956 the word at +14544,
// +12952 the cursor (+48), the segment counter (+10780) up by 2; when the device writes its scratch itself (+10813
// bit 2, +21156 zero), the two values into the scratch (+10768) at once.
inline bool SegmentEnd(PPCContext& ctx, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const uint32_t device = ctx.r3.u32, at = ctx.r4.u32;
  if (!any360::xdk::IsPlainMemory(device, 21300) || !any360::xdk::IsPlainMemory(at, 52) ||
      any360::xdk::detail::Overlap(at, 52, device, 21300))
    return false;
  uint8_t* d = m.Host(device);
  const uint32_t scratch = At32(d + 10768);
  const bool direct = !At32(d + 21156) && (d[10813] & 4);
  if (direct && (!any360::xdk::IsPlainMemory(scratch, 8) || any360::xdk::detail::Overlap(scratch, 8, device, 21300)))
    return false;
  const uint32_t word = At32(d + 14544), cursor = At32(d + 48), counter = At32(d + 10780);
  const uint32_t end = (word & 3) | cursor;
  Set32(d + 12956, word), Set32(d + 12952, cursor);
  uint32_t w[13];
  const uint32_t n = records ? SegmentEndWords<true>(w, scratch, end, counter) : SegmentEndWords<false>(w, scratch, end, counter);
  std::memcpy(m.Host(at), w, 4 * size_t(n));
  if (direct) Set32(m.Host(scratch), counter), Set32(m.Host(scratch + 4), end);
  Set32(d + 10780, counter + 2);
  ctx.r3.u64 = at + 4 * n;
  return true;
}
inline void SegmentEndOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeSegments() && SegmentEnd(ctx, m, DrawRecords())) return;
  original(ctx, base);
}
// ChainSegment (sub_826BA5C8): r3 the chain ({+4 the last segment, +8 the word to link the next one through}), r4 the
// segment's header, r5 its dwords, r6 its tag -> r3 the header's end. The header: its data's address in the
// physical view (past the 16 bytes), 0x82000000 | dwords, the tag, 0xC0000000 (its link, set by the next one); the
// last segment's link set to the header (its CPU flushes do nothing in recompiled code).
inline bool ChainSegment(PPCContext& ctx, const any360::xdk::Memory& m) {
  using namespace detail;
  const uint32_t chain = ctx.r3.u32, header = ctx.r4.u32;
  if (!any360::xdk::IsPlainMemory(chain + 4, 8) || !any360::xdk::IsPlainMemory(header, 16)) return false;
  const uint32_t link = At32(m.Host(chain + 8));
  if (!any360::xdk::IsPlainMemory(link, 4)) return false;
  const uint32_t physical = GpuAddress(header, 0x1FFFFFFFu) - 0x40000000u;
  uint8_t* h = m.Host(header);
  Set32(h + 8, ctx.r6.u32), Set32(h + 12, 0xC0000000u), Set32(h + 4, ctx.r5.u32 | 0x82000000u), Set32(h, physical + 16);
  Set32(m.Host(chain + 8), header + 12), Set32(m.Host(chain + 4), header);
  Set32(m.Host(link), physical);
  ctx.r3.u64 = header + 16;
  return true;
}
inline void ChainSegmentOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeSegments() && ChainSegment(ctx, m)) return;
  original(ctx, base);
}
// SegmentSubmit (sub_826BB348): r3 the device, r4 the segment's end (returned), r5 its address, r6 its dwords, r7
// added to the pending count (+10872), r8 the chain. With no segment pending, the segment goes to the ring's submit
// (its {dwords, address} in the runtime's frame); otherwise it is chained under the kernel's lock: the runtime's.
inline bool SegmentSubmit(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, end = ctx.r4.u32, entry = ctx.r1.u32;
  if (!rt.ring_submit || !any360::xdk::IsPlainMemory(device, 21300) || !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  if (At32(d + 10872)) return false;
  Set32(d + 10872, ctx.r7.u32);
  const uint32_t segment = entry - 96;
  Set32(m.Host(segment), ctx.r6.u32), Set32(m.Host(segment + 4), ctx.r5.u32);
  ctx.r1.u64 = entry - 176;
  Call(ctx, base, rt.ring_submit, device, segment, 1);
  ctx.r1.u64 = entry;
  ctx.r3.u64 = end;
  return true;
}
inline void SegmentSubmitOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeSegments() && SegmentSubmit(ctx, base, m)) return;
  original(ctx, base);
}
// SegmentClose (sub_826BB1A0): r3 the device. Its segment (from +14552 to the cursor +48) ended and submitted, unless
// the device has none (+10813 bit 6); the next one opened at the cursor 32-byte aligned (+14552, the cursor before
// it), or taken from the allocator past the command buffer's end (+56); with a replay pending (+21156), its mark
// (+14540) rounded up to a page. The command buffer being recorded (+10812 bit 7) or tiled (+12944): the runtime's.
inline bool SegmentClose(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, entry = ctx.r1.u32;
  if (!rt.segment_submit || !rt.segment_room || !any360::xdk::IsPlainMemory(device, 21300) ||
      !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  uint32_t cursor = At32(d + 48) + 4;
  if (!(d[10813] & 0x40)) {
    if ((d[10812] & 0x80) || At32(d + 12944)) return false;
    const uint32_t start = At32(d + 14552);
    ctx.r3.u64 = device, ctx.r4.u64 = cursor;
    if (!SegmentEnd(ctx, m, records)) {
      ctx.r3.u64 = device;
      return false;
    }
    const uint32_t end = ctx.r3.u32;
    ctx.r1.u64 = entry - 128;
    ctx.r7.u64 = 0, ctx.r8.u64 = device + 13000;
    cursor = uint32_t(Call(ctx, base, rt.segment_submit, device, end, GpuAddress(start, 0x1FFFFFFFu),
                           uint32_t(int32_t(end - start) >> 2)));
    ctx.r1.u64 = entry;
  }
  const uint32_t next = (cursor + 31) & ~31u;
  if (next > At32(d + 56)) {
    Set32(d + 48, cursor - 4);
    ctx.r1.u64 = entry - 128;
    Call(ctx, base, rt.segment_room, device, 0);
    ctx.r1.u64 = entry;
    return true;
  }
  Set32(d + 14552, next), Set32(d + 48, next - 4);
  if (At32(d + 21156))
    if (const uint32_t mark = At32(d + 14540)) Set32(d + 14540, (mark + 4095) & ~4095u);
  ctx.r3.u64 = cursor;
  return true;
}
inline void SegmentCloseOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeSegments() && SegmentClose(ctx, base, m, DrawRecords())) return;
  original(ctx, base);
}

// The tiled recording (D6, docs/D3D.md). BeginTiling opens it (BeginTiledRecording, Crackdown sub_826CE1C8): the
// segment kicked, the device's segment state saved, and three lists started (+13000 the tiles' ops, +12960 and
// +12980), the first holding the tile rects. EndTiling closes it (EndTiledRecording, sub_826CE360): a fence whose
// interrupt hands the lists to the runtime's DPC, the lists ended, the work submitted as one pending segment. The DPC
// interprets the ops (sub_826CDAF8): each recorded segment submitted once a tile, each tile's bin select first
// (TileSelect, sub_826CD7D8), which the runtime writes into the ring: handed to the command processor here.
// TileSelect (sub_826CD7D8): r3 the interpreter's state (+364 the bin select in force). A new select goes where the
// runtime writes SET_BIN_SELECT_LO into the ring (after waiting for its room), handed to the command processor
// (SubmitBins), or written into the ring when it takes none. Declines with a ring callback (+21156, +21296) or
// any360_direct_submit off.
inline bool TileSelect(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t state = ctx.r3.u32, entry = ctx.r1.u32;
  if (!DirectSubmit() || !rt.ring_space || !rt.device_slot || !any360::xdk::IsPlainMemory(state, 368) ||
      !any360::xdk::IsPlainMemory(rt.device_slot, 4) || !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  const uint32_t slot = At32(m.Host(rt.device_slot));
  if (!any360::xdk::IsPlainMemory(slot, 4)) return false;
  const uint32_t device = At32(m.Host(slot));
  if (!any360::xdk::IsPlainMemory(device, 21300)) return false;
  uint8_t* s = m.Host(state);
  uint8_t* d = m.Host(device);
  const uint32_t mask = any360::xdk::TileMask(At32(s + 40), At32(s + 44));
  if (mask == At32(s + 364)) return true;
  if (At32(d + 21156) || At32(d + 21296)) return false;
  Set32(s + 364, mask);
  const uint32_t position = At32(d + 10824), ring_mask = At32(d + 14524);
  if (SubmitBins(position, 2, mask)) return true;  // the ring and its write index untouched
  // Not taken (a capture of the ring, ring words not kicked yet): the runtime's way, into the ring.
  ctx.r1.u64 = entry - 144;
  Call(ctx, base, rt.ring_space, device, position, 2);
  ctx.r1.u64 = entry;
  const uint32_t ring = At32(d + 14520);
  Set32(m.Host(ring + 4 * position), 0xC0006200u), Set32(m.Host(ring + 4 * ((position + 1) & ring_mask)), mask);
  Set32(d + 10824, (position + 2) & ring_mask);
  return true;
}
inline void TileSelectOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && TileSelect(ctx, base, m)) return;
  original(ctx, base);
}
// BeginTiledRecording (sub_826CE1C8): r3 the device -> r3 the tile ops list's cursor. The tile ops' first op: 0x80000000,
// +12908, the tile count (+12468) and each tile's rect (+12472, 16 bytes a tile) less the screen offset (+12712,
// +12716), 252 bytes in all.
inline bool BeginTiledRecording(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, entry = ctx.r1.u32;
  if (!rt.make_room || !rt.list_block || !any360::xdk::IsPlainMemory(device, 21300) ||
      !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  if (At32(d + 12468) > 15) return false;  // more rects than the op's 252 bytes hold
  ctx.r1.u64 = entry - 128;
  Call(ctx, base, rt.make_room, device);
  static constexpr uint32_t kSaved[6] = {48, 52, 14552, 14548, 14540, 14544};
  for (uint32_t i = 0; i < 6; ++i) Set32(d + 13040 + 4 * i, At32(d + kSaved[i]));
  const uint32_t ends = At32(d + 12956) + 1;
  Set32(d + 12944, At32(d + 12952)), Set32(d + 12948, ends);
  for (uint32_t i = 0; i < 5; ++i) Set32(d + 13020 + 4 * i, At32(d + 13000 + 4 * i));
  for (const uint32_t list : {13000u, 12960u, 12980u}) {
    Set32(d + list + 16, device), Set32(d + list + 8, 0), Set32(d + list + 12, 0), Set32(d + list + 4, 0);
    Call(ctx, base, rt.list_block, device + list);
    Set32(d + list, At32(d + list + 4));
  }
  uint32_t cursor = At32(d + 13008);
  if (cursor + 252 > At32(d + 13012)) cursor = uint32_t(Call(ctx, base, rt.list_block, device + 13000));
  ctx.r1.u64 = entry;
  uint8_t* op = m.Host(cursor);
  Set32(op, 0x80000000u), Set32(op + 4, At32(d + 12908)), Set32(op + 8, At32(d + 12468));
  const uint32_t x = At32(d + 12712), y = At32(d + 12716);
  for (uint32_t i = 0; i < At32(d + 12468); ++i) {
    const uint8_t* rect = d + 12472 + 16 * i;
    Set32(op + 12 + 16 * i, At32(rect) - x), Set32(op + 16 + 16 * i, At32(rect + 4) - y);
    Set32(op + 20 + 16 * i, At32(rect + 8) - x), Set32(op + 24 + 16 * i, At32(rect + 12) - y);
  }
  Set32(d + 13008, cursor + 252);
  ctx.r3.u64 = cursor;
  return true;
}
inline void BeginTiledRecordingOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && BeginTiledRecording(ctx, base, m)) return;
  original(ctx, base);
}
// EndTiledRecording (sub_826CE360): r3 the device, r5 the fence's flags -> r3 1 when the device had no segment (+10813
// bit 6: its saved state put back, the lists left), 0 otherwise. The segment written so far taken; in a block of 100
// dwords, a fence whose interrupt calls Runtime::tiled_callback with the ops list's physical address; the three lists
// ended (their link set, 0xC0000000 at their cursor); the segment so far and then the fence's block submitted as
// pending work (+13020 the saved ops list), the block unless the device writes its scratch itself.
inline bool EndTiledRecording(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, flags = ctx.r5.u32, entry = ctx.r1.u32, frame = entry - 176;
  if (!rt.pending_segment || !rt.allocate || !rt.fence || !rt.segment_submit || !rt.tiled_callback ||
      !any360::xdk::IsPlainMemory(device, 21300) || !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  ctx.r1.u64 = frame;
  if (!(d[10813] & 0x40)) Call(ctx, base, rt.pending_segment, device, frame + 84, frame + 80);
  const uint32_t block = uint32_t(Call(ctx, base, rt.allocate, device, 100, 32));
  uint32_t block_end;
  if (block) {
    const uint32_t head = At32(d + 13000);
    ctx.r7.u64 = head ? GpuAddress(head, 0x1FFFFFFFu) - 0x40000000u : 0;
    block_end = uint32_t(Call(ctx, base, rt.fence, device, block - 4, flags | 1, rt.tiled_callback)) + 4;
  } else {
    block_end = At32(m.Host(frame + 84));
  }
  uint32_t end = uint32_t(Call(ctx, base, rt.allocate, device, 32, 4));
  if (!end) end = At32(m.Host(frame + 84));
  const uint8_t state = d[10813];
  Set32(d + 12944, 0), Set32(d + 12948, 0);
  const uint32_t no_segment = (state >> 6) & 1;
  if (no_segment) {
    static constexpr uint32_t kSaved[6] = {48, 52, 14552, 14548, 14540, 14544};
    for (uint32_t i = 0; i < 6; ++i) Set32(d + kSaved[i], At32(d + 13040 + 4 * i));
    d[10813] = state & 0xBF;
    Set32(d + 56, At32(d + 13044) - 160);
  } else {
    const uint32_t pending = At32(m.Host(frame + 80));
    for (const uint32_t list : {13000u, 12960u, 12980u}) {
      const uint32_t cursor = At32(d + list + 8);
      Set32(m.Host(At32(d + list + 4)), GpuAddress(cursor, 0x1FFFFFFFu) - 0x40000000u);
      Set32(m.Host(At32(d + list + 8)), 0xC0000000u);
      Set32(d + list + 12, 0);
    }
    if (pending) {
      ctx.r7.u64 = 0, ctx.r8.u64 = device + 13020;
      end = uint32_t(Call(ctx, base, rt.segment_submit, device, end, At32(m.Host(frame + 84)), pending));
    }
    if (At32(d + 21156) || !(d[10813] & 4)) {
      ctx.r7.u64 = 1, ctx.r8.u64 = device + 13020;
      Call(ctx, base, rt.segment_submit, device, end, GpuAddress(block, 0x1FFFFFFFu),
           uint32_t(int32_t(block_end - block) >> 2));
    }
  }
  ctx.r1.u64 = entry;
  ctx.r3.u64 = no_segment;
  return true;
}
inline void EndTiledRecordingOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && EndTiledRecording(ctx, base, m)) return;
  original(ctx, base);
}

namespace detail {
// The XDK's tile-wide state BeginTiling and EndTiling both set at the end: whether the render targets are the ones the
// bracket saved (+10812 bit 1), the device's predication words cleared.
inline void TilingTargetsBit(uint8_t* d, bool same) {
  bool on = same;
  if (on) {
    if (d[10812] & 0x08) on = false;
    else if (!(d[10815] & 0x40)) on = !d[11841] && !d[11907];
  }
  uint8_t b = d[10812];
  b = uint8_t((b & ~0x02) | (on ? 0x02 : 0));
  Set32(d + 12432, 0), Set32(d + 10804, 0), Set32(d + 10808, 0), Set32(d + 12428, 0xFFFFFFFFu);
  d[10812] = b;
}
// A word appended to the tile ops list (+13000), a new block taken when it is full.
inline uint32_t TileOps(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, uint32_t device, uint32_t bytes) {
  uint8_t* d = m.Host(device);
  uint32_t cursor = At32(d + 13008);
  if (cursor + bytes > At32(d + 13012)) cursor = uint32_t(Call(ctx, base, g_runtime.list_block, device + 13000));
  return cursor;
}
inline bool PixCallbackSet(const any360::xdk::Memory& m) {
  const Runtime& rt = g_runtime;
  if (!any360::xdk::IsPlainMemory(rt.pix_slot, 4) || !any360::xdk::IsPlainMemory(rt.pix_slot_2, 4)) return true;
  const uint32_t a = At32(m.Host(rt.pix_slot));
  if (!any360::xdk::IsPlainMemory(a, 4)) return true;
  if (const uint32_t b = At32(m.Host(a))) return !any360::xdk::IsPlainMemory(b, 4) || At32(m.Host(b)) != 0;
  const uint32_t c = At32(m.Host(rt.pix_slot_2));
  return !any360::xdk::IsPlainMemory(c, 4) || At32(m.Host(c)) != 0;
}
}  // namespace detail
// BeginTiling (Crackdown sub_826BD488): r3 the device, r4 flags (bit 0: no clear, bit 1: the tiles' EDRAM layout set
// here, bit 2 with it), r5 the tile count, r6 the rects (x1, y1, x2, y2), r7 the clear color (16 bytes) or 0, f1 the
// clear depth, r9 the stencil. The rects kept (+12472) with their 32-aligned corners (+12712); with bit 1 the EDRAM
// tile layout (+12832 each tile's base, +12892 the pitch, +10240's top bits, +12896 the rows); without bit 0 the tiles'
// largest rect cleared under the whole-screen surface; the clear values (+12912 color, +12928 depth, +12932 stencil),
// the render targets saved (+12444 on), the tiled recording opened with its loop op (0x87000000) and an EVENT_WRITE
// of 25 into the command buffer (a record with any360_d3d_records); the viewport put back, every dirty bit set.
// Declines when the profiler's callback is set or for more than 15 tiles.
inline bool BeginTiling(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m, bool records) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, count = ctx.r5.u32, rects = ctx.r6.u32, color = ctx.r7.u32;
  const uint32_t stencil = ctx.r9.u32, entry = ctx.r1.u32, frame = entry - 272;
  const double depth = ctx.f1.f64;
  if (!rt.set_predication || !rt.viewport_of || !rt.scissor_of || !rt.set_viewport || !rt.clear ||
      !rt.restore_viewport || !rt.begin_tiled_recording || !rt.list_block || !rt.make_room || !rt.tiling_surface ||
      !rt.tiling_rect || count > 15 || !any360::xdk::IsPlainMemory(device, 21300) ||
      !any360::xdk::IsPlainMemory(entry - 1024, 1024 + 40) || !any360::xdk::IsPlainMemory(rects, 16 * count + 4) ||
      any360::xdk::detail::Overlap(rects, 16 * count + 4, device, 21300) ||
      (color && !any360::xdk::IsPlainMemory(color & ~15u, 16)) || PixCallbackSet(m))
    return false;
  uint8_t* d = m.Host(device);
  Set32(m.Host(entry + 36), count);
  Set32(d + 12468, count);
  uint32_t right = 0, bottom = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = m.Host(rects + 16 * i);
    right = std::max(right, At32(r + 8)), bottom = std::max(bottom, At32(r + 12));
    for (uint32_t k = 0; k < 4; ++k) Set32(d + 12472 + 16 * i + 4 * k, At32(r + 4 * k));
    Set32(d + 12712 + 8 * i, At32(r) & ~31u), Set32(d + 12716 + 8 * i, At32(r + 4) & ~31u);
  }
  if (flags & 2) {
    d[10815] |= 0x40;
    if (flags & 4) d[10815] |= 0x20;
    const uint32_t samples = (uint32_t(d[10240]) << 8 | d[10241]) & 3;
    const uint32_t row = samples == 0 ? 16 : 8, column = samples <= 1 ? 32 : 16, rows = samples <= 1 ? 80 : 40;
    const uint32_t pitch = (column + right - 1) & ~(column - 1), across = pitch / column;
    for (uint32_t i = 0; i < count; ++i)
      Set32(d + 12832 + 4 * i, At32(d + 12716 + 8 * i) / row * across + At32(d + 12712 + 8 * i) / column);
    const uint32_t height = (rows + right - 1) / rows * rows;
    Set32(d + 12892, pitch);
    Set32(d + 10240, (At32(d + 10240) & 0x0003FFFFu) | (pitch << 18));
    Set64(d + 16, At64(d + 16) | 0x100);
    Set32(d + 12896, height);
    if (!(d[10815] & 0x40) && (At32(d + 11572) & 0x000E0000u) == 0x40000u) {
      Set64(d + 40, At64(d + 40) & ~uint64_t(0x100));
    } else {
      Set64(d + 40, At64(d + 40) | 0x100);
    }
  }
  Set32(d + 12900, right), Set32(d + 12904, bottom), Set32(d + 12908, flags);
  ctx.r1.u64 = frame;
  if (!(flags & 1) && !(d[10815] & 0x40)) {
    uint32_t width = 0, height = 0;
    for (uint32_t i = 0; i < count; ++i) {
      width = std::max(width, At32(d + 12480 + 16 * i) - At32(d + 12712 + 8 * i));
      height = std::max(height, At32(d + 12484 + 16 * i) - At32(d + 12716 + 8 * i));
    }
    Set32(m.Host(frame + 112), 0), Set32(m.Host(frame + 116), 0), Set32(m.Host(frame + 120), width);
    Set32(m.Host(frame + 124), height);
    std::memcpy(m.Host(frame + 144), d + 12368, 28);
    std::memcpy(m.Host(frame + 128), d + 12396, 16);
    ctx.r3.u64 = device, ctx.r4.u64 = rt.tiling_surface;
    rt.viewport_of(ctx, base);
    ctx.r3.u64 = device, ctx.r4.u64 = rt.tiling_rect;
    rt.scissor_of(ctx, base);
    ctx.r3.u64 = device, ctx.r4.u64 = 63, ctx.r5.u64 = frame + 112, ctx.r6.u64 = color, ctx.r8.u64 = stencil;
    ctx.f1.f64 = depth;
    rt.clear(ctx, base);
    const auto single = [&](uint32_t at) {
      const uint32_t bits = At32(m.Host(at));
      float f;
      std::memcpy(&f, &bits, 4);
      return double(f);
    };
    ctx.r3.u64 = device, ctx.r10.u64 = At32(m.Host(frame + 168));
    ctx.f1.f64 = single(frame + 144), ctx.f2.f64 = single(frame + 148), ctx.f3.f64 = single(frame + 152);
    ctx.f4.f64 = single(frame + 156), ctx.f5.f64 = single(frame + 160), ctx.f6.f64 = single(frame + 164);
    rt.set_viewport(ctx, base);
    ctx.r3.u64 = device, ctx.r4.u64 = frame + 128;
    rt.scissor_of(ctx, base);
  }
  if (color) {
    std::memcpy(m.Host((device + 12912) & ~15u), m.Host(color & ~15u), 16);  // lvx128 / stvx: 16-byte aligned
  } else {
    std::memset(d + 12912, 0, 16);
  }
  {
    const float z = float(depth);
    uint32_t bits;
    std::memcpy(&bits, &z, 4);
    Set32(d + 12928, bits);
  }
  Set32(d + 12932, stencil);
  const uint32_t target = At32(d + 12160);
  Set32(d + 12448, target), Set32(d + 12444, target ? target : At32(d + 12176));
  for (uint32_t i = 0; i < 4; ++i) Set32(d + 12452 + 4 * i, At32(d + 12164 + 4 * i));
  Set32(d + 12440, 2), Set32(d + 12436, 0);
  d[10812] |= 0x20;
  TilingTargetsBit(d, true);
  ctx.r3.u64 = device;
  rt.begin_tiled_recording(ctx, base);
  const uint32_t op = TileOps(ctx, base, m, device, 4);
  Set32(m.Host(op), 0x87000000u);
  Set32(d + 13008, op + 4);
  uint32_t cursor = At32(d + 48);
  if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
  Out out{m, cursor, records};
  out.Event(0x46, {25});
  Set32(d + 48, out.cursor);
  ctx.r3.u64 = device, ctx.r4.u64 = rt.tiling_surface;
  rt.viewport_of(ctx, base);
  const uint32_t a = At32(d + 10308), b = At32(d + 10312);
  ctx.r3.u64 = device;
  const auto field = [](uint32_t word) { return uint64_t(int64_t(int32_t(word) >> 17)); };  // srawi: sign-extended
  ctx.r4.u64 = field(a << 17), ctx.r5.u64 = field(a << 1), ctx.r6.u64 = field(b << 17), ctx.r7.u64 = field(b << 1);
  rt.restore_viewport(ctx, base);
  std::memset(d, 0xFF, 40);
  ctx.r1.u64 = entry;
  return true;
}
inline void BeginTilingOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && BeginTiling(ctx, base, m, DrawRecords())) {
    CountBracket(true);
    return;
  }
  CountBracket(false);
  original(ctx, base);
}
// EndTiling (Crackdown sub_826BDA38): r3 the device, r4 the resolve's flags, r5 the resolve rects (0: the tiles'), r6 the
// resolve's target (0: no resolve), r7 its last tile's clear color, f1 its depth, r9 r10 its stencil and more. Each
// tile resolved under its predication (a rect ending on the surface's edge rounded up to 8), the bracket's clear values
// for each but the last; the predication off; the segment kicked; with several tiles, a fence whose interrupt hands
// the tile lists to the DPC (its flags +12908's, or the device's from +11196), the ops 0x8C000000 0x80000000 and
// 0x8B000000 with the other two lists; the loop's end op (0x88000000); the bracket's flags and targets dropped; the
// tiled recording ended -> r3 0, or 0x8007000E (and the runtime's tiling_failed) when the device had no segment.
inline bool EndTiling(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, target = ctx.r6.u32, color = ctx.r7.u32;
  const uint32_t r9 = ctx.r9.u32, r10 = ctx.r10.u32, entry = ctx.r1.u32, frame = entry - 224;
  uint32_t rects = ctx.r5.u32;
  const double depth = ctx.f1.f64;
  if (!rt.set_predication || !rt.tile_samples || !rt.resolve || !rt.make_room || !rt.fence || !rt.list_block ||
      !rt.end_tiled_recording || !rt.viewport_of || !rt.tiling_failed || !rt.tiled_callback || !rt.tiling_surface ||
      !any360::xdk::IsPlainMemory(device, 21300) || !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  uint8_t* d = m.Host(device);
  if (!rects && target) rects = device + 12472;
  if (target && At32(d + 12468) &&
      (!any360::xdk::IsPlainMemory(rects, 16 * At32(d + 12468)) || !any360::xdk::IsPlainMemory(At32(d + 12444) + 36, 4)))
    return false;
  ctx.r1.u64 = frame;
  if (target) {
    for (uint32_t i = 0; i < At32(d + 12468); ++i) {
      const uint32_t rect = rects + 16 * i, scratch = frame + 112;
      const uint32_t surface = At32(m.Host(At32(d + 12444) + 36));
      std::memcpy(m.Host(scratch), m.Host(rect), 16);
      if (At32(m.Host(scratch + 8)) == ((surface >> 18) & 0x3FFF) + 1)
        Set32(m.Host(scratch + 8), (At32(m.Host(scratch + 8)) + 7) & ~7u);
      if (At32(m.Host(scratch + 12)) == ((surface >> 3) & 0x7FFF) + 1)
        Set32(m.Host(scratch + 12), (At32(m.Host(scratch + 12)) + 7) & ~7u);
      Call(ctx, base, rt.set_predication, device, 2 * i < 32 ? 3u << (2 * i) : 0);
      Call(ctx, base, rt.tile_samples, device, (d[10815] & 0x40) ? 0 : 2);
      uint32_t bits = flags;
      ctx.r3.u64 = device, ctx.r5.u64 = scratch, ctx.r6.u64 = target, ctx.r7.u64 = rect, ctx.r8.u64 = 0, ctx.r9.u64 = 0;
      if (i + 1 < At32(d + 12468)) {
        if (At32(d + 12160)) bits |= 0x100;
        if (At32(d + 12176)) bits |= 0x200;
        const uint32_t z = At32(d + 12928);
        float f;
        std::memcpy(&f, &z, 4);
        Set32(m.Host(frame + 100), 0), Set32(m.Host(frame + 92), At32(d + 12932));
        ctx.r10.u64 = device + 12912, ctx.f1.f64 = double(f);
      } else {
        Set32(m.Host(frame + 100), r10), Set32(m.Host(frame + 92), r9);
        ctx.r10.u64 = color, ctx.f1.f64 = depth;
      }
      ctx.r4.u64 = bits;
      rt.resolve(ctx, base);
    }
  }
  Call(ctx, base, rt.set_predication, device, 0);
  Call(ctx, base, rt.tile_samples, device, 2);
  Call(ctx, base, rt.make_room, device);
  const uint32_t shift = At32(d + 11196);
  uint32_t fence_flags = shift < 32 ? 0x01000000u << shift : 0;
  if (At32(d + 12908) & 0x3F000000u) fence_flags = At32(d + 12908) & 0x3F000000u;
  const auto physical = [&](uint32_t list) { return list ? GpuAddress(list, 0x1FFFFFFFu) - 0x40000000u : 0u; };
  if (At32(d + 12468) != 1) {
    Call(ctx, base, rt.set_predication, device, 0x80000000u);
    const uint32_t lists = physical(At32(d + 13000));
    uint32_t cursor = At32(d + 48);
    if (cursor > At32(d + 56)) cursor = uint32_t(Call(ctx, base, rt.make_room, device));
    ctx.r7.u64 = lists;
    Set32(d + 48, uint32_t(Call(ctx, base, rt.fence, device, cursor, fence_flags, rt.tiled_callback)));
    Call(ctx, base, rt.set_predication, device, 0);
    Call(ctx, base, rt.make_room, device);
    uint32_t op = TileOps(ctx, base, m, device, 8);
    Set32(m.Host(op + 4), 0x80000000u), Set32(m.Host(op), 0x8C000000u);
    Set32(d + 13008, op + 8);
    op = TileOps(ctx, base, m, device, 12);
    Set32(m.Host(op), 0x8B000000u);
    Set32(m.Host(op + 4), physical(At32(d + 12960)));
    Set32(m.Host(op + 8), physical(At32(d + 12980)));
    Set32(d + 13008, op + 12);
  } else {
    Call(ctx, base, rt.make_room, device);
  }
  const uint32_t op = TileOps(ctx, base, m, device, 4);
  Set32(m.Host(op), 0x88000000u);
  const uint8_t bits_12 = d[10812] & 0xDF, bits_15 = d[10815] & 0x9F;
  Set32(d + 13008, op + 4), Set32(d + 12444, 0), Set32(d + 12436, 0);
  d[10812] = bits_12, d[10815] = bits_15;
  TilingTargetsBit(d, (bits_12 & 0x10) != 0);
  const uint32_t failed =
      uint32_t(Call(ctx, base, rt.end_tiled_recording, device, At32(d + 12468), fence_flags));
  ctx.r3.u64 = device, ctx.r4.u64 = rt.tiling_surface;
  rt.viewport_of(ctx, base);
  if (failed) {
    Call(ctx, base, rt.tiling_failed, device, 0);
    ctx.r3.u64 = 0x8007000Eu;
  } else {
    ctx.r3.u64 = 0;
  }
  ctx.r1.u64 = entry;
  return true;
}
// The tile ops interpreter (Crackdown sub_826CDAF8, any360/xdk/d3d_tiling.h): r3 the ops, r4 its state -> r3 where the
// run stopped. Its calls are the title's (TileSelect and the ring's submit Any360's where bound), from the
// interpreter's frame (a segment's {dwords, address} at +80).
inline bool TileOps(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  using namespace detail;
  const Runtime& rt = g_runtime;
  const uint32_t ops = ctx.r3.u32, state = ctx.r4.u32, entry = ctx.r1.u32, frame = entry - 160;
  if (!rt.tile_select || !rt.ring_submit || !rt.tile_resources || !rt.tile_wait || !rt.lock_l2 || !rt.unlock_l2 ||
      !rt.device_slot || !any360::xdk::IsPlainMemory(rt.device_slot, 4) || !any360::xdk::IsPlainMemory(state, 380) ||
      !any360::xdk::IsPlainMemory(ops, 4) || !any360::xdk::IsPlainMemory(entry - 1024, 1024))
    return false;
  const uint32_t slot = At32(m.Host(rt.device_slot));
  if (!any360::xdk::IsPlainMemory(slot, 4)) return false;
  const uint32_t device = At32(m.Host(slot));
  struct Calls : any360::xdk::TileOpsCalls {
    PPCContext& ctx;
    uint8_t* base;
    const any360::xdk::Memory& m;
    const Runtime& rt;
    uint32_t device, frame;
    Calls(PPCContext& c, uint8_t* b, const any360::xdk::Memory& mem, const Runtime& r, uint32_t d, uint32_t f)
        : ctx(c), base(b), m(mem), rt(r), device(d), frame(f) {}
    void Select(uint32_t state) override { Call(ctx, base, rt.tile_select, state); }
    void Submit(uint32_t dwords, uint32_t address) override {
      Set32(m.Host(frame + 80), dwords), Set32(m.Host(frame + 84), address);
      Call(ctx, base, rt.ring_submit, device, frame + 80, 1);
    }
    void Resources(uint32_t header, uint32_t list, bool flush) override {
      Call(ctx, base, rt.tile_resources, header, list, flush ? 1 : 0);
    }
    void Wait(uint32_t state) override { Call(ctx, base, rt.tile_wait, state); }
    void LockL2() override {
      ctx.r7.u64 = 1;
      Call(ctx, base, rt.lock_l2, 0, 0x7F100000u, 0x20000, 1);
      std::memset(m.Host(0x7F100000u), 0, 0x20000);  // dcbzl over the locked lines
    }
    void UnlockL2() override { Call(ctx, base, rt.unlock_l2, 0); }
    void Again() override { std::this_thread::yield(); }
  } calls(ctx, base, m, rt, device, frame);
  ctx.r1.u64 = frame;
  const uint32_t stop = any360::xdk::RunTileOps(m, ops, state, calls);
  ctx.r1.u64 = entry;
  ctx.r3.u64 = stop;
  return true;
}
inline void TileOpsOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && TileOps(ctx, base, m)) {
    CountTileRun(true);
    return;
  }
  CountTileRun(false);
  original(ctx, base);
}
inline void EndTilingOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeDraws() && NativeTiling() && EndTiling(ctx, base, m)) return;
  original(ctx, base);
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

// D3DResource_AddRef (Crackdown sub_826ACC90): r3 the resource -> r3 its new count; and D3DResource_Release
// (sub_826ACD08): r3 the resource -> r3 its new count, at 0 its parent released and the resource destroyed through
// the title's function (Runtime::destroy_resource, r1 the runtime's frame at that depth, 96 bytes a level). The model
// and what it declines: any360/xdk/d3d.h (D3dResourceAddRef, D3dResourceRelease).
inline bool ResourceAddRef(PPCContext& ctx, const any360::xdk::Memory& m) {
  uint64_t count;
  if (!any360::xdk::D3dResourceAddRef(m, ctx.r3.u32, count)) return false;
  ctx.r3.u64 = count;
  return true;
}
inline bool ResourceRelease(PPCContext& ctx, uint8_t* base, const any360::xdk::Memory& m) {
  if (!g_runtime.destroy_resource) return false;
  const uint64_t entry = ctx.r1.u64;
  auto destroy = [&](uint32_t resource, uint32_t depth) {
    ctx.r1.u64 = entry - 96 * (depth + 1);
    ctx.r3.u64 = resource;
    g_runtime.destroy_resource(ctx, base);
  };
  uint64_t count;
  if (!any360::xdk::D3dResourceRelease(m, ctx.r3.u32, destroy, count)) return false;
  ctx.r1.u64 = entry;
  ctx.r3.u64 = count;
  return true;
}
inline void ResourceAddRefOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeRefCounts() && ResourceAddRef(ctx, m)) return;
  original(ctx, base);
}
inline void ResourceReleaseOrOriginal(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const any360::xdk::Memory& m = xdk::g_memory;
  if (m.base == base && NativeRefCounts() && ResourceRelease(ctx, base, m)) return;
  original(ctx, base);
}

}  // namespace any360::host::rexglue::d3d_draw
