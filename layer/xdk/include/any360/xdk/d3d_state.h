// any360::xdk: the XDK D3D runtime's state setters (its entry functions: they write the device's fields and dirty
// bits, no command buffer), native. The device stays in guest memory in the runtime's own layout (D3dLayout: where
// a runtime keeps each field), so that the runtime's own flush functions and the native ones read the same state.
// Each function here does what the runtime's function leaves in memory, byte for byte; registers other than r3 are
// the caller's scratch. A call the function would handle in a way the replacement does not (memory that is not
// plain, a count of 0 the runtime loops on for ever, overlapping source and destination) is declined: the runtime's
// own code runs. The rule this follows: docs/NATIVE_FUNCTIONS.md; the plan: docs/D3D.md (phase D1).
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

#include "crt.h"
#include "d3d.h"

namespace any360::xdk {

// Where a D3D runtime keeps the state its setters write, as byte offsets in its device. The defaults are those of
// the runtime Crackdown links; another runtime gives its own.
struct D3dLayout {
  uint32_t state_dirty = 16;     // 64 bits: the render state's dirty bits
  uint32_t constant_dirty = 32;  // 64 bits: the bool and integer constants' (bit 56)
  // The blend word as the title set it (color fields low, alpha fields high) and its flags (bit 31 blending on,
  // bit 30 separate alpha), then the render targets' blend words, which the flush sends.
  uint32_t blend_control = 11568, blend_flags = 11572;
  uint32_t target_blend[4] = {10424, 10456, 10460, 10464};
  // Vertex then pixel: bool constants (a bit a register), integer constants (a word a register), float constants
  // (16 bytes a register, as a register offset: the register's place is (first + base) * 16) and their dirty bits.
  uint32_t bool_constants[2] = {2496 * 4, 2500 * 4};
  uint32_t int_constants[2] = {2504 * 4, 2520 * 4};
  uint32_t float_constant_base[2] = {112, 368};
  uint32_t float_dirty[2] = {0, 8};
  // The device bytes the blend setters touch, from the device's start (plain-memory check).
  uint32_t blend_extent = 11576;
  // The device's size as the resource binders check it (no field of theirs past it).
  uint32_t device_bytes = 16384;
  // Fetch constants (24 bytes a texture stage from fetch_constants; the vertex fetch constants, two words a stream,
  // down from vertex_fetch_last for stream 0) and their 64 dirty bits.
  uint32_t fetch_constants = 1024, vertex_fetch_last = 1784, fetch_dirty = 24;
  // The bound resources (a word a texture stage, a word a stream), the stages' mip limits (a byte each), the
  // streams' strides in words as set and as the vertex shader has them (a byte each).
  uint32_t texture_bound = 12264, stream_bound = 12180, texture_mip_floor = 11670, texture_mip_ceiling = 11696;
  uint32_t stream_stride = 12248, shader_stream_stride = 11552;
  // A replaced resource's release (D3dSetTexture): the pending fence, the busy mask, the release list's cursor and
  // end.
  uint32_t resource_fence = 10780, resource_busy_mask = 10784, release_cursor = 13148, release_end = 13152;
};

namespace detail {

inline uint32_t Get32(const Memory& m, uint32_t address) { return std::byteswap(Load32(m.Host(address))); }
inline void Put32(const Memory& m, uint32_t address, uint32_t v) { Store32(m.Host(address), std::byteswap(v)); }
inline void Or64(const Memory& m, uint32_t address, uint64_t bits) {
  uint8_t* p = m.Host(address);
  Store64(p, ByteSwap64(ByteSwap64(Load64(p)) | bits));
}

// With separate alpha off, the alpha half of the blend word follows the color half: the color fields copied up
// (source to alpha source, operation to alpha operation, destination to alpha destination) and two bits cleared.
inline uint32_t BlendAlphaFromColor(uint32_t control) {
  const uint32_t color = control & 0xFFFFu;
  const uint32_t alpha = (((color << 4) | (color & 0x1010u)) << 12) & 0xFFFF0000u & ~0x00100000u & ~0x10000000u;
  return alpha | color;
}
// Big-endian words at a host pointer (the device's fields, under the one host pointer IsPlainMemory guarantees).
inline uint32_t At32(const uint8_t* p) { return std::byteswap(Load32(p)); }
inline void Set32(uint8_t* p, uint32_t v) { Store32(p, std::byteswap(v)); }
// The blend word sent: into each render target's, and the four dirty bits of the flush that sends them.
inline void StoreTargetBlend(uint8_t* d, const D3dLayout& l, uint32_t word) {
  const uint32_t be = std::byteswap(word);
  for (uint32_t target : l.target_blend) Store32(d + target, be);
  uint8_t* dirty = d + l.state_dirty;
  Store64(dirty, ByteSwap64(ByteSwap64(Load64(dirty)) | 0x407));
}
inline bool Overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) { return !Disjoint(a, a_size, b, b_size); }

}  // namespace detail

