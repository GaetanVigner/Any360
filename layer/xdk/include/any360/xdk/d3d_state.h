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
  // The sampler filters (D3dSetSamplerFilter): the runtime's table of anisotropic bits (a guest address in the
  // title's image), each stage's index into it and its LOD limit (a byte each).
  uint32_t sampler_filter_table = 0x820E9E30u, stage_filter_index = 11644, stage_lod_limit = 11722;
  // The bound vertex declaration and shaders, and the flags byte whose bit 0 a vertex shader clears.
  uint32_t vertex_declaration = 12156, pixel_shader = 12412, vertex_shader = 12416, shader_flags = 10813;
  // The registers' mirror (a word a register): PA_CL_CLIP_CNTL (0x2204), PA_SU_SC_MODE_CNTL (0x2205), PA_CL_VTE_CNTL
  // (0x2206), the polygon offsets (front scale, front offset, back scale, back offset: 0x2380-0x2383), and the runtime's
  // 0.0f the depth bias is compared with (a guest address in the title's image).
  uint32_t clip_control = 10436, mode_control = 10440, vte_control = 10444, poly_offset = 10704;
  uint32_t zero_float = 0x820722DCu;
  // The bound surfaces: render targets 0-3 then the depth surface (a word each), and those a tiling recorded (same
  // order). The mirror's RB_SURFACE_INFO (0x2000), RB_COLOR_INFO (0x2001; RB_DEPTH_INFO 0x2002, RB_COLOR1-3_INFO
  // 0x2003-0x2005 after it), RB_COLOR_MASK (0x2104), RB_DEPTHCONTROL (0x2200), the word of 0x2203 a depth surface sets.
  uint32_t surfaces = 12160, tiled_surfaces = 12448;
  uint32_t surface_info = 10240, color_info = 10244, color_mask = 10332, depth_control = 10420, depth_word = 10432;
  // As the title set them: the color write masks (a word a target), the depth and stencil enables; a word a target:
  // its 2_10_10_10 formats taken as their _AS_ variants (1) or not (0).
  uint32_t color_mask_set = 11580, depth_enable_set = 11596, stencil_enable_set = 11600, target_as_wide = 11748;
  // The tiling's state: the bin mask's word (non-zero: one set), the flags (bit 4 tiled, bit 5 tiled when the
  // surfaces match the recorded ones, bit 3 none, bit 1 the draws' predicated-query variant), the depth's flags (bit 6
  // the surface's word 32 bit 0), the options (bit 6), two bytes the query also needs at 0, the hi-z pitch.
  uint32_t bin_mask_set = 12436, tiling_flags = 10812, depth_flags = 10814, tiling_options = 10815;
  uint32_t tiling_pending[2] = {11841, 11907};
  uint32_t tiling_hiz_pitch = 12892;
  // The command buffer's cursor and the point past which a writer first makes room (the runtime's own check).
  uint32_t command_cursor = 48, command_end = 56;
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

// SetRenderState_ViewportEnable (Crackdown sub_826B0AA0): PA_CL_VTE_CNTL 0x43F (the viewport's scale and offset
// applied) or, with `value` 0, 0x400 and PA_CL_CLIP_CNTL's clip_disable (bit 16) set; the state's dirty bits 0x20 and
// 0x80. Declined: the device's fields not plain memory.
inline bool D3dSetViewportEnable(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t value) {
  if (!IsPlainMemory(device, l.vte_control + 4) || !IsPlainMemory(device, l.state_dirty + 8)) return false;
  uint8_t* d = m.Host(device);
  detail::Set32(d + l.vte_control, value ? 0x43Fu : 0x400u);
  detail::Set32(d + l.clip_control, (detail::At32(d + l.clip_control) & ~0x10000u) | (value ? 0u : 0x10000u));
  detail::Or64(m, device + l.state_dirty, 0xA0);
  return true;
}

