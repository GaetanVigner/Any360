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

// SetPixelShaderConstantF: D3dSetShaderConstants (d3d.h) at the pixel registers and their dirty bits.
inline bool D3dSetPixelShaderConstants(const Memory& m, const D3dLayout& l, uint64_t device, uint64_t first,
                                       uint32_t source, uint32_t count, uint64_t dirty, uint32_t stack_pointer) {
  return D3dSetShaderConstants(m, device, first, source, count, dirty, stack_pointer, l.float_constant_base[1],
                               l.float_dirty[1]);
}

}  // namespace any360::xdk