// SetRenderState for one field of the blend word: color source (bits 0-4), operation (5-7), destination (8-12),
// alpha source (16-20), operation (21-23), destination (24-28). The word as set is kept; with blending on it is
// sent (a color field: with its alpha copy unless separate alpha is on; an alpha field: only with separate alpha on).
enum class D3dBlendField : uint32_t { kColorSource, kColorOperation, kColorDestination, kAlphaSource, kAlphaOperation, kAlphaDestination };
struct D3dBlendFieldBits {
  uint32_t shift, mask;
  bool alpha;
};
inline constexpr D3dBlendFieldBits kD3dBlendFields[6] = {{0, 0x1Fu, false},       {5, 0xE0u, false},
                                                          {8, 0x1F00u, false},     {16, 0x1F0000u, true},
                                                          {21, 0xE00000u, true},   {24, 0x1F000000u, true}};

inline bool D3dBlendAccepts(const D3dLayout& l, uint32_t device) { return IsPlainMemory(device, l.blend_extent); }

inline bool D3dSetBlendField(const Memory& m, const D3dLayout& l, uint32_t device, D3dBlendField field, uint32_t value) {
  if (!D3dBlendAccepts(l, device)) return false;
  uint8_t* d = m.Host(device);
  const D3dBlendFieldBits bits = kD3dBlendFields[uint32_t(field)];
  const uint32_t flags = detail::At32(d + l.blend_flags);
  const uint32_t control = (detail::At32(d + l.blend_control) & ~bits.mask) | ((value << bits.shift) & bits.mask);
  detail::Set32(d + l.blend_control, control);
  if (!(flags & 0x80000000u)) return true;
  const bool separate = (flags & 0x40000000u) != 0;
  if (bits.alpha) {
    if (separate) detail::StoreTargetBlend(d, l, control);
  } else {
    detail::StoreTargetBlend(d, l, separate ? control : detail::BlendAlphaFromColor(control));
  }
  return true;
}