// SetRenderState_DepthBias (Crackdown sub_826B0488): `value` a float's bits, kept in the caller's parameter area
// (`stack_word`, the caller's r1 + 28) as the runtime does, then as the front and back polygon offsets (through a
// double, as its lfs/stfs); PA_SU_SC_MODE_CNTL's poly_offset_front_enable (bit 11) and back_enable (bit 12) set unless
// that side's scale and the offset both equal the runtime's 0.0f (a float comparison: -0 is 0, NaN never equal); the
// dirty bits 42 and 44 of the word at constant_dirty and 0x40 of the state's. The caller runs it with the host's
// denormals kept (the runtime's code turns their flush off first). Declined: memory not plain, or the parameter area
// over the device's fields or the 0.0f.
inline bool D3dSetDepthBias(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t value, uint32_t stack_word) {
  if (!IsPlainMemory(device, l.poly_offset + 16) || !IsPlainMemory(device, l.state_dirty + 8) ||
      !IsPlainMemory(device, l.constant_dirty + 8) || !IsPlainMemory(stack_word, 4) || !IsPlainMemory(l.zero_float, 4) ||
      detail::Overlap(stack_word, 4, device, l.poly_offset + 16) || detail::Overlap(stack_word, 4, l.zero_float, 4)) {
    return false;
  }
  detail::Put32(m, stack_word, value);
  uint8_t* d = m.Host(device);
  const double zero = double(std::bit_cast<float>(detail::Get32(m, l.zero_float)));
  const double front_scale = double(std::bit_cast<float>(detail::At32(d + l.poly_offset)));
  const double bias = double(std::bit_cast<float>(value));
  const uint32_t stored = std::bit_cast<uint32_t>(float(bias));
  detail::Set32(d + l.poly_offset + 4, stored);
  detail::Set32(d + l.poly_offset + 12, stored);
  const double back_scale = double(std::bit_cast<float>(detail::At32(d + l.poly_offset + 8)));
  const bool front = !(front_scale == zero) || !(bias == zero);
  const bool back = !(back_scale == zero) || !(bias == zero);
  detail::Set32(d + l.mode_control,
                (detail::At32(d + l.mode_control) & ~0x1800u) | (front ? 0x800u : 0u) | (back ? 0x1000u : 0u));
  detail::Or64(m, device + l.constant_dirty, (uint64_t(1) << 44) | (uint64_t(1) << 42));
  detail::Or64(m, device + l.state_dirty, 0x40);
  return true;
}

namespace detail {
// The tiling's surfaces as the runtime checks them: flags bit 4, or bit 5 with each bound surface the recorded one
// (or none bound there).
inline bool SurfacesTiled(const uint8_t* d, const D3dLayout& l) {
  const uint8_t flags = d[l.tiling_flags];
  if (flags & 0x10) return true;
  if (!(flags & 0x20)) return false;
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t bound = At32(d + l.surfaces + 4 * k);
    if (At32(d + l.tiled_surfaces + 4 * k) != bound && bound != 0) return false;
  }
  return true;
}
// The draws' predicated-query variant (tiling flags bit 1): no bin mask set, the surfaces tiled, flags bit 3 off,
// and options bit 6 or both pending bytes 0.
inline void SetTilingQuery(uint8_t* d, const D3dLayout& l) {
  const bool query = At32(d + l.bin_mask_set) == 0 && SurfacesTiled(d, l) && !(d[l.tiling_flags] & 0x08) &&
                     ((d[l.tiling_options] & 0x40) || (d[l.tiling_pending[0]] == 0 && d[l.tiling_pending[1]] == 0));
  d[l.tiling_flags] = uint8_t((d[l.tiling_flags] & ~0x02) | (query ? 0x02 : 0));
}
// The device's fields the surface setters touch, and the surface's words they read (24-39, the helper's too).
inline bool SurfaceSetterAccepts(const D3dLayout& l, uint32_t device, uint32_t surface) {
  return IsPlainMemory(device, l.device_bytes) &&
         (surface == 0 || (IsPlainMemory(surface, 40) && !Overlap(surface, 40, device, l.device_bytes)));
}
}  // namespace detail

// SetRenderTarget (Crackdown sub_826B1EE0): target `index` (0-3) bound to `surface` (0: none). A surface's word 28
// becomes the target's COLOR_INFO with dirty bit 56 (index 0) or 55 - index; a 2_10_10_10 format (2, 3, 10, 12)
// whose _AS_ bit (19) is not what the target's word says is switched to the other variant in the surface and the
// mirror. Target 0 then has the viewport and scissor set to its surface, or to the depth surface without one:
// `viewport_of(surface)`, the runtime's function (it writes the device), and the predicated-query flag set again.
// Last the target's color write mask (0 with no surface bound) and dirty bit 37. Declined: index past 3, memory not
// plain, the surface over the device.
template <class ViewportOf>
inline bool D3dSetRenderTarget(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t index, uint32_t surface,
                               ViewportOf&& viewport_of) {
  if (index > 3 || !detail::SurfaceSetterAccepts(l, device, surface)) return false;
  uint8_t* d = m.Host(device);
  detail::Set32(d + l.surfaces + 4 * index, surface);
  if (surface) {
    uint8_t* s = m.Host(surface);
    const uint32_t slot = index ? index + 1 : 0;
    const uint64_t dirty = (uint64_t(1) << 63) >> (7 + slot);
    uint8_t* info = d + l.color_info + 4 * slot;
    const uint32_t word = detail::At32(s + 28);
    detail::Set32(info, word);
    detail::Or64(m, device + l.state_dirty, dirty);
    const uint32_t wide = detail::At32(d + l.target_as_wide + 4 * index);
    const uint32_t format = (word >> 16) & 0xF;
    if ((format == 2 || format == 3 || format == 10 || format == 12) && ((word >> 19) & 1) != wide) {
      const uint32_t keep = wide - 1;  // all ones for 0: the narrow format; 0 for 1: the wide one
      const uint32_t other = ((((format >> 1) - 3) & keep) | (((format + 3) << 1) & ~keep)) & 0xF;
      detail::Set32(s + 28, (word & ~0xF0000u) | other << 16);
      detail::Set32(info, (detail::At32(info) & ~0xF0000u) | other << 16);
    }
  }
  if (index == 0) {
    const uint32_t target = surface ? surface : detail::At32(d + l.surfaces + 16);
    if (target) viewport_of(target);
    detail::SetTilingQuery(d, l);
  }
  const uint32_t mask = detail::At32(d + l.surfaces + 4 * index) ? detail::At32(d + l.color_mask_set + 4 * index) : 0;
  const uint32_t shift = 4 * index;
  detail::Set32(d + l.color_mask, (detail::At32(d + l.color_mask) & ~(0xFu << shift)) | (mask & 0xF) << shift);
  detail::Or64(m, device + l.state_dirty, uint64_t(1) << 37);
  return true;
}