// SetRenderState for blending on or off (`value` bit 0): kept in the flags; the word sent is ONE/ZERO (0x00010001)
// when off.
inline bool D3dSetBlendEnable(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t value) {
  if (!D3dBlendAccepts(l, device)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t flags = (detail::At32(d + l.blend_flags) & 0x7FFFFFFFu) | (value << 31);
  detail::Set32(d + l.blend_flags, flags);
  const uint32_t control = detail::At32(d + l.blend_control);
  uint32_t word = (flags & 0x40000000u) ? control : detail::BlendAlphaFromColor(control);
  if (value == 0) word = 0x00010001u;
  detail::StoreTargetBlend(d, l, word);
  return true;
}

// SetRenderState for separate alpha blending (`value` bit 0): kept in the flags; the word sent with the alpha half
// copied from the color half when off, ONE/ZERO when blending is off.
inline bool D3dSetSeparateAlphaBlend(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t value) {
  if (!D3dBlendAccepts(l, device)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t flags = (detail::At32(d + l.blend_flags) & ~0x40000000u) | ((value & 1u) << 30);
  detail::Set32(d + l.blend_flags, flags);
  uint32_t word = detail::At32(d + l.blend_control);
  if (value == 0) word = detail::BlendAlphaFromColor(word);
  if (!(flags & 0x80000000u)) word = 0x00010001u;
  detail::StoreTargetBlend(d, l, word);
  return true;
}

// SetVertexShaderConstantB / SetPixelShaderConstantB (`pixel`): `count` registers from `first`, bit 0 of each word
// at `source` into the device's bits, and the constants' dirty bit. The runtime loops 2^32 times on a count of 0:
// declined, as are counts past 256, a register past 2^32 and a source that overlaps the device's words.
inline bool D3dBoolConstantsAccepts(const D3dLayout& l, uint32_t device, bool pixel, uint32_t first, uint32_t source,
                                    uint32_t count, Range& words) {
  if (count == 0 || count > 256 || uint64_t(first) + count > 0x100000000ull) return false;
  const uint32_t first_word = uint32_t(device + ((((first >> 5) + l.bool_constants[pixel] / 4) << 2)));
  const uint32_t last_word = uint32_t(device + (((((first + count - 1) >> 5) + l.bool_constants[pixel] / 4) << 2)));
  if (last_word < first_word) return false;
  words = {first_word, last_word - first_word + 4};
  return IsPlainMemory(words.at, words.size) && IsPlainMemory(source, 4 * count) &&
         IsPlainMemory(device + l.constant_dirty, 8) && !detail::Overlap(words.at, words.size, source, 4 * count) &&
         !detail::Overlap(device + l.constant_dirty, 8, source, 4 * count) &&
         !detail::Overlap(device + l.constant_dirty, 8, words.at, words.size);
}
inline bool D3dSetBoolConstants(const Memory& m, const D3dLayout& l, uint32_t device, bool pixel, uint32_t first,
                                uint32_t source, uint32_t count) {
  Range words;
  if (!D3dBoolConstantsAccepts(l, device, pixel, first, source, count, words)) return false;
  // A word at a time: the bits of its registers gathered, then one read and one write of the device's word.
  uint32_t i = 0;
  while (i < count) {
    const uint32_t reg = first + i;
    const uint32_t address = words.at + 4 * ((reg >> 5) - (first >> 5));
    const uint32_t in_word = std::min(32 - (reg & 31), count - i);
    uint32_t set = 0, mask = 0;
    for (uint32_t k = 0; k < in_word; ++k) {
      const uint32_t bit = (reg + k) & 31;
      set |= (detail::Get32(m, source + 4 * (i + k)) & 1u) << bit;
      mask |= 1u << bit;
    }
    detail::Put32(m, address, (detail::Get32(m, address) & ~mask) | set);
    i += in_word;
  }
  detail::Or64(m, device + l.constant_dirty, uint64_t(1) << 56);
  return true;
}

// SetVertexShaderConstantI / SetPixelShaderConstantI (`pixel`): `count` registers from `first`, each 16 bytes at
// `source` (four integers, of which the runtime keeps the low bytes of the first three: count, start, step) packed
// into the device's word, and the constants' dirty bit. Declined as the bool constants are.
inline bool D3dIntConstantsAccepts(const D3dLayout& l, uint32_t device, bool pixel, uint32_t first, uint32_t source,
                                   uint32_t count, Range& words) {
  if (count == 0 || count > 256) return false;
  const uint32_t at = uint32_t(device + (((first + l.int_constants[pixel] / 4) << 2) & 0xFFFFFFFCu));
  words = {at, 4 * count};
  return IsPlainMemory(words.at, words.size) && IsPlainMemory(source, 16 * count) &&
         IsPlainMemory(device + l.constant_dirty, 8) && !detail::Overlap(words.at, words.size, source, 16 * count) &&
         !detail::Overlap(device + l.constant_dirty, 8, source, 16 * count) &&
         !detail::Overlap(device + l.constant_dirty, 8, words.at, words.size);
}
inline bool D3dSetIntConstants(const Memory& m, const D3dLayout& l, uint32_t device, bool pixel, uint32_t first,
                               uint32_t source, uint32_t count) {
  Range words;
  if (!D3dIntConstantsAccepts(l, device, pixel, first, source, count, words)) return false;
  const uint8_t* in = m.Host(source);
  uint8_t* out = m.Host(words.at);
  for (uint32_t i = 0; i < count; ++i, in += 16, out += 4) {
    out[0] = 0, out[1] = in[11], out[2] = in[7], out[3] = in[3];
  }
  detail::Or64(m, device + l.constant_dirty, uint64_t(1) << 56);
  return true;
}

// The resource binders (SetTexture, SetStreamSource). Binding a resource in a slot replaces the one there: the old
// one is marked as used by the GPU until the commands so far are done, by one of two means. With a fence pending
// (the device's word at resource_fence), that fence goes into the resource (its word at +8). Otherwise, when the
// resource's first word has a bit of the device's busy mask, the resource goes into the device's release list: a
// record of two words (the resource's address >> 2, then 0xFFFFFFFF) at the list's cursor. The record's first word
// keeps the top bit of the function's own stack word it builds the record in, which the function never sets (its
// value is whatever an earlier call left there, below the caller's stack pointer: `stack_word`, read and written
// the same way here). A list block that is full makes the runtime take a new one from the command buffer: declined.
namespace detail {
struct Release {
  enum Kind { kNone, kFence, kRecord } kind = kNone;
  uint32_t old = 0, fence = 0, record = 0;
};
inline bool PlanRelease(const Memory& m, const D3dLayout& l, const uint8_t* d, uint32_t device, uint32_t old,
                        uint32_t stack_word, Release& r) {
  r = {};
  r.old = old;
  if (!old) return true;
  if (!IsPlainMemory(old, 12) || Overlap(old, 12, device, l.device_bytes)) return false;
  r.fence = At32(d + l.resource_fence);
  if (r.fence) {
    r.kind = Release::kFence;
    return true;
  }
  if (!(At32(d + l.resource_busy_mask) & At32(m.Host(old)))) return true;
  const uint32_t cursor = At32(d + l.release_cursor), end = At32(d + l.release_end);
  if (cursor >= end) return false;  // a new block: the runtime's own code
  if (!IsPlainMemory(cursor, 8) || Overlap(cursor, 8, device, l.device_bytes) || Overlap(cursor, 8, old, 12) ||
      !IsPlainMemory(stack_word, 8) || Overlap(stack_word, 8, device, l.device_bytes) || Overlap(stack_word, 8, old, 12) ||
      Overlap(stack_word, 8, cursor, 8))
    return false;
  r.kind = Release::kRecord;
  r.record = cursor;
  return true;
}
inline void DoRelease(const Memory& m, const D3dLayout& l, uint8_t* d, const Release& r, uint32_t stack_word) {
  if (r.kind == Release::kFence) {
    Set32(m.Host(r.old + 8), r.fence);
  } else if (r.kind == Release::kRecord) {
    const uint32_t first = (At32(m.Host(stack_word)) & 0x80000000u) | (r.old >> 2);
    Set32(m.Host(stack_word), first);
    Set32(m.Host(stack_word + 4), 0xFFFFFFFFu);
    Set32(m.Host(r.record), first);
    Set32(m.Host(r.record + 4), 0xFFFFFFFFu);
    Set32(d + l.release_cursor, r.record + 8);
  }
}
// A GPU address from a CPU one, as the runtime makes it: the low 29 bits, plus 4 KB when the top 12 bits + 512
// carry into bit 12 (the physical view's pages at 0xE0000000 and up start 4 KB further).
inline uint32_t GpuAddress(uint32_t cpu, uint32_t low_mask) { return (((cpu >> 20) + 512) & 0x1000u) + (cpu & low_mask); }
}  // namespace detail

// SetTexture: r4 the stage, r5 the texture (its fetch constant's six words at +28, as built at its creation) or 0,
// r6 the fetch constants' dirty bits; r1 the caller's stack pointer (the function's own record word at r1 - 80).
// The stage's fetch constant takes the texture's, keeping some of the slot's own bits (the sampler state's), the
// mip range clamped by the stage's limits; the slot's pointer takes the texture, the old one is released.
inline bool D3dSetTexture(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t stage, uint32_t texture,
                          uint64_t dirty, uint32_t stack_pointer) {
  if (stage >= 32 || !IsPlainMemory(device, l.device_bytes)) return false;
  if (texture && (!IsPlainMemory(texture + 28, 24) || detail::Overlap(texture + 28, 24, device, l.device_bytes)))
    return false;
  uint8_t* d = m.Host(device);
  const uint32_t stack_word = stack_pointer - 80;
  const uint32_t slot = l.texture_bound + 4 * stage;
  detail::Release release;
  if (!detail::PlanRelease(m, l, d, device, detail::At32(d + slot), stack_word, release)) return false;
  if (texture) {
    using detail::At32;
    const uint8_t* t = m.Host(texture + 28);
    uint8_t* f = d + l.fetch_constants + 24 * stage;
    const uint32_t t0 = At32(t), t1 = At32(t + 4), t2 = At32(t + 8), t3 = At32(t + 12), t4 = At32(t + 16),
                   t5 = At32(t + 20);
    const uint32_t f0 = At32(f), f1 = At32(f + 4), f3 = At32(f + 12), f4 = At32(f + 16), f5 = At32(f + 20);
    const uint32_t low = std::max((t4 >> 2) & 0xFu, uint32_t(d[l.texture_mip_floor + stage]));
    const uint32_t high = std::min((t4 >> 6) & 0xFu, uint32_t(d[l.texture_mip_ceiling + stage]));
    detail::Set32(f, (t0 & ~0x003FFC00u) | (f0 & 0x003FFC00u));
    detail::Set32(f + 4, (detail::GpuAddress(t1, 0x1FFFFFFFu) & ~0x800u) | (f1 & 0x800u));
    detail::Set32(f + 8, t2);
    detail::Set32(f + 12, (t3 & ~0x7FF80000u) | (f3 & 0x7FF80000u));
    // The texture's mip bits (0x3FC) give way to its range clamped by the stage's limits.
    detail::Set32(f + 16, (f4 & 0xFFFFFC03u) | ((low << 2) & 0x3Cu) | ((high << 6) & 0x3C0u));
    detail::Set32(f + 20, (detail::GpuAddress(t5, 0x1FFFFE00u) & ~0x1FFu) | (f5 & 0x1FFu));
    uint8_t* bits = d + l.fetch_dirty;
    detail::Store64(bits, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(bits)) | dirty));
  }
  detail::Set32(d + slot, texture);
  detail::DoRelease(m, l, d, release, stack_word);
  return true;
}