// SetDepthStencilSurface (Crackdown sub_826B2218): the depth surface bound (0: none). A surface: with no render
// target 0, the viewport and scissor set to it first (`viewport_of(surface)`, the runtime's function); its word 28
// to DEPTH_INFO, word 32 to 0x2203's word and its bit 0 to the depth flags' bit 6; when tiled (options bit 6 and the
// surfaces tiled) SURFACE_INFO's hi-z pitch (bits 18-31) from the tiling's; dirty bits 55, 8 and 17. None: 0x2203's
// low 4 bits and the depth flags' bit 6 cleared, dirty bit 8. Then the predicated-query flag, DEPTHCONTROL's z and
// stencil enables as the title set them (0 with no surface bound), dirty bits 11 and 17. Declined: memory not plain,
// the surface over the device.
template <class ViewportOf>
inline bool D3dSetDepthStencilSurface(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t surface,
                                      ViewportOf&& viewport_of) {
  if (!detail::SurfaceSetterAccepts(l, device, surface)) return false;
  uint8_t* d = m.Host(device);
  detail::Set32(d + l.surfaces + 16, surface);
  if (surface) {
    if (detail::At32(d + l.surfaces) == 0) viewport_of(surface);
    const uint8_t* s = m.Host(surface);
    const bool tiled = (d[l.tiling_options] & 0x40) != 0;
    detail::Set32(d + l.color_info + 4, detail::At32(s + 28));
    const uint32_t word = detail::At32(s + 32);
    detail::Set32(d + l.depth_word, word);
    d[l.depth_flags] = uint8_t((d[l.depth_flags] & ~0x40) | (word & 1) << 6);
    if (tiled && detail::SurfacesTiled(d, l)) {
      detail::Set32(d + l.surface_info,
                    (detail::At32(d + l.surface_info) & 0x3FFFFu) | detail::At32(d + l.tiling_hiz_pitch) << 18);
    }
    detail::Or64(m, device + l.state_dirty, (uint64_t(1) << 55) | 0x100 | 0x20000);
  } else {
    detail::Set32(d + l.depth_word, detail::At32(d + l.depth_word) & ~0xFu);
    d[l.depth_flags] &= 0xBF;
    detail::Or64(m, device + l.state_dirty, 0x100);
  }
  detail::SetTilingQuery(d, l);
  const bool bound = detail::At32(d + l.surfaces + 16) != 0;
  const uint32_t z = bound ? detail::At32(d + l.depth_enable_set) : 0;
  detail::Set32(d + l.depth_control, (detail::At32(d + l.depth_control) & ~2u) | (z & 1) << 1);
  const uint32_t stencil = detail::At32(d + l.surfaces + 16) != 0 ? detail::At32(d + l.stencil_enable_set) : 0;
  detail::Set32(d + l.depth_control, (detail::At32(d + l.depth_control) & ~1u) | (stencil & 1));
  detail::Or64(m, device + l.state_dirty, 0x800 | 0x20000);
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

// SetSamplerState for a texture stage's minification (`magnification` false) or magnification filter: r4 the stage,
// r5 the filter. The filter goes into the stage's fetch constant (word 3, with the anisotropic bits from the
// runtime's table, indexed by the stage's byte at stage_filter_index, when the filter or the word 4 flag asks for
// it); word 4 keeps the filter's flag and its low two bits are recomputed from word 3 and the stage's byte at
// stage_lod_limit. Sets the stage's fetch dirty bit (bit 31 - stage).
inline bool D3dSetSamplerFilter(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t stage, uint32_t filter,
                                bool magnification) {
  if (stage >= 32 || !IsPlainMemory(device, l.device_bytes)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t table_at = l.sampler_filter_table + 4 * uint32_t(d[l.stage_filter_index + stage]);
  if (!IsPlainMemory(table_at, 4) || detail::Overlap(table_at, 4, device, l.device_bytes)) return false;
  uint8_t* f = d + l.fetch_constants + 24 * stage;
  const uint32_t table = detail::At32(m.Host(table_at));
  const uint32_t v = filter >> 2;
  uint32_t f4 = detail::At32(f + 16);
  const uint32_t flag = (f4 >> (magnification ? 11 : 10)) & 1u;
  f4 = magnification ? (f4 & ~0x400u) | ((v & 1u) << 10) : (f4 & ~0x800u) | ((v & 1u) << 11);
  const uint32_t aniso = table & ~((flag | v) - 1u);
  const uint32_t bits = (magnification ? aniso << 6 : aniso << 4) | v | filter;
  const uint32_t field = magnification ? 0x0E180000u : 0x0E600000u;
  const uint32_t f3 = (detail::At32(f + 12) & ~field) | (std::rotl(bits, magnification ? 19 : 21) & field);
  detail::Set32(f + 12, f3);
  const uint32_t mixed = (f3 & 0x80080000u) | (std::rotr(f3, 1) & 0x7FF7FFFFu);
  const uint32_t limit = d[l.stage_lod_limit + stage];
  const uint32_t mask = (limit >> 2) - 1u;
  const uint32_t low = (((mixed >> 19) & 0xFFFu) & mask) + (limit & ~mask);
  detail::Set32(f + 16, (f4 & 0xFFFFFFFCu) | (low & 3u));
  uint8_t* dirty = d + l.fetch_dirty;
  detail::Store64(dirty, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(dirty)) | (uint64_t(1) << (31 - stage))));
  return true;
}