// SetStreamSource: r4 the stream (0-15), r5 the vertex buffer (its address and size at +24 and +28) or 0, r6 the
// offset into it, r7 the stride, r8 the fetch constants' dirty bits; r1 the caller's stack pointer (record word at
// r1 - 64). The stream's vertex fetch constant (two words, the last stream's first) takes the buffer's GPU address
// from the offset and the size left; the stream's stride in words is kept (and the vertex shader's stride bit of
// the render state's dirty bits set when it differs from the shader's); the old buffer is released.
inline bool D3dSetStreamSource(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t stream, uint32_t buffer,
                               uint32_t offset, uint32_t stride, uint64_t dirty, uint32_t stack_pointer) {
  if (stream > 15 || !IsPlainMemory(device, l.device_bytes)) return false;
  if (buffer && (!IsPlainMemory(buffer + 24, 8) || detail::Overlap(buffer + 24, 8, device, l.device_bytes))) return false;
  uint8_t* d = m.Host(device);
  const uint32_t stack_word = stack_pointer - 64;
  const uint32_t slot = l.stream_bound + 4 * stream;
  detail::Release release;
  if (!detail::PlanRelease(m, l, d, device, detail::At32(d + slot), stack_word, release)) return false;
  if (buffer) {
    const uint8_t* b = m.Host(buffer + 24);
    uint8_t* f = d + l.vertex_fetch_last - 8 * stream;
    detail::Set32(f + 4, detail::At32(b + 4) - offset);
    detail::Set32(f, detail::GpuAddress(detail::At32(b) + offset, 0x1FFFFFFFu));
    uint8_t* bits = d + l.fetch_dirty;
    detail::Store64(bits, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(bits)) | dirty));
  }
  detail::DoRelease(m, l, d, release, stack_word);
  detail::Set32(d + slot, buffer);
  const uint32_t words = stride >> 2;
  d[l.stream_stride + stream] = uint8_t(words);
  if (words && words != d[l.shader_stream_stride + stream]) {
    uint8_t* bits = d + l.state_dirty;
    detail::Store64(bits, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(bits)) | 0x80000u));
  }
  return true;
}

// SetPixelShaderConstantF: D3dSetShaderConstants (d3d.h) at the pixel registers and their dirty bits.
inline bool D3dSetPixelShaderConstants(const Memory& m, const D3dLayout& l, uint64_t device, uint64_t first,
                                       uint32_t source, uint32_t count, uint64_t dirty, uint32_t stack_pointer) {
  return D3dSetShaderConstants(m, device, first, source, count, dirty, stack_pointer, l.float_constant_base[1],
                               l.float_dirty[1]);
}

}  // namespace any360::xdk