// SetVertexDeclaration: r4 the declaration; the old one released as the binders do (record word at r1 - 48).
inline bool D3dSetVertexDeclaration(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t declaration,
                                    uint32_t stack_pointer) {
  if (!IsPlainMemory(device, l.device_bytes)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t stack_word = stack_pointer - 48;
  detail::Release release;
  if (!detail::PlanRelease(m, l, d, device, detail::At32(d + l.vertex_declaration), stack_word, release)) return false;
  detail::DoRelease(m, l, d, release, stack_word);
  detail::Set32(d + l.vertex_declaration, declaration);
  return true;
}

// The constant program a shader carries (at `header`: +0 the bits of the float constants it sets itself, cleared
// from the shader's dirty bits; +8 nonzero when it sets bool or integer constants; +16 the program's size; +20 the
// program): entries of a half-word place in the fetch constants (from fetch_constants) and a half-word count.
// First entries of 8 bytes to skip until one of count 0; then copies (count words follow) until one of count 0;
// then masked writes (count / 2 pairs of AND and OR words follow) until one of count 0 or the program's end.
namespace detail {
struct ShaderProgram {
  uint32_t header = 0, program = 0, end = 0, highest = 0;  // highest: the last byte read
  uint32_t write_low = ~0u, write_high = 0;  // the fetch constant bytes written, from fetch_constants
  // The writes the walk found, in order (the run replays them without walking again); more than kOps: walked again.
  struct Op {
    uint32_t source;
    uint16_t place, count;
    bool masked;
  };
  static constexpr uint32_t kOps = 16;
  Op ops[kOps];
  uint32_t op_count = 0;
  bool ops_overflow = false;
  void Note(uint32_t source, uint32_t place, uint32_t count, bool masked) {
    if (op_count < kOps) {
      ops[op_count++] = {source, uint16_t(place), uint16_t(count), masked};
    } else {
      ops_overflow = true;
    }
  }
};
// Walks the program as the runtime does, reading only; false when it would read past `limit` bytes, write outside
// the device, loop for ever (an odd count) or read the device.
inline bool PlanShaderProgram(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t header, ShaderProgram& p) {
  if (!IsPlainMemory(header, 20) || Overlap(header, 20, device, l.device_bytes)) return false;
  const uint32_t size = At32(m.Host(header + 16));
  if (size > 0x10000) return false;
  p.header = header, p.program = header + 20, p.end = header + 20 + size;
  const uint32_t limit = size + 0x40000;  // the masked writes may run past the end, as the runtime reads them
  if (!IsPlainMemory(p.program, limit)) return false;
  const auto half = [&](uint32_t at) { return uint32_t(m.Host(at)[0]) << 8 | m.Host(at)[1]; };
  const auto in = [&](uint32_t at, uint32_t bytes) { return at - p.program + bytes <= limit; };
  uint32_t at = p.program;
  while (at < p.end) {
    if (!in(at, 4)) return false;
    const uint32_t count = half(at + 2);
    at += 4;
    if (count == 0) break;
    at += 4;
  }
  if (at < p.end) {
    for (;;) {
      if (!in(at, 4)) return false;
      const uint32_t place = half(at), words = half(at + 2);
      at += 4;
      if (words == 0) break;
      if (!in(at, 4 * words) || place + 4 * words > l.device_bytes - l.fetch_constants) return false;
      p.write_low = std::min(p.write_low, place), p.write_high = std::max(p.write_high, place + 4 * words);
      p.Note(at, place, words, false);
      at += 4 * words;
      if (at >= p.end) break;
    }
    while (at < p.end) {
      if (!in(at, 4)) return false;
      const uint32_t place = half(at), count = half(at + 2);
      at += 4;
      if (count == 0) break;
      if (count & 1u) return false;
      if (!in(at, 4 * count) || place + 2 * count > l.device_bytes - l.fetch_constants) return false;
      p.write_low = std::min(p.write_low, place), p.write_high = std::max(p.write_high, place + 2 * count);
      p.Note(at, place, count, true);
      at += 4 * count;
    }
  }
  // What the walk read (the entry read last ends at `at`), apart from the device it writes.
  p.highest = std::max(at, p.end);
  return !Overlap(p.program, p.highest - p.program, device, l.device_bytes);
}
inline void RunShaderProgram(const Memory& m, const D3dLayout& l, uint8_t* d, const ShaderProgram& p) {
  const auto half = [&](uint32_t at) { return uint32_t(m.Host(at)[0]) << 8 | m.Host(at)[1]; };
  uint8_t* constants = d + l.fetch_constants;
  if (!p.ops_overflow) {
    for (uint32_t i = 0; i < p.op_count; ++i) {
      const ShaderProgram::Op& op = p.ops[i];
      if (!op.masked) {
        std::memcpy(constants + op.place, m.Host(op.source), 4 * size_t(op.count));
        continue;
      }
      uint8_t* word = constants + op.place;
      const uint8_t* pairs = m.Host(op.source);
      for (uint32_t k = 0; k < op.count / 2u; ++k, word += 4, pairs += 8) {
        Store32(word, (Load32(word) & Load32(pairs)) | Load32(pairs + 4));  // byte order kept: AND and OR are bytewise
      }
    }
    return;
  }
  uint32_t at = p.program;
  while (at < p.end) {
    const uint32_t count = half(at + 2);
    at += 4;
    if (count == 0) break;
    at += 4;
  }
  if (at >= p.end) return;
  for (;;) {
    const uint32_t place = half(at), words = half(at + 2);
    at += 4;
    if (words == 0) break;
    std::memcpy(constants + place, m.Host(at), 4 * words);
    at += 4 * words;
    if (at >= p.end) break;
  }
  while (at < p.end) {
    const uint32_t place = half(at), count = half(at + 2);
    at += 4;
    if (count == 0) return;
    uint8_t* word = constants + place;
    const uint8_t* pairs = m.Host(at);
    for (uint32_t i = 0; i < count / 2; ++i, word += 4, pairs += 8) {
      Store32(word, (Load32(word) & Load32(pairs)) | Load32(pairs + 4));  // byte order kept: AND and OR are bytewise
    }
    at += 4 * count;
  }
}
}  // namespace detail

// What D3dSetShader may write besides the release (verify): false when it declines. `program` is set when the
// shader has one.
inline bool D3dShaderProgramOf(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t shader, bool vertex,
                               detail::ShaderProgram& program, bool& has_program) {
  has_program = false;
  if (!shader) return true;
  const uint32_t block = shader + (vertex ? 872u : 40u);
  if ((vertex && block == 0) || !IsPlainMemory(block + 20, 4) || detail::Overlap(block + 20, 4, device, l.device_bytes))
    return false;
  const uint32_t offset = detail::At32(m.Host(block + 20));
  if (!offset) return true;
  has_program = detail::PlanShaderProgram(m, l, device, block + offset, program);
  return has_program;
}

// SetPixelShader (`vertex` false) / SetVertexShader: r4 the shader or 0. The old one released (record word at
// r1 - 48), the shader kept, the render state's dirty bits (pixel: 0x120000; vertex: 0x80000 when a shader is set,
// and bit 0 of the flags byte cleared), then the shader's constant program (its header at the shader's +40 / +872
// plus the offset there at +20, when nonzero): the float constants' dirty bits it sets itself cleared, the bool and
// integer constants' dirty bit set when it has any, its fetch constant writes done.
inline bool D3dSetShader(const Memory& m, const D3dLayout& l, uint32_t device, uint32_t shader, bool vertex,
                         uint32_t stack_pointer) {
  if (!IsPlainMemory(device, l.device_bytes)) return false;
  uint8_t* d = m.Host(device);
  const uint32_t stack_word = stack_pointer - 48;
  const uint32_t slot = vertex ? l.vertex_shader : l.pixel_shader;
  const uint32_t block = shader + (vertex ? 872u : 40u);
  (void)block;
  detail::ShaderProgram program;
  bool has_program = false;
  if (!D3dShaderProgramOf(m, l, device, shader, vertex, program, has_program)) return false;  // (the runtime's
  // addic. test of the block's address declined there)
  detail::Release release;
  if (!detail::PlanRelease(m, l, d, device, detail::At32(d + slot), stack_word, release)) return false;
  if (has_program && (detail::Overlap(program.header, 24, stack_word, 8) ||
                      (release.kind == detail::Release::kRecord && detail::Overlap(program.header, 24, release.record, 8)) ||
                      (release.kind == detail::Release::kFence && detail::Overlap(program.header, 24, release.old + 8, 4))))
    return false;
  if (has_program) {  // the release writes the program the run reads after it: walked again, not replayed
    const uint32_t from = program.program, bytes = program.highest - program.program;
    if (detail::Overlap(from, bytes, stack_word, 8) ||
        (release.kind == detail::Release::kRecord && detail::Overlap(from, bytes, release.record, 8)) ||
        (release.kind == detail::Release::kFence && detail::Overlap(from, bytes, release.old + 8, 4)))
      program.ops_overflow = true;
  }
  uint8_t* state = d + l.state_dirty;
  const auto or_state = [&](uint64_t bits) { detail::Store64(state, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(state)) | bits)); };
  if (vertex && shader) or_state(0x80000);
  detail::DoRelease(m, l, d, release, stack_word);
  detail::Set32(d + slot, shader);
  if (vertex) d[l.shader_flags] &= 0xFEu;
  else or_state(0x120000);
  if (!has_program) return true;
  const uint8_t* h = m.Host(program.header);
  uint8_t* floats = d + l.float_dirty[vertex ? 0 : 1];
  detail::Store64(floats, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(floats)) & ~detail::ByteSwap64(detail::Load64(h))));
  if (detail::Load64(h + 8)) {
    uint8_t* c = d + l.constant_dirty;
    detail::Store64(c, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(c)) | (uint64_t(1) << 56)));
  }
  detail::RunShaderProgram(m, l, d, program);
  return true;
}

// The runtime's write-back of a CPU range for the GPU (dcbf over its lines, then sync): both are no-ops in recompiled
// code, so the function leaves nothing but its scratch registers and the stack below the caller's. Nothing to do.
inline bool D3dFlushCpuRange() { return true; }

// SetPixelShaderConstantF: D3dSetShaderConstants (d3d.h) at the pixel registers and their dirty bits.
inline bool D3dSetPixelShaderConstants(const Memory& m, const D3dLayout& l, uint64_t device, uint64_t first,
                                       uint32_t source, uint32_t count, uint64_t dirty, uint32_t stack_pointer) {
  return D3dSetShaderConstants(m, device, first, source, count, dirty, stack_pointer, l.float_constant_base[1],
                               l.float_dirty[1]);
}

}  // namespace any360::xdk
