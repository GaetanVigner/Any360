// The D3D runtime's functions of d3d.h as their PowerPC code does them, instruction by instruction: the models the
// unit tests compare the replacements with. Written for clarity, not speed.
#pragma once

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>

#include "d3d.h"
#include "d3d_state.h"

namespace any360::xdk::reference {

// The upload of the dirty shader constants (Crackdown's sub_826C5EA0; Test Drive Unlimited's sub_8268DEF0 is the
// same steps in another order, its pointer at 40). Only the calls with room for every run: the function that makes
// room is the runtime's own. r3 device, r4 mask, r5 index, r6 constants; gives back r4 and r5.
inline void D3dDirtyConstants(const Memory& memory, uint32_t cursor_offset, uint64_t r3, uint64_t& r4, uint64_t& r5,
                              uint64_t r6) {
  const auto load32 = [&](uint32_t a) { return std::byteswap(detail::Load32(memory.Host(a))); };
  const auto store32 = [&](uint32_t a, uint32_t v) { detail::Store32(memory.Host(a), std::byteswap(v)); };
  const uint64_t r26 = r3;
  uint64_t r31 = r4;
  uint64_t r30 = r6 - 4;
  const uint32_t r25 = 0x80000000u;  // lis r25,-32768
  r4 = load32(uint32_t(r26) + cursor_offset);
  do {
    const uint64_t lz = r31 ? uint64_t(std::countl_zero(r31)) : 64;  // cntlzd r11,r31
    const uint64_t r9 = load32(uint32_t(r26) + cursor_offset + 4);
    const uint64_t r10 = uint32_t(lz << 6) & 0xFFFFFFC0u;  // rlwinm r10,r11,6,0,25
    const uint64_t r11 = uint32_t(lz << 4) & 0xFFFFFFF0u;  // rlwinm r11,r11,4,0,27
    r30 = r10 + r30;
    const uint64_t r27 = r11 + r5;
    r31 = lz >= 64 ? 0 : r31 << lz;  // sld r31,r31,r8
    const uint64_t r28 = ~r31 ? uint64_t(std::countl_zero(~r31)) : 64;
    const uint64_t r29 = uint32_t(r28 << 4) & 0xFFFFFFF0u;
    const uint64_t room = (uint32_t((r29 + 3) << 2) & 0xFFFFFFFCu) + r4;
    if (!(uint32_t(room) < uint32_t(r9))) return;  // the runtime's function: never reached by the tests
    const uint32_t misalign = uint32_t(r4) & 15;  // clrlwi r11,r4,28
    if (misalign == 4) {
      r4 += 4, store32(uint32_t(r4), r25);
    } else if (misalign == 0) {
      r4 += 4, store32(uint32_t(r4), r25);
      r4 += 4, store32(uint32_t(r4), r25);
    } else if (misalign == 12) {
      r4 += 4, store32(uint32_t(r4), r25);
      r4 += 4, store32(uint32_t(r4), r25);
      r4 += 4, store32(uint32_t(r4), r25);
    }
    const uint32_t header = (uint32_t((r29 - 1) << 16) & 0xFFFF0000u) | uint32_t(r27);
    r4 += 4, store32(uint32_t(r4), header);
    r5 = r29 + r27;
    uint64_t count = r28;
    do {  // loc_826C5FA4: four vectors, lvx128 and stvx (addresses rounded down to 16)
      count -= 1;
      uint8_t v[64];
      for (uint32_t k = 0; k < 4; ++k) {
        for (uint32_t b = 0; b < 16; ++b) v[16 * k + b] = *memory.Host(((uint32_t(r30) + 4 + 16 * k) & ~0xFu) + b);
      }
      r30 += 64;
      for (uint32_t k = 0; k < 4; ++k) {
        for (uint32_t b = 0; b < 16; ++b) *memory.Host(((uint32_t(r4) + 4 + 16 * k) & ~0xFu) + b) = v[16 * k + b];
      }
      r4 += 64;
      r31 = r31 << 1;  // rldicr r31,r31,1,62
    } while (uint32_t(count) != 0);
  } while (r31 != 0);
  store32(uint32_t(r26) + cursor_offset, uint32_t(r4));  // stw r4,48(r26)
}

// The upload of 4-byte constants (Crackdown's sub_826C5B00), instruction by instruction, for the calls with room.
inline void D3dDirtyWords(const Memory& memory, uint32_t cursor_offset, uint64_t r3, uint64_t& r4, uint64_t& r5,
                          uint64_t r6) {
  const auto load32 = [&](uint32_t a) { return std::byteswap(detail::Load32(memory.Host(a))); };
  const auto store32 = [&](uint32_t a, uint32_t v) { detail::Store32(memory.Host(a), std::byteswap(v)); };
  const uint64_t r26 = r3;
  uint64_t r31 = r4;
  uint64_t r29 = r6 - 4;
  r4 = load32(uint32_t(r26) + cursor_offset);
  do {
    const uint64_t lz = r31 ? uint64_t(std::countl_zero(r31)) : 64;  // cntlzd r11,r31
    const uint64_t r9 = load32(uint32_t(r26) + cursor_offset + 4);
    const uint64_t r10 = uint32_t(lz << 2) & 0xFFFFFFFCu;  // rlwinm r10,r11,2,0,29
    const uint64_t r28 = lz + r5;
    r29 = r10 + r29;
    r31 = lz >= 64 ? 0 : r31 << lz;
    const uint64_t r30 = ~r31 ? uint64_t(std::countl_zero(~r31)) : 64;
    const uint64_t r27 = uint32_t(r30 << 2) & 0xFFFFFFFCu;
    if (!(uint32_t(r27 + r4) < uint32_t(r9))) return;  // the runtime's function: never reached by the tests
    const uint32_t header = (uint32_t((r30 - 1) << 16) & 0xFFFF0000u) | uint32_t(r28);
    r5 = r30 + r28;
    r4 += 4, store32(uint32_t(r4), header);
    uint64_t r11 = r30;
    do {  // loc_826C5B98
      r29 += 4;
      const uint32_t word = load32(uint32_t(r29));
      r11 -= 1;
      r31 = r31 << 1;
      r4 += 4, store32(uint32_t(r4), word);
    } while (uint32_t(r11) != 0);
  } while (r31 != 0);
  store32(uint32_t(r26) + cursor_offset, uint32_t(r4));
}

// The copy of a title's shader constants into the device (Crackdown's sub_826B2F68), instruction by instruction:
// lvlx loads from the address to its 16-byte block's end into the vector's left, lvrx the bytes before the address
// from its block into the right (nothing when the address is on 16 bytes), vor joins them.
inline void D3dSetShaderConstants(const Memory& memory, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7,
                                  uint32_t r1, uint32_t vector_base = 112, uint32_t dirty_offset = 0) {
  using Vector = uint8_t[16];
  const auto lvlx = [&](uint32_t ea, Vector& v) {
    const uint32_t n = 16 - (ea & 15);
    for (uint32_t i = 0; i < 16; ++i) v[i] = i < n ? *memory.Host(ea + i) : 0;
  };
  const auto lvrx = [&](uint32_t ea, Vector& v) {
    const uint32_t n = ea & 15;  // the bytes of the block before ea
    for (uint32_t i = 0; i < 16; ++i) v[i] = i >= 16 - n ? *memory.Host((ea & ~0xFu) + (i - (16 - n))) : 0;
  };
  const auto stvx = [&](uint32_t ea, const Vector& v) {
    for (uint32_t i = 0; i < 16; ++i) *memory.Host((ea & ~0xFu) + i) = v[i];
  };
  for (int i = 0; i < 8; ++i) *memory.Host(r1 + 48 + i) = uint8_t(r7 >> (8 * (7 - i)));  // std r7,48(r1)
  uint64_t r10 = (uint32_t(r4 + vector_base) << 4) & 0xFFFFFFF0u;
  uint64_t r11 = r5, r9 = r6;
  r10 = r10 + r3;
  if (uint32_t(r6) > 3) {
    uint64_t r8 = ((uint32_t(r6 - 4) >> 2) & 0x3FFFFFFFu) + 1;
    do {
      Vector v0, v13, v12, v11, v10, v9, v8, v7;
      lvlx(uint32_t(r11), v0);
      lvrx(uint32_t(r11 + 16), v13);
      lvlx(uint32_t(r11 + 16), v12);
      for (int i = 0; i < 16; ++i) v0[i] |= v13[i];
      lvrx(uint32_t(r11 + 32), v11);
      lvlx(uint32_t(r11 + 32), v10);
      for (int i = 0; i < 16; ++i) v13[i] = v12[i] | v11[i];
      lvrx(uint32_t(r11 + 48), v9);
      lvlx(uint32_t(r11 + 48), v8);
      lvrx(uint32_t(r11 + 64), v7);
      for (int i = 0; i < 16; ++i) v12[i] = v10[i] | v9[i], v11[i] = v8[i] | v7[i];
      stvx(uint32_t(r10), v0);
      r8 -= 1;
      stvx(uint32_t(r10 + 16), v13);
      r11 += 64;
      r9 -= 4;
      stvx(uint32_t(r10 + 32), v12);
      r10 += 64;
      stvx(uint32_t(r10 - 64 + 48), v11);
    } while (uint32_t(r8) != 0);
  }
  while (uint32_t(r9) != 0) {
    Vector v0, v13;
    r9 -= 1;
    lvlx(uint32_t(r11), v0);
    lvrx(uint32_t(r11 + 16), v13);
    r11 += 16;
    for (int i = 0; i < 16; ++i) v0[i] |= v13[i];
    stvx(uint32_t(r10), v0);
    r10 += 16;
  }
  uint64_t bits = 0;  // ld r11,0(r3); or r11,r11,r7; std r11,0(r3) (8(r3) for the pixel shader's)
  for (int i = 0; i < 8; ++i) bits = bits << 8 | *memory.Host(uint32_t(r3) + dirty_offset + i);
  bits |= r7;
  for (int i = 0; i < 8; ++i) *memory.Host(uint32_t(r3) + dirty_offset + i) = uint8_t(bits >> (8 * (7 - i)));
}

// The state setters of d3d_state.h, instruction by instruction (Crackdown's runtime: sub_826AFA70 and its five
// siblings, sub_826AF950, sub_826AFCE0, sub_826B3178/sub_826B31D8, sub_826B3238/sub_826B3290), on the layout's
// offsets.
namespace state {

inline uint32_t lwz(const Memory& m, uint32_t ea) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = v << 8 | *m.Host(ea + i);
  return v;
}
inline void stw(const Memory& m, uint32_t ea, uint32_t v) {
  for (int i = 0; i < 4; ++i) *m.Host(ea + i) = uint8_t(v >> (8 * (3 - i)));
}
inline uint64_t ld(const Memory& m, uint32_t ea) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | *m.Host(ea + i);
  return v;
}
inline void std_(const Memory& m, uint32_t ea, uint64_t v) {
  for (int i = 0; i < 8; ++i) *m.Host(ea + i) = uint8_t(v >> (8 * (7 - i)));
}
inline uint32_t rotlw(uint32_t v, uint32_t n) { return std::rotl(v, int(n)); }
// rlwinm/rlwimi's mask from mb to me (bit 0 the most significant), wrapping when mb > me.
inline uint32_t mask(uint32_t mb, uint32_t me) {
  const uint32_t from = 0xFFFFFFFFu >> mb, to = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? from & to : from | to;
}
inline uint32_t rlwinm(uint32_t rs, uint32_t sh, uint32_t mb, uint32_t me) { return rotlw(rs, sh) & mask(mb, me); }
inline uint32_t rlwimi(uint32_t ra, uint32_t rs, uint32_t sh, uint32_t mb, uint32_t me) {
  const uint32_t m = mask(mb, me);
  return (rotlw(rs, sh) & m) | (ra & ~m);
}
// The tail the blend setters share: the word into the four render targets, then ori 1024, 4, 2, 1 into the dirty
// bits (stored after each).
inline void TargetsAndDirty(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r11) {
  for (uint32_t t : l.target_blend) stw(m, r3 + t, r11);
  uint64_t d = ld(m, r3 + l.state_dirty);
  for (uint64_t bit : {1024u, 4u, 2u, 1u}) d |= bit, std_(m, r3 + l.state_dirty, d);
}
// clrlwi r11,r11,16; rlwinm r9,r11,4,0,27; andi. r10,r11,4112; or r10,r10,r9; rlwinm r10,r10,12,0,15;
// rlwinm r10,r10,0,12,10; rlwinm r10,r10,0,4,2; or r11,r10,r11
inline uint32_t AlphaFromColor(uint32_t r11) {
  r11 = r11 & 0xFFFFu;
  const uint32_t r9 = rlwinm(r11, 4, 0, 27);
  uint32_t r10 = r11 & 4112u;
  r10 |= r9;
  r10 = rlwinm(r10, 12, 0, 15);
  r10 = rlwinm(r10, 0, 12, 10);
  r10 = rlwinm(r10, 0, 4, 2);
  return r10 | r11;
}

// sub_826AFA70 (color source: rlwimi r10,r4,0,27,31), sub_826AF9E0 (operation: 5,24,26), sub_826AFB00
// (destination: 8,19,23); `sh`, `mb`, `me` its rlwimi.
inline void BlendColorField(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t sh, uint32_t mb,
                            uint32_t me) {
  uint32_t r11 = lwz(m, r3 + l.blend_flags);
  uint32_t r10 = lwz(m, r3 + l.blend_control);
  r10 = rlwimi(r10, uint32_t(r4), sh, mb, me);
  stw(m, r3 + l.blend_control, r10);
  if (!rlwinm(r11, 0, 0, 0)) return;  // beqlr
  const bool separate = rlwinm(r11, 0, 1, 1) != 0;
  r11 = lwz(m, r3 + l.blend_control);
  if (!separate) r11 = AlphaFromColor(r11);
  TargetsAndDirty(m, l, r3, r11);
}
// sub_826AFC00 (alpha source: rlwimi r10,r4,16,11,15), sub_826AFB90 (operation: 21,8,10), sub_826AFC70
// (destination: 24,3,7).
inline void BlendAlphaField(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t sh, uint32_t mb,
                            uint32_t me) {
  const uint32_t r11 = lwz(m, r3 + l.blend_flags);
  uint32_t r10 = lwz(m, r3 + l.blend_control);
  r10 = rlwimi(r10, uint32_t(r4), sh, mb, me);
  stw(m, r3 + l.blend_control, r10);
  if (!rlwinm(r11, 0, 0, 0)) return;
  if (!rlwinm(r11, 0, 1, 1)) return;
  TargetsAndDirty(m, l, r3, r10);
}
// sub_826AF950.
inline void BlendEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  uint32_t r11 = lwz(m, r3 + l.blend_flags);
  r11 = rlwimi(r11, uint32_t(r4), 31, 0, 0);
  stw(m, r3 + l.blend_flags, r11);
  const bool separate = rlwinm(r11, 0, 1, 1) != 0;
  r11 = lwz(m, r3 + l.blend_control);
  if (!separate) r11 = AlphaFromColor(r11);
  if (int32_t(r4) == 0) r11 = (1u << 16) | 1u;  // lis r11,1; ori r11,r11,1
  TargetsAndDirty(m, l, r3, r11);
}
// sub_826AFCE0.
inline void SeparateAlphaBlend(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  uint32_t r11 = lwz(m, r3 + l.blend_flags);
  r11 = rlwimi(r11, uint32_t(r4), 30, 1, 1);
  const uint32_t r10 = rlwinm(r11, 1, 31, 31);
  stw(m, r3 + l.blend_flags, r11);
  r11 = lwz(m, r3 + l.blend_control);
  if (int32_t(r4) == 0) r11 = AlphaFromColor(r11);
  if (r10 == 0) r11 = (1u << 16) | 1u;
  TargetsAndDirty(m, l, r3, r11);
}
// sub_826B3178 (vertex) / sub_826B31D8 (pixel): r4 the first register, r5 the words, r6 the count (not 0).
inline void BoolConstants(const Memory& m, const D3dLayout& l, uint32_t r3, bool pixel, uint64_t r4, uint64_t r5,
                          uint64_t r6) {
  do {
    uint32_t r9 = lwz(m, uint32_t(r5));
    const uint32_t r11 = uint32_t(r4) & 31u;
    r9 &= 1u;
    uint32_t r10 = rlwinm(uint32_t(r4), 27, 5, 31);
    r6 -= 1;
    r10 += l.bool_constants[pixel] / 4;
    r5 += 4;
    r10 = rlwinm(r10, 2, 0, 29);
    r4 += 1;
    r9 = r9 << r11;
    const uint32_t bit = 1u << r11;
    const uint32_t r8 = lwz(m, r10 + r3);
    stw(m, r10 + r3, (r8 & ~bit) | r9);
  } while (uint32_t(r6) != 0);
  std_(m, r3 + l.constant_dirty, ld(m, r3 + l.constant_dirty) | (uint64_t(1) << 56));
}
// sub_826B3238 (vertex) / sub_826B3290 (pixel).
inline void IntConstants(const Memory& m, const D3dLayout& l, uint32_t r3, bool pixel, uint64_t r4, uint64_t r5,
                         uint64_t r6) {
  uint32_t r11 = rlwinm(uint32_t(r4 + l.int_constants[pixel] / 4), 2, 0, 29);
  r11 += r3;
  do {
    uint32_t r10 = *m.Host(uint32_t(r5) + 11);
    r6 -= 1;
    const uint32_t r9 = *m.Host(uint32_t(r5) + 7);
    r10 = rotlw(r10, 8);
    const uint32_t r8 = *m.Host(uint32_t(r5) + 3);
    r5 += 16;
    r10 |= r9;
    r10 = rlwinm(r10, 8, 0, 23);
    r10 |= r8;
    stw(m, r11, r10);
    r11 += 4;
  } while (uint32_t(r6) != 0);
  std_(m, r3 + l.constant_dirty, ld(m, r3 + l.constant_dirty) | (uint64_t(1) << 56));
}

inline uint8_t lbz(const Memory& m, uint32_t ea) { return *m.Host(ea); }
// The old resource's release, as sub_826AF038 and sub_826B1D30 write it (from loc_826AF138 / loc_826B1DA0); `r1` the
// function's own stack pointer (its record word at 80(r1)). The block-full call (sub_826C3440) is not modelled: the
// tests give the list room.
inline void Release(const Memory& m, const D3dLayout& l, uint32_t r31, uint32_t r28, uint32_t r1) {
  if (r28 == 0) return;
  uint32_t r11 = lwz(m, r31 + l.resource_fence);
  if (r11 != 0) {
    stw(m, r28 + 8, r11);
    return;
  }
  r11 = lwz(m, r31 + l.resource_busy_mask);
  const uint32_t r10 = lwz(m, r28 + 0);
  if ((r11 & r10) == 0) return;
  r11 = lwz(m, r31 + l.release_end);
  const uint32_t r3 = lwz(m, r31 + l.release_cursor);
  if (!(r3 < r11)) std::abort();  // bl sub_826C3440
  r11 = lwz(m, r1 + 80);
  r11 = rlwimi(r11, r28, 30, 2, 31);
  r11 = rlwinm(r11, 0, 2, 0);
  stw(m, r1 + 84, 0xFFFFFFFFu);
  stw(m, r1 + 80, r11);
  std_(m, r3, ld(m, r1 + 80));
  stw(m, r31 + l.release_cursor, r3 + 8);
}
// sub_826AF038 (SetTexture): r3 device, r4 stage, r5 texture, r6 dirty bits; r1 the caller's stack pointer.
inline void SetTexture(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t r5, uint64_t r6,
                       uint32_t caller_r1) {
  const uint32_t r1 = caller_r1 - 160;
  const uint32_t r31 = r3;
  const uint32_t r29 = rlwinm(uint32_t(r4 + l.texture_bound / 4), 2, 0, 29);
  const uint32_t r28 = lwz(m, r29 + r31);
  if (r5 != 0) {
    uint32_t r10 = lwz(m, r5 + 32);
    uint32_t r11 = uint32_t(r4 * 24);
    const uint32_t r8 = uint32_t(r31 + r4);
    uint32_t r9 = lwz(m, r5 + 48);
    uint32_t r27 = lwz(m, r5 + 28);
    uint32_t r25 = lwz(m, r5 + 36);
    uint32_t r26 = lwz(m, r5 + 40);
    uint32_t r4b = rlwinm(r10, 12, 20, 31);
    r11 += r31;
    r4b += 512;
    r11 += l.fetch_constants;
    uint32_t r30 = r10 & 0x1FFFFFFFu;
    uint32_t r3b = rlwinm(r4b, 0, 19, 19);
    r10 = lwz(m, r5 + 44);
    uint32_t r7 = rlwinm(r9, 12, 20, 31);
    r3b += r30;
    r7 += 512;
    r30 = lwz(m, r11 + 0);
    r4b = rlwinm(r9, 0, 3, 22);
    r7 = rlwinm(r7, 0, 19, 19);
    stw(m, r11 + 8, r25);
    r27 = rlwimi(r27, r30, 0, 10, 21);
    r25 = lwz(m, r11 + 20);
    r7 += r4b;
    r9 = lbz(m, r8 + l.texture_mip_floor);
    r4b = lwz(m, r11 + 16);
    r30 = rlwinm(r10, 30, 28, 31);
    r10 = rlwimi(r10, r4b, 0, 30, 21);
    r4b = lwz(m, r11 + 12);
    r7 = rlwimi(r7, r25, 0, 23, 31);
    r26 = rlwimi(r26, r4b, 0, 1, 12);
    stw(m, r11 + 0, r27);
    r27 = lwz(m, r11 + 4);
    r3b = rlwimi(r3b, r27, 0, 20, 20);
    stw(m, r11 + 16, r10);
    stw(m, r11 + 20, r7);
    stw(m, r11 + 12, r26);
    stw(m, r11 + 4, r3b);
    if (r30 > r9) r9 = rlwinm(lwz(m, r5 + 44), 30, 28, 31);
    r7 = lwz(m, r5 + 44);
    r10 = rlwimi(r10, r9, 2, 26, 29);
    uint32_t r8b = lbz(m, r8 + l.texture_mip_ceiling);
    r7 = rlwinm(r7, 26, 28, 31);
    stw(m, r11 + 16, r10);
    if (r7 < r8b) r8b = r7;
    r10 = rlwimi(r10, r8b, 6, 22, 25);
    stw(m, r11 + 16, r10);
    std_(m, r31 + l.fetch_dirty, ld(m, r31 + l.fetch_dirty) | r6);
  }
  stw(m, r29 + r31, r5);
  Release(m, l, r31, r28, r1);
}
// sub_826B1D30 (SetStreamSource): r3 device, r4 stream, r5 buffer, r6 offset, r7 stride, r8 dirty bits.
inline void SetStreamSource(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t r5, uint64_t r6,
                            uint64_t r7, uint64_t r8, uint32_t caller_r1) {
  const uint32_t r1 = caller_r1 - 144;
  const uint32_t r30 = r5, r31 = r3;
  const uint64_t r29 = r4, r26 = r7;
  if (r30 != 0) {
    uint32_t r10 = lwz(m, r30 + 24);
    const uint64_t r11 = 17 - r29;
    uint32_t r9 = lwz(m, r30 + 28);
    r10 = uint32_t(r10 + r6);
    r9 = uint32_t(r9 - r6);
    const uint32_t r6b = rlwinm(r10, 12, 20, 31);
    const uint32_t r7b = r10 & 0x1FFFFFFFu;
    r10 = r6b + 512;
    const uint64_t r5b = r11 + 206;
    uint32_t r11b = rlwinm(uint32_t(r11), 3, 0, 28);
    r10 = rlwinm(r10, 0, 19, 19);
    r11b += r31;
    const uint32_t r6c = rlwinm(uint32_t(r5b), 3, 0, 28);
    r10 += r7b;
    stw(m, r11b + (l.vertex_fetch_last + 4 - 17 * 8), r9);  // 1652(r11)
    stw(m, r6c + r31 - (206 + 17) * 8 + l.vertex_fetch_last, r10);
    std_(m, r31 + l.fetch_dirty, ld(m, r31 + l.fetch_dirty) | r8);
  }
  const uint32_t r27 = rlwinm(uint32_t(r29 + l.stream_bound / 4), 2, 0, 29);
  const uint32_t r28 = lwz(m, r27 + r31);
  Release(m, l, r31, r28, r1);
  const uint32_t r11 = rlwinm(uint32_t(r26), 30, 2, 31);
  stw(m, r27 + r31, r30);
  const uint32_t r10 = uint32_t(r31 + r29);
  *m.Host(r10 + l.stream_stride) = uint8_t(r11);
  if (r11 == 0) return;
  if (r11 == lbz(m, r10 + l.shader_stream_stride)) return;
  std_(m, r31 + l.state_dirty, ld(m, r31 + l.state_dirty) | (uint64_t(8) << 16));
}

// sub_826B1180 (minification: rlwinm r7,r9,22,31,31; rlwimi r9,r10,11,20,20; rlwinm r7,r7,4,0,27; rlwimi r31,r10,21,
// 9,10 and 4,6) / sub_826B1308 (magnification: 21; 10,21,21; 6,0,25; 19, 11,12 and 4,6). r3 device, r4 stage,
// r5 filter.
inline void SamplerFilter(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5, bool mag) {
  const uint64_t r7a = (r4 + 32) & 0xFFFFFFFFull;
  const uint64_t r6 = uint64_t(1) << 63;
  const uint32_t r8 = uint32_t(r3 + r4);
  uint32_t r11 = uint32_t(r4 * 24);
  const uint32_t r9t = l.sampler_filter_table;
  r11 += r3;
  uint32_t r10 = rlwinm(uint32_t(r5), 30, 2, 31);
  r11 += l.fetch_constants;
  const uint64_t r6b = (r7a & 127) >= 64 ? 0 : r6 >> (r7a & 127);
  uint32_t r7 = lbz(m, r8 + l.stage_filter_index);
  uint32_t r31 = lwz(m, r11 + 12);
  r7 = rotlw(r7, 2);
  const uint32_t r4b = lwz(m, r7 + r9t);
  uint32_t r9 = lwz(m, r11 + 16);
  r7 = mag ? rlwinm(r9, 21, 31, 31) : rlwinm(r9, 22, 31, 31);
  r9 = mag ? rlwimi(r9, r10, 10, 21, 21) : rlwimi(r9, r10, 11, 20, 20);
  r7 |= r10;
  r7 -= 1;
  r7 = r4b & ~r7;
  stw(m, r11 + 16, r9);
  r7 = mag ? rlwinm(r7, 6, 0, 25) : rlwinm(r7, 4, 0, 27);
  r10 = r7 | r10;
  r10 |= uint32_t(r5);
  if (mag) {
    r31 = rlwimi(r31, r10, 19, 11, 12);
    r31 = rlwimi(r31, r10, 19, 4, 6);
  } else {
    r31 = rlwimi(r31, r10, 21, 9, 10);
    r31 = rlwimi(r31, r10, 21, 4, 6);
  }
  r10 = rotlw(r31, 0);
  r7 = r10;
  stw(m, r11 + 12, r31);
  r10 = rlwimi(r10, r7, 31, 13, 31);
  const uint32_t r8b = lbz(m, r8 + l.stage_lod_limit);
  r10 = rlwimi(r10, r7, 31, 1, 11);
  r7 = rlwinm(r10, 13, 20, 31);
  r10 = rlwinm(r8b, 30, 2, 31);
  r10 -= 1;
  r7 &= r10;
  r10 = r8b & ~r10;
  r10 = r7 + r10;
  r10 = rlwimi(r10, r9, 0, 0, 29);
  stw(m, r11 + 16, r10);
  std_(m, r3 + l.fetch_dirty, ld(m, r3 + l.fetch_dirty) | r6b);
}
// sub_826B1E50 (SetVertexDeclaration).
inline void SetVertexDeclaration(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r4, uint32_t caller_r1) {
  Release(m, l, r3, lwz(m, r3 + l.vertex_declaration), caller_r1 - 128);
  stw(m, r3 + l.vertex_declaration, r4);
}
inline uint32_t lhz(const Memory& m, uint32_t ea) { return uint32_t(*m.Host(ea)) << 8 | *m.Host(ea + 1); }
// sub_826B3458 (SetPixelShader) / sub_826B3848 (SetVertexShader): r3 device, r4 shader.
inline void SetShader(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r4, bool vertex, uint32_t caller_r1) {
  const uint32_t r30 = r3, r29 = r4;
  const uint32_t slot = vertex ? l.vertex_shader : l.pixel_shader;
  if (vertex && r29 != 0) std_(m, r30 + l.state_dirty, ld(m, r30 + l.state_dirty) | (uint64_t(8) << 16));
  Release(m, l, r30, lwz(m, r30 + slot), caller_r1 - 128);
  uint32_t r11;
  if (vertex) {
    const uint32_t flags = lbz(m, r30 + l.shader_flags);
    stw(m, r30 + slot, r29);
    *m.Host(r30 + l.shader_flags) = uint8_t(rlwinm(flags, 0, 0, 30));
    if (r29 == 0) return;
    r11 = r29 + 872;
    if (r11 == 0) return;  // addic. r11,r29,872
  } else {
    stw(m, r30 + slot, r29);
    uint64_t d = ld(m, r30 + l.state_dirty);
    d |= uint64_t(16) << 16;
    std_(m, r30 + l.state_dirty, d);
    d |= uint64_t(2) << 16;
    std_(m, r30 + l.state_dirty, d);
    if (r29 == 0) return;
    r11 = r29 + 40;
  }
  const uint32_t r10 = lwz(m, r11 + 20);
  if (r10 == 0) return;
  r11 = r10 + r11;
  const uint32_t floats = r30 + l.float_dirty[vertex ? 0 : 1];
  std_(m, floats, ld(m, floats) & ~ld(m, r11 + 0));
  if (ld(m, r11 + 8) != 0) std_(m, r30 + l.constant_dirty, ld(m, r30 + l.constant_dirty) | (uint64_t(1) << 56));
  uint32_t r31 = r11 + 20;
  const uint32_t r28 = r30 + l.fetch_constants;
  const uint32_t r29e = lwz(m, r11 + 16) + r31;
  if (r31 >= r29e) return;
  do {  // loc_826B3558
    const uint32_t count = lhz(m, r31 + 2);
    r31 += 4;
    if (count == 0) break;
    r31 += 4;
  } while (r31 < r29e);
  if (r31 >= r29e) return;  // loc_826B3574
  bool masked = false;
  for (;;) {  // loc_826B357C
    const uint32_t words = lhz(m, r31 + 2), place = lhz(m, r31);
    r31 += 4;
    if (words == 0) {
      masked = true;
      break;
    }
    const uint32_t bytes = rlwinm(words, 2, 0, 29);
    for (uint32_t i = 0; i < bytes; ++i) *m.Host(place + r28 + i) = *m.Host(r31 + i);  // bl memcpy
    r31 += bytes;
    if (!(r31 < r29e)) break;
  }
  if (!masked && !(r31 < r29e)) return;
  while (r31 < r29e) {  // loc_826B35B8 / loc_826B3604
    uint32_t count = lhz(m, r31 + 2);
    const uint32_t place = lhz(m, r31);
    r31 += 4;
    if (count == 0) return;
    uint32_t word = place + r28;
    do {
      count += 0x10000;
      uint32_t r9 = lwz(m, word);
      const uint32_t r8 = lwz(m, r31);
      count -= 2;
      const uint32_t r7 = lwz(m, r31 + 4);
      r9 &= r8;
      r31 += 8;
      r9 |= r7;
      stw(m, word, r9);
      word += 4;
      count &= 0xFFFFu;
    } while (count != 0);
  }
}

// SetRenderState_ViewportEnable (Crackdown sub_826B0AA0), as its code reads.
inline void ViewportEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  const uint32_t r11 = uint32_t(r4) != 0 ? 1087u : 1024u;                  // cmplwi r4,0; li r11,1087 / 1024
  stw(m, r3 + l.vte_control, r11);                                          // stw r11,10444(r3)
  const uint32_t lz = uint32_t(std::countl_zero(uint32_t(r4)));             // cntlzw r11,r4
  uint32_t r10 = lwz(m, r3 + l.clip_control);                               // lwz r10,10436(r3)
  const uint32_t bit = rlwinm(lz, 27, 31, 31);                              // rlwinm r11,r11,27,31,31
  r10 = rlwimi(r10, bit, 16, 15, 15);                                       // rlwimi r10,r11,16,15,15
  stw(m, r3 + l.clip_control, r10);                                         // stw r10,10436(r3)
  uint64_t dirty = ld(m, r3 + l.state_dirty);                               // ld r11,16(r3)
  dirty |= 32;                                                              // ori r11,r11,32
  std_(m, r3 + l.state_dirty, dirty);
  dirty |= 128;                                                             // ori r11,r11,128
  std_(m, r3 + l.state_dirty, dirty);
}
// SetRenderState_DepthBias (Crackdown sub_826B0488), as its code reads (f registers as doubles, lfs/stfs).
inline void DepthBias(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t r1) {
  auto lfs = [&](uint32_t ea) { return double(std::bit_cast<float>(lwz(m, ea))); };
  stw(m, r1 + 28, uint32_t(r4));                                            // stw r4,28(r1)
  const double f12 = lfs(r3 + l.poly_offset);                               // lfs f12,10704(r3)
  const double f0 = lfs(l.zero_float);                                      // lfs f0,8924(r11)
  const bool eq6 = f12 == f0;                                               // fcmpu cr6,f12,f0
  const double f13 = lfs(r1 + 28);                                          // lfs f13,28(r1)
  stw(m, r3 + l.poly_offset + 4, std::bit_cast<uint32_t>(float(f13)));      // stfs f13,10708(r3)
  stw(m, r3 + l.poly_offset + 12, std::bit_cast<uint32_t>(float(f13)));     // stfs f13,10716(r3)
  uint32_t r10 = (!eq6 || !(f13 == f0)) ? 1u : 0u;                          // bne / fcmpu f13,f0 / li
  uint32_t r11 = lwz(m, r3 + l.mode_control);                               // lwz r11,10440(r3)
  const double f12b = lfs(r3 + l.poly_offset + 8);                          // lfs f12,10712(r3)
  r11 = rlwimi(r11, r10, 11, 20, 20);                                       // rlwimi r11,r10,11,20,20
  stw(m, r3 + l.mode_control, r11);
  r10 = (!(f12b == f0) || !(f13 == f0)) ? 1u : 0u;
  r11 = rlwimi(r11, r10, 12, 19, 19);                                       // rlwimi r11,r10,12,19,19
  stw(m, r3 + l.mode_control, r11);
  uint64_t c = ld(m, r3 + l.constant_dirty);                                // ld r11,32(r3)
  c |= uint64_t(1) << 44;
  std_(m, r3 + l.constant_dirty, c);
  c |= uint64_t(1) << 42;
  std_(m, r3 + l.constant_dirty, c);
  std_(m, r3 + l.state_dirty, ld(m, r3 + l.state_dirty) | 64);              // ld; ori 64; std
}

// The render state fields' setters and getters (sub_826AF8B8 to sub_826B03C0, sub_826B3A98, sub_826B2698), as their
// code reads; a getter returns its r3.
inline double lfs_(const Memory& m, uint32_t ea) { return double(std::bit_cast<float>(lwz(m, ea))); }
inline void stfs_(const Memory& m, uint32_t ea, double f) { stw(m, ea, std::bit_cast<uint32_t>(float(f))); }
inline void Dirty16(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t bits) {
  std_(m, r3 + l.state_dirty, ld(m, r3 + l.state_dirty) | bits);
}
// sub_826AF8B8: lwz r11,10440(r3); rlwimi r4,r11,0,0,28; stw r4; ld; ori 64; std.
inline void CullMode(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  const uint32_t r11 = lwz(m, r3 + l.mode_control);
  stw(m, r3 + l.mode_control, rlwimi(uint32_t(r4), r11, 0, 0, 28));
  Dirty16(m, l, r3, 64);
}
// sub_826AF918: lwz r11,10428(r3); rlwimi r11,r4,3,28,28; stw; ori 512; std; oris 4; std.
inline void AlphaTestEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  stw(m, r3 + l.color_control, rlwimi(lwz(m, r3 + l.color_control), uint32_t(r4), 3, 28, 28));
  uint64_t r11 = ld(m, r3 + l.state_dirty) | 512;
  std_(m, r3 + l.state_dirty, r11);
  r11 |= 0x40000;
  std_(m, r3 + l.state_dirty, r11);
}
// sub_826AFDE0: lwz r11,10428(r3); rlwimi r4,r11,0,0,28; stw r4; ori 512.
inline void AlphaFunc(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  stw(m, r3 + l.color_control, rlwimi(uint32_t(r4), lwz(m, r3 + l.color_control), 0, 0, 28));
  Dirty16(m, l, r3, 512);
}
// sub_826AFE10: the bytes rotated out (rlwinm 24/16/8, clrldi 56), through the stack as doubles (fcfid, frsp), times
// the float at -3116(0x82070000) (fmuls), stfs into 10340, 10336, 10344, 10348; dirty 15 << 33.
inline void BlendFactor(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  const uint32_t w = uint32_t(r4);
  const uint64_t r10 = rlwinm(w, 24, 24, 31), r11 = rlwinm(w, 16, 24, 31), r8 = rlwinm(w, 8, 24, 31);
  const uint64_t r9 = r4 & 0xFF;
  const double f13 = double(float(double(int64_t(r10))));
  const double f10 = double(float(double(int64_t(r11))));
  const double f0 = lfs_(m, l.blend_byte_scale);
  const double f12 = double(float(double(int64_t(r9))));
  const double f11 = double(float(double(int64_t(r8))));
  stfs_(m, r3 + l.blend_color + 4, double(float(f13 * f0)));
  stfs_(m, r3 + l.blend_color, double(float(f10 * f0)));
  stfs_(m, r3 + l.blend_color + 8, double(float(f12 * f0)));
  stfs_(m, r3 + l.blend_color + 12, double(float(f11 * f0)));
  Dirty16(m, l, r3, uint64_t(15) << 33);
}
// sub_826AFFD8: lwz r11,12176(r3); stw r4,11596(r3); cmplwi r11,0; bne; li r4,0; rlwimi r11,r4,1,30,30 into 10420;
// ori 2048; std; oris 2; std.
inline void ZEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  const uint32_t r11 = lwz(m, r3 + l.surfaces + 16);
  stw(m, r3 + l.depth_enable_set, uint32_t(r4));
  if (r11 == 0) r4 = 0;
  stw(m, r3 + l.depth_control, rlwimi(lwz(m, r3 + l.depth_control), uint32_t(r4), 1, 30, 30));
  uint64_t d = ld(m, r3 + l.state_dirty) | 2048;
  std_(m, r3 + l.state_dirty, d);
  std_(m, r3 + l.state_dirty, d | 0x20000);
}
// sub_826B0018: lwz r11,10420(r3); rlwimi r11,r4,2,29,29; stw; ori 2048.
inline void ZWriteEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  stw(m, r3 + l.depth_control, rlwimi(lwz(m, r3 + l.depth_control), uint32_t(r4), 2, 29, 29));
  Dirty16(m, l, r3, 2048);
}
// sub_826B0358 as recompiled: subfic's carry from the low word (r4 == 0); subfe; rlwinm 0,19,19 into 10292; 10436's
// rlwinm 0,0,25 or r4; ori 128; std; or 1 << 44; std.
inline void ClipPlaneEnable(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  const uint64_t ca = uint32_t(r4) == 0 ? 1 : 0;
  uint64_t r11 = 0 - r4;
  r11 = ~r11 + r11 + ca;
  stw(m, r3 + l.clip_plane_flag, rlwinm(uint32_t(r11), 0, 19, 19));
  stw(m, r3 + l.clip_control, uint32_t((rlwinm(lwz(m, r3 + l.clip_control), 0, 0, 25)) | r4));
  uint64_t d = ld(m, r3 + l.state_dirty) | 128;
  std_(m, r3 + l.state_dirty, d);
  std_(m, r3 + l.state_dirty, d | (uint64_t(1) << 44));
}
// sub_826B03C0: stw r4,28(r1); lfs the factor (-31180(0x82BB0000)); lfs f13,28(r1); fmuls; lfs the 0.0f; stfs into
// 10704 and 10712; fcmpu with 0 (then 10708, 10716) into 10440's bits 11, 12; 1 << 45, 1 << 43 into +32; ori 64.
inline void SlopeScaleDepthBias(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint32_t r1) {
  stw(m, r1 + 28, uint32_t(r4));
  double f0 = lfs_(m, l.slope_scale_factor);
  double f13 = lfs_(m, r1 + 28);
  f0 = double(float(f13 * f0));
  f13 = lfs_(m, l.zero_float);
  stfs_(m, r3 + l.poly_offset, f0);
  stfs_(m, r3 + l.poly_offset + 8, f0);
  uint32_t r10 = 1;
  if (f0 == f13) r10 = lfs_(m, r3 + l.poly_offset + 4) == f13 ? 0u : 1u;
  uint32_t r11 = lwz(m, r3 + l.mode_control);
  r11 = rlwimi(r11, r10, 11, 20, 20);
  stw(m, r3 + l.mode_control, r11);
  r10 = 1;
  if (f0 == f13) r10 = lfs_(m, r3 + l.poly_offset + 12) == f13 ? 0u : 1u;
  r11 = rlwimi(r11, r10, 12, 19, 19);
  stw(m, r3 + l.mode_control, r11);
  uint64_t c = ld(m, r3 + l.constant_dirty) | (uint64_t(1) << 45);
  std_(m, r3 + l.constant_dirty, c);
  std_(m, r3 + l.constant_dirty, c | (uint64_t(1) << 43));
  Dirty16(m, l, r3, 64);
}
// sub_826B3A98: stw r4,11540(r3); oris 8.
inline void SetIndices(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4) {
  stw(m, r3 + l.index_buffer, uint32_t(r4));
  Dirty16(m, l, r3, 0x80000);
}
// sub_826B2698 as recompiled: srd's count from the low byte of r4 + 9 (0 past 63), the plane at (r4 + 634) << 4.
inline void SetClipPlane(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5) {
  const uint64_t r10 = (r4 + 9) & 0xFFFFFFFF;
  const uint64_t r9 = uint64_t(1) << 63;
  const uint32_t r11 = uint32_t(rlwinm(uint32_t(r4 + l.clip_planes / 16), 4, 0, 27) + r3);
  const uint64_t bit = (r10 & 0x40) ? 0 : r9 >> (r10 & 0x7F);
  for (uint32_t k = 0; k < 16; k += 4) stw(m, r11 + k, lwz(m, uint32_t(r5) + k));
  std_(m, r3 + l.constant_dirty, bit | ld(m, r3 + l.constant_dirty));
}
// The getters: lwz and a mask into r3.
inline uint64_t GetCullMode(const Memory& m, const D3dLayout& l, uint32_t r3) { return lwz(m, r3 + l.mode_control) & 7; }
inline uint64_t GetAlphaTestEnable(const Memory& m, const D3dLayout& l, uint32_t r3) {
  return rlwinm(lwz(m, r3 + l.color_control), 29, 31, 31);
}
inline uint64_t GetBlendEnable(const Memory& m, const D3dLayout& l, uint32_t r3) {
  return rlwinm(lwz(m, r3 + l.blend_flags), 1, 31, 31);
}
inline uint64_t GetSrcBlend(const Memory& m, const D3dLayout& l, uint32_t r3) { return lwz(m, r3 + l.blend_control) & 31; }
inline uint64_t GetDestBlend(const Memory& m, const D3dLayout& l, uint32_t r3) {
  return rlwinm(lwz(m, r3 + l.blend_control), 24, 27, 31);
}
inline uint64_t GetAlphaFunc(const Memory& m, const D3dLayout& l, uint32_t r3) { return lwz(m, r3 + l.color_control) & 7; }
inline uint64_t GetZWriteEnable(const Memory& m, const D3dLayout& l, uint32_t r3) {
  return rlwinm(lwz(m, r3 + l.depth_control), 30, 31, 31);
}
// sub_826AFEB0 as recompiled: lfs the four floats, fmadds with the scale (-11164(0x820F0000)) and the rounding term
// (1148(0x82070000)), fctidz (NaN and too large saturating as the recompiled code does), stfiwx's low words through
// r1 - 16, rlwimi 8,0,23 three times.
inline int64_t Fctidz(double f) {
  if (std::isnan(f)) return int64_t(0x8000000000000000ULL);
  if (f > double(INT64_MAX)) return INT64_MAX;
  if (!(f < 9223372036854775808.0) || f < -9223372036854775808.0) return int64_t(0x8000000000000000ULL);
  return int64_t(f);
}
inline uint64_t GetBlendFactor(const Memory& m, const D3dLayout& l, uint32_t r3) {
  const double f12 = lfs_(m, r3 + l.blend_color), f11 = lfs_(m, r3 + l.blend_color + 12);
  const double f10 = lfs_(m, r3 + l.blend_color + 4), f9 = lfs_(m, r3 + l.blend_color + 8);
  const double f0 = lfs_(m, l.blend_float_scale), f13 = lfs_(m, l.blend_rounding);
  const uint32_t red = uint32_t(Fctidz(double(float(std::fma(f12, f0, f13)))));
  const uint32_t green = uint32_t(Fctidz(double(float(std::fma(f10, f0, f13)))));
  const uint32_t alpha = uint32_t(Fctidz(double(float(std::fma(f11, f0, f13)))));
  const uint32_t blue = uint32_t(Fctidz(double(float(std::fma(f9, f0, f13)))));
  uint32_t r11 = red;
  r11 = rlwimi(r11, alpha, 8, 0, 23);
  uint32_t r10 = rlwimi(green, r11, 8, 0, 23);
  return rlwimi(blue, r10, 8, 0, 23);
}
// sub_826B1580: stage word 4 bits 11 or 10 (rlwinm. 0,20,20 / 0,21,21): r9 = r4 + 32 (clrldi 32); the table entry at
// -25040(0x820F0000) + (r5 << 2); rlwimi r8,r10,25,4,6 into word 3; srd into +24. Always stb r5,11644(r3 + r4).
inline void SamplerAnisotropy(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5) {
  const uint32_t r11 = uint32_t(r4 * 24) + r3 + l.fetch_constants;
  const uint32_t r10 = lwz(m, r11 + 16);
  if ((r10 & 0x800) || (r10 & 0x400)) {
    const uint64_t r9 = (r4 + 32) & 0xFFFFFFFF;
    const uint32_t r8 = rlwinm(uint32_t(r5), 2, 0, 29);
    const uint32_t entry = lwz(m, r8 + l.sampler_filter_table);
    stw(m, r11 + 12, rlwimi(lwz(m, r11 + 12), entry, 25, 4, 6));
    const uint64_t bit = (r9 & 127) >= 64 ? 0 : (uint64_t(1) << 63) >> (r9 & 127);
    std_(m, r3 + l.fetch_dirty, bit | ld(m, r3 + l.fetch_dirty));
  }
  *m.Host(uint32_t(r3 + r4) + l.stage_filter_index) = uint8_t(r5);
}
// sub_826B1270 (`bit` 1: rlwinm r9,r5,1,0,30; rlwinm r7,r11,0,31,29) / sub_826B13F8 (`bit` 0: rlwinm r9,r11,0,0,30;
// or r9,r9,r5): the LOD byte's new value r9, word 3's bits mixed (rlwimi 31,13,31 and 31,1,11; rlwinm 13,20,31), r7 =
// (r9 >> 2) - 1, and/andc/add, rlwimi r11,r4,0,0,29 into word 4; stb r31; srd into +24.
inline void SamplerLodFlag(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5, uint32_t bit) {
  const uint32_t r10 = uint32_t(r4 * 24) + r3 + l.fetch_constants;
  const uint32_t r8 = uint32_t(r3 + r4);
  const uint64_t r11s = (r4 + 32) & 0xFFFFFFFF;
  const uint32_t w4 = lwz(m, r10 + 16);
  const uint64_t r6 = (r11s & 127) >= 64 ? 0 : (uint64_t(1) << 63) >> (r11s & 127);
  const uint32_t byte = lbz(m, r8 + l.stage_lod_limit);
  uint32_t r9;
  if (bit == 1) {
    r9 = rlwinm(uint32_t(r5), 1, 0, 30);
    r9 = rlwinm(byte, 0, 31, 29) | r9;
  } else {
    r9 = rlwinm(byte, 0, 0, 30) | uint32_t(r5);
  }
  uint32_t r11 = lwz(m, r10 + 12);
  const uint32_t w3 = r11;
  r11 = rlwimi(r11, w3, 31, 13, 31);
  uint32_t r7 = rlwinm(r9, 30, 2, 31);
  r11 = rlwimi(r11, w3, 31, 1, 11);
  r7 -= 1;
  r11 = rlwinm(r11, 13, 20, 31);
  const uint32_t r31 = r9;
  r11 &= r7;
  r9 &= ~r7;
  r11 += r9;
  r11 = rlwimi(r11, w4, 0, 0, 29);
  stw(m, r10 + 16, r11);
  *m.Host(r8 + l.stage_lod_limit) = uint8_t(r31);
  std_(m, r3 + l.fetch_dirty, r6 | ld(m, r3 + l.fetch_dirty));
}

// The predicated-query flag as sub_826B1EE0 (from loc_826B2054) and sub_826B2218 (from loc_826B237C) set it: r10 1
// or 0 into bit 1 of +10812.
inline uint32_t TiledCheck(const Memory& m, const D3dLayout& l, uint32_t r31, uint32_t r10) {
  if (r10 & 0x10) return 1;                                                 // rlwinm. r11,r10,0,27,27; li r11,1
  if (!(r10 & 0x20)) return 0;                                              // rlwinm. r11,r10,0,26,26
  for (uint32_t k = 0; k < 5; ++k) {                                        // 12160/12448 ... 12176/12464
    const uint32_t r11 = lwz(m, r31 + l.surfaces + 4 * k), r9 = lwz(m, r31 + l.tiled_surfaces + 4 * k);
    if (r9 != r11 && r11 != 0) return 0;                                    // cmplw; beq; cmplwi; bne
  }
  return 1;
}
inline void QueryFlag(const Memory& m, const D3dLayout& l, uint32_t r31) {
  uint32_t r10 = 0;
  if (lwz(m, r31 + l.bin_mask_set) == 0) {                                           // lwz r11,12436(r31); bne
    const uint32_t flags = lbz(m, r31 + l.tiling_flags);                             // lbz r10,10812(r31)
    if (TiledCheck(m, l, r31, flags) && !(flags & 8)) {                        // clrlwi.; rlwinm. r11,r10,0,28,28
      if (lbz(m, r31 + l.tiling_options) & 0x40) r10 = 1;                              // rlwinm. r11,r11,0,25,25; bne
      else if (lbz(m, r31 + l.tiling_pending[0]) == 0 && lbz(m, r31 + l.tiling_pending[1]) == 0) r10 = 1;
    }
  }
  uint32_t r9 = lbz(m, r31 + l.tiling_flags);                                        // lbz r9,10812(r31)
  r9 = rlwimi(r9, r10, 1, 30, 30);                                          // rlwimi r9,r10,1,30,30
  *m.Host(r31 + l.tiling_flags) = uint8_t(r9);                                       // stb r9,10812(r31)
}
// SetRenderTarget (Crackdown sub_826B1EE0), as its code reads; `call(r4)` its bl 0x826af2d8 (r3 the device).
template <class Call>
inline void SetRenderTarget(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5, Call&& call) {
  const uint32_t r31 = r3;                                                  // mr r31,r3
  const uint32_t r6 = uint32_t((uint32_t(r4) + l.surfaces / 4) << 2);                 // addi r11,r4,3040; rlwinm r6,r11,2,0,29
  stw(m, r6 + r31, uint32_t(r5));                                           // stwx r5,r6,r31
  if (uint32_t(r5) != 0) {                                                  // beq cr6,0x826b2028
    uint64_t r9 = uint32_t(r4) == 0 ? 0 : r4 + 1;                           // li r9,0; addi r9,r4,1
    const uint64_t r8 = uint64_t(l.state_dirty / 8) << 16 | 263;                                            // lis r11,2; ori r8,r11,263
    const uint64_t r7 = uint64_t(1) << 63;                                  // li r7,1; rldicr r7,r7,63,63
    uint64_t r11 = r9 + r8;                                                 // add r11,r9,r8
    const uint32_t r9s = uint32_t((r9 + l.color_info / 4) << 2);                        // addi r9,r9,2561; rlwinm
    const uint32_t r10 = uint32_t((uint32_t(r4) + l.target_as_wide / 4) << 2);              // addi r10,r4,2937; rlwinm
    stw(m, r9s + r31, lwz(m, uint32_t(r5) + 28));                           // lwz r3,28(r5); stwx r3,r9,r31
    const uint64_t sh = r11 & 0xFF;                                         // clrldi r9,r11,56
    r11 = uint32_t(int32_t(uint32_t(r11)) >> 16) << 3;                      // srawi r11,r11,16; rlwinm r11,r11,3
    const uint64_t bit = sh & 0x40 ? 0 : r7 >> (sh & 0x3F);                 // srd r9,r7,r9
    std_(m, uint32_t(r11) + r31, bit | ld(m, uint32_t(r11) + r31));         // ldx; or; stdx
    r9 = lwz(m, r10 + r31);                                                 // lwzx r9,r10,r31
    const uint32_t s = lwz(m, r6 + r31);                                    // lwzx r11,r6,r31
    stw(m, r10 + r31, uint32_t(r9));                                        // stwx r9,r10,r31
    if (s != 0) {
      const uint32_t w = lwz(m, s + 28);                                    // lwz r6,28(r11)
      const uint32_t f = rlwinm(w, 16, 28, 31);                             // rlwinm r10,r6,16,28,31
      if ((f == 2 || f == 3 || f == 10 || f == 12) &&
          uint32_t(rlwinm(w, 13, 31, 31) ^ r9) != 0) {                      // rlwinm r10,r6,13,31,31; xor.
        const uint64_t q = uint32_t(r4) == 0 ? 0 : r4 + 1;                  // li r6,0; addi r6,r4,1
        const uint32_t at = uint32_t((q + l.color_info / 4) << 2);                      // addi r3,r6,2561; rlwinm r6,r3,2,0,29
        const uint32_t w2 = lwz(m, s + 28);                                 // lwz r10,28(r11)
        const uint64_t r8b = q + r8;                                        // add r8,r6,r8
        const uint64_t r29 = r8b & 0xFF;                                    // clrldi r29,r8,56
        const uint32_t f2 = rlwinm(w2, 16, 28, 31);                         // rlwinm r7,r10,16,28,31
        const uint32_t r30 = w2 & mask(16, 11);                             // rlwinm r30,r10,0,16,11
        const uint32_t word = uint32_t(int32_t(uint32_t(r8b)) >> 16);      // srawi r10,r8,16
        uint64_t a = rlwinm(f2, 31, 1, 31);                                 // rlwinm r8,r7,31,1,31
        const uint64_t r9m = r9 - 1;                                        // addi r9,r9,-1
        a = (a - 3) & r9m;                                                  // addi r8,r8,-3; and r8,r8,r9
        const uint64_t r3b = r29 & 0x40 ? 0 : r7 >> (r29 & 0x3F);           // srd r3,r3,r29
        uint64_t b = rlwinm(uint32_t(f2 + 3), 1, 0, 30);                    // addi r29,r7,3; rlwinm r7,r29,1,0,30
        b &= ~r9m;                                                          // andc r7,r7,r9
        const uint32_t r9w = word << 3;                                     // rlwinm r9,r10,3,0,28
        const uint32_t r10b = rlwinm(uint32_t(b | a), 16, 12, 15);          // or r10,r7,r8; rlwinm r10,r10,16,12,15
        stw(m, s + 28, r30 | r10b);                                         // or r8,r30,r10; stw r8,28(r11)
        stw(m, at + r31, (lwz(m, at + r31) & mask(16, 11)) | r10b);         // lwzx; rlwinm; or; stwx
        std_(m, r9w + r31, r3b | ld(m, r9w + r31));                         // ldx; or; stdx
      }
    }
  }
  uint32_t r11 = 0, r10 = 0;
  if (uint32_t(r4) == 0) {                                                  // loc_826B2028
    uint32_t a = uint32_t(r5) != 0 ? uint32_t(r5) : lwz(m, r31 + l.surfaces + 16);    // mr r4,r5 / lwz r4,12176(r31)
    if (a != 0) call(a);                                                    // bl 0x826af2d8
    QueryFlag(m, l, r31);
    r11 = lwz(m, r31 + l.color_mask_set);                                              // lwz r11,11580(r31)
    stw(m, r31 + l.color_mask_set, r11);
    if (lwz(m, r31 + l.surfaces) == 0) r11 = 0;                                  // lwz r10,12160; li r11,0
    r10 = rlwimi(lwz(m, r31 + l.color_mask), r11, 0, 28, 31);                      // rlwimi r10,r11,0,28,31
  } else if (uint32_t(r4) <= 3) {                                           // loc_826B2174 ...
    const uint32_t k = uint32_t(r4);
    const bool none = lwz(m, r31 + l.surfaces + 4 * k) == 0;                     // lwz r11,12164(r31); cmplwi
    r11 = lwz(m, r31 + l.color_mask_set + 4 * k);                                      // lwz r11,11584(r31)
    stw(m, r31 + l.color_mask_set + 4 * k, r11);
    if (none) r11 = 0;
    r10 = rlwimi(lwz(m, r31 + l.color_mask), r11, 4 * k, 28 - 4 * k, 31 - 4 * k);  // rlwimi r10,r11,4,24,27 ...
  } else {
    return;                                                                 // loc_826B220C
  }
  stw(m, r31 + l.color_mask, r10);                                                 // stw r10,10332(r31)
  std_(m, r31 + l.state_dirty, ld(m, r31 + l.state_dirty) | (uint64_t(1) << 37));                 // rldicr r12,r12,37,63; or; std
}
// SetDepthStencilSurface (Crackdown sub_826B2218), as its code reads; `call(r4)` its bl 0x826af2d8.
template <class Call>
inline void SetDepthStencilSurface(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, Call&& call) {
  const uint32_t r31 = r3, r30 = uint32_t(r4);                              // mr r30,r4; mr r31,r3
  stw(m, r31 + l.surfaces + 16, r30);                                                 // stw r30,12176(r31)
  uint64_t r11;
  if (r30 != 0) {
    if (lwz(m, r31 + l.surfaces) == 0) call(r30);                                // lwz r11,12160; bne; bl 0x826af2d8
    const uint32_t b = lbz(m, r31 + l.tiling_options);                                 // lbz r10,10815(r31)
    stw(m, r31 + l.color_info + 4, lwz(m, r30 + 28));                                  // lwz r11,28(r30); stw r11,10248(r31)
    stw(m, r31 + l.depth_word, lwz(m, r30 + 32));                                  // lwz r11,32(r30); stw r11,10432(r31)
    const uint32_t w = lwz(m, r30 + 32);                                    // lwz r11,32(r30)
    const uint32_t r10 = rlwimi(lbz(m, r31 + l.depth_flags), w, 6, 25, 25);         // lbz r10,10814; rlwimi r10,r11,6,25,25
    *m.Host(r31 + l.depth_flags) = uint8_t(r10);                                    // stb r10,10814(r31)
    if (b & 0x40) {                                                         // rlwinm. r10,r10,0,25,25; beq
      if (TiledCheck(m, l, r31, lbz(m, r31 + l.tiling_flags))) {                        // lbz r11,10812(r31) ...
        stw(m, r31 + l.surface_info, rlwimi(lwz(m, r31 + l.surface_info), lwz(m, r31 + l.tiling_hiz_pitch), 18, 0, 13));
      }
    }
    r11 = ld(m, r31 + l.state_dirty) | (uint64_t(1) << 55);                            // ld; rldicr r12,r12,55,63; or
    std_(m, r31 + l.state_dirty, r11);
    r11 |= 256;                                                             // ori r11,r11,256
    std_(m, r31 + l.state_dirty, r11);
    r11 |= 0x20000;                                                         // oris r11,r11,2
  } else {                                                                  // loc_826B235C
    const uint32_t r10 = lbz(m, r31 + l.depth_flags);
    stw(m, r31 + l.depth_word, lwz(m, r31 + l.depth_word) & mask(0, 27));                 // rlwinm r11,r11,0,0,27
    *m.Host(r31 + l.depth_flags) = uint8_t(r10 & 191);                              // andi. r10,r10,191; stb
    r11 = ld(m, r31 + l.state_dirty) | 256;                                            // ld r11,16(r31); ori r11,r11,256
  }
  std_(m, r31 + l.state_dirty, r11);                                                   // loc_826B237C: std r11,16(r31)
  QueryFlag(m, l, r31);
  uint32_t z = lwz(m, r31 + l.depth_enable_set);                                         // lwz r11,11596(r31)
  stw(m, r31 + l.depth_enable_set, z);
  if (lwz(m, r31 + l.surfaces + 16) == 0) z = 0;                                      // lwz r10,12176; li r11,0
  stw(m, r31 + l.depth_control, rlwimi(lwz(m, r31 + l.depth_control), z, 1, 30, 30));           // rlwimi r10,r11,1,30,30
  r11 = ld(m, r31 + l.state_dirty) | 2048;                                             // ori r11,r11,2048
  std_(m, r31 + l.state_dirty, r11);
  std_(m, r31 + l.state_dirty, r11 | 0x20000);                                         // oris r11,r11,2
  uint32_t st = lwz(m, r31 + l.stencil_enable_set);                                        // lwz r11,11600(r31)
  const uint32_t bound = lwz(m, r31 + l.surfaces + 16);                               // lwz r10,12176(r31)
  stw(m, r31 + l.stencil_enable_set, st);
  if (bound == 0) st = 0;                                                   // li r11,0
  stw(m, r31 + l.depth_control, rlwimi(st, lwz(m, r31 + l.depth_control), 0, 0, 30));           // rlwimi r11,r10,0,0,30
  r11 = ld(m, r31 + l.state_dirty) | 2048;
  std_(m, r31 + l.state_dirty, r11);
  std_(m, r31 + l.state_dirty, r11 | 0x20000);
}

// The viewport and scissor (Crackdown sub_826AF370, sub_826B1B60, sub_826AF650, sub_826B1C60, sub_826AF2D8), as their
// code reads. The packets function only for the calls with room (its two bl 0x826bb408 not taken).
// fctiwz as the recompiled code does it (its stfiwx: the low word).
inline uint32_t fctiwz_(double f) {
  if (std::isnan(f)) return 0x80000000u;
  if (f >= double(INT32_MAX)) return 0x7FFFFFFFu;
  const double t = std::trunc(f);
  return t < -2147483648.0 ? 0x80000000u : uint32_t(int32_t(t));
}
// The tiling test both functions make (lbz r10,10812; rlwinm. 27,27; rlwinm. 26,26; five cmplw / cmplwi pairs).
inline bool TiledTest(const Memory& m, const D3dLayout& l, uint32_t r31) {
  const uint32_t r10 = lbz(m, r31 + l.tiling_flags);
  if (rlwinm(r10, 0, 27, 27)) return true;
  if (!rlwinm(r10, 0, 26, 26)) return false;
  for (uint32_t k = 0; k < 5; ++k) {
    const uint32_t bound = lwz(m, r31 + l.surfaces + 4 * k), recorded = lwz(m, r31 + l.tiled_surfaces + 4 * k);
    if (recorded != bound && bound != 0) return false;
  }
  return true;
}
inline void WindowScissorPackets(const Memory& m, const D3dLayout& l, uint32_t r3, uint64_t r4, uint64_t r5,
                                 uint64_t r6, uint64_t r7) {
  const uint32_t r31 = r3;
  const uint64_t r24 = r4, r23 = r5, r22 = r6, r21 = r7;
  uint64_t r11 = lwz(m, r31 + l.command_cursor);  // lwz r11,48(r31) (cmplw r11,56(r31): not past it)
  const auto stwu = [&](uint32_t v) { r11 = uint32_t(r11 + 4), stw(m, uint32_t(r11), v); };
  if (!TiledTest(m, l, r31)) {
    uint32_t r10 = (2u << 16) | 8320u;                       // lis r10,2; ori r10,r10,8320
    uint32_t r8 = rlwinm(uint32_t(r23), 16, 2, 15);          // rlwinm r8,r23,16,2,15
    const uint32_t r7_ = uint32_t(r24) & 0x3FFFu;           // clrlwi r7,r24,18
    const uint32_t r6_ = rlwinm(uint32_t(r21), 16, 2, 15);  // rlwinm r6,r21,16,2,15
    stwu(r10);
    r10 = r8 | r7_;
    r8 = uint32_t(r22) & 0x3FFFu;
    r8 = r6_ | r8;
    stwu(0), stwu(r10), stwu(r8);
  } else {
    const uint32_t r25 = 0xC0000000u | 24832u;  // lis r10,-16384; ori r25,r10,24832
    stwu(r25), stwu(0);
    const uint32_t r30 = 0xC0000000u | 24576u;  // ori r30,r9,24576
    if (lwz(m, r31 + l.tile_count) > 0) {
      uint32_t r29 = 0, r28 = r31 + l.tile_rects + 4, r27 = r31 + l.tile_offsets + 4, r26 = 0;
      do {
        uint32_t r10 = lwz(m, r28 - 4);
        uint32_t r7_ = uint32_t(r24);
        uint32_t r6_ = lwz(m, r27 - 4), r5_ = lwz(m, r27);
        if (!(int32_t(r24) > int32_t(r10))) r7_ = r10;  // cmpw r24,r10; bgt
        uint32_t r8 = lwz(m, r28);
        if (int32_t(r23) > int32_t(r8)) r8 = uint32_t(r23);  // cmpw r23,r8; ble
        uint32_t r9 = lwz(m, r28 + 4);
        if (!(int32_t(r22) >= int32_t(r9))) r9 = uint32_t(r22);  // cmpw r22,r9; bge
        r10 = lwz(m, r28 + 8);
        if (!(int32_t(r21) >= int32_t(r10))) r10 = uint32_t(r21);  // cmpw r21,r10; bge
        if (!(int32_t(r7_) < int32_t(r9)) || !(int32_t(r8) < int32_t(r10))) r10 = r9 = r8 = r7_ = 0;
        r7_ &= 0x3FFFu;                          // clrlwi r7,r7,18
        r8 = rlwinm(r8, 16, 2, 15) | r7_;        // rlwinm r8,r8,16,2,15; or r8,r8,r7
        const uint32_t r20 = 0xC0030000u | 11521u, r19 = (4u << 16) | 128u;
        stwu(r30);
        r6_ = (0u - r6_) & 0x7FFFu;              // neg r6,r6; clrlwi r6,r6,17
        r5_ = rlwinm(0u - r5_, 16, 1, 15);       // neg r5,r5; rlwinm r5,r5,16,1,15
        r10 = rlwinm(r10, 16, 2, 15);
        r6_ = r5_ | r6_;
        r9 &= 0x3FFFu;
        const uint32_t shift = r29 & 0x3F;        // slw r7,r3(3),r29
        r7_ = shift >= 32 ? 0 : 3u << shift;
        r10 = r10 | r9;
        stwu(r7_), stwu(r20), stwu(r19), stwu(r6_), stwu(r8), stwu(r10);
        stw(m, r31 + l.command_cursor, uint32_t(r11));  // stw r11,48(r31) (cmplw r11,56(r31): not past it)
        r26 += 1, r27 += 8, r28 += 16, r29 += 2;
      } while (r26 < lwz(m, r31 + l.tile_count));
    }
    if (rlwinm(lbz(m, r31 + l.tiling_options), 0, 25, 25) && rlwinm(lbz(m, r31 + l.tiling_flags), 0, 25, 25)) {
      stwu(r30), stwu(1), stwu(0xC0020000u | 11521u), stwu((4u << 16) | 129u);
      stwu(rlwinm(uint32_t(r23), 16, 2, 15) | (uint32_t(r24) & 0x3FFFu));
      stwu(rlwinm(uint32_t(r21), 16, 2, 15) | (uint32_t(r22) & 0x3FFFu));
    }
    stwu(r30), stwu(lwz(m, r31 + l.tile_bins)), stwu(r25), stwu(lwz(m, r31 + l.tile_bins + 4));
  }
  stw(m, r31 + l.command_cursor, uint32_t(r11));  // stw r11,48(r31)
}
// The scissor: r3 device, r4 the rect; `call(r4, r5, r6, r7)` its bl 0x826af370 (r3 the device).
template <class Call>
inline void ApplyScissor(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r4, Call&& call) {
  const uint32_t x = fctiwz_(lfs_(m, r3 + l.viewport_set)), y = fctiwz_(lfs_(m, r3 + l.viewport_set + 4));
  const uint32_t w = fctiwz_(lfs_(m, r3 + l.viewport_set + 8));
  const uint32_t r11 = lwz(m, r4), r10 = lwz(m, r4 + 4), r8 = lwz(m, r4 + 8), r31 = lwz(m, r4 + 12);
  const uint32_t r7 = lwz(m, r3 + l.scissor_enable);
  const uint32_t h = fctiwz_(lfs_(m, r3 + l.viewport_set + 12));
  uint64_t a4 = x, a5 = y;  // lwz r4,84(r1); lwz r5,88(r1)
  stw(m, r3 + l.scissor_set, r11), stw(m, r3 + l.scissor_set + 4, r10);
  stw(m, r3 + l.scissor_set + 8, r8), stw(m, r3 + l.scissor_set + 12, r31);
  uint64_t a6 = uint64_t(w) + a4;  // add r6,r9,r4
  uint64_t a7 = uint64_t(h) + a5;  // add r7,r9,r5
  if (r7 != 0) {
    if (!(int32_t(a4) > int32_t(r11))) a4 = r11;
    if (!(int32_t(a5) > int32_t(r10))) a5 = r10;
    if (!(int32_t(a6) < int32_t(r8))) a6 = r8;
    if (!(int32_t(a7) < int32_t(r31))) a7 = r31;
  }
  uint32_t w0 = lwz(m, r3 + l.window_scissor), w1 = lwz(m, r3 + l.window_scissor + 4);
  w0 = rlwimi(w0, uint32_t(a5), 16, 1, 15), w1 = rlwimi(w1, uint32_t(a7), 16, 1, 15);
  w0 = rlwimi(w0, uint32_t(a4), 0, 17, 31), w1 = rlwimi(w1, uint32_t(a6), 0, 17, 31);
  stw(m, r3 + l.window_scissor, w0), stw(m, r3 + l.window_scissor + 4, w1);
  call(uint32_t(a4), uint32_t(a5), uint32_t(a6), uint32_t(a7));
}
// The viewport function: r3 device, f1-f6 x, y, width, height, min z, max z.
template <class Call>
inline void SetViewportFloats(const Memory& m, const D3dLayout& l, uint32_t r3, double f1, double f2, double f3,
                              double f4, double f5, double f6, Call&& call) {
  const uint32_t r31 = r3;
  const double f28 = f1, f27 = f2, f31 = f5, f26 = f6;
  const uint32_t r10 = lwz(m, r31 + l.surfaces);
  uint32_t r9 = r10;
  if (r9 == 0) {
    r9 = lwz(m, r31 + l.surfaces + 16);
    if (r9 == 0) return;
  }
  uint64_t r11;
  if (TiledTest(m, l, r31)) {
    r11 = lwz(m, r31 + l.tiled_size), r9 = lwz(m, r31 + l.tiled_size + 4);
  } else {
    const uint32_t word = lwz(m, r9 + 36);
    r9 = rlwinm(word, 29, 17, 31) + 1;
    r11 = rlwinm(word, 14, 18, 31) + 1;
  }
  const uint64_t r7 = fctiwz_(f28), r8 = fctiwz_(f27);
  uint64_t s = uint64_t(fctiwz_(f3)) + r7;  // add r10,r10,r7
  if (int32_t(s) < int32_t(r11)) r11 = s;
  uint64_t t = uint64_t(fctiwz_(f4)) + r8;  // add r10,r10,r8
  if (!(int32_t(t) < int32_t(r9))) t = r9;
  r11 = r11 - r7;  // subf. r11,r7,r11
  t = t - r8;      // subf r10,r8,r10
  if (int32_t(r11) < 0 || int32_t(t) < 0) r11 = 0, t = 0;
  const int64_t width = int32_t(r11), height = int32_t(t);  // extsw
  stfs_(m, r31 + l.viewport_set, f28), stfs_(m, r31 + l.viewport_set + 4, f27);
  stfs_(m, r31 + l.viewport_set + 16, f31), stfs_(m, r31 + l.viewport_set + 20, f26);
  stw(m, r31 + l.viewport_set + 24, 0);
  const double f30 = double(float(double(width))), f29 = double(float(double(height)));  // fcfid; frsp
  stfs_(m, r31 + l.viewport_set + 8, f30), stfs_(m, r31 + l.viewport_set + 12, f29);
  ApplyScissor(m, l, r31, r31 + l.scissor_set, call);
  const double f12 = double(float(f26 - f31));  // fsubs
  stfs_(m, r31 + l.viewport_scale + 20, f31), stfs_(m, r31 + l.viewport_scale + 16, f12);
  double f0 = lfs_(m, l.blend_rounding);
  double f13 = double(float(f30 * f0));
  stfs_(m, r31 + l.viewport_scale, f13);
  f0 = double(float(f29 * f0));
  f13 = double(float(f13 + f28));
  stfs_(m, r31 + l.viewport_scale + 4, f13);
  f13 = std::bit_cast<double>(std::bit_cast<uint64_t>(f0) ^ 0x8000000000000000ull);  // fneg
  stfs_(m, r31 + l.viewport_scale + 8, f13);
  f0 = double(float(f0 + f27));
  stfs_(m, r31 + l.viewport_scale + 12, f0);
  uint64_t dirty = ld(m, r31 + l.state_dirty);
  for (uint64_t bits : {1024u, 512u, 256u, 128u, 64u, 32u}) dirty |= bits << 16, std_(m, r31 + l.state_dirty, dirty);
}
// SetViewport: r3 device, r4 the D3DVIEWPORT9.
template <class Call>
inline void SetViewport(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r4, Call&& call) {
  const auto fcfid_frsp = [](uint64_t v) { return double(float(double(int64_t(v)))); };  // std; lfd; fcfid; frsp
  const uint64_t r11 = lwz(m, r4 + 12), r9 = lwz(m, r4 + 8), r8 = lwz(m, r4 + 4), r7 = lwz(m, r4);
  const double f6 = lfs_(m, r4 + 20), f5 = lfs_(m, r4 + 16);
  SetViewportFloats(m, l, r3, fcfid_frsp(r7), fcfid_frsp(r8), fcfid_frsp(r9), fcfid_frsp(r11), f5, f6, call);
}
// A surface's viewport: r3 device, r4 the surface.
template <class Call>
inline void ViewportOfSurface(const Memory& m, const D3dLayout& l, uint32_t r3, uint32_t r4, Call&& call) {
  const uint32_t r8 = lwz(m, r4 + 24);
  const uint32_t r11 = rlwinm(r8, 16, 30, 31);
  uint32_t r7 = 0;
  if (r11 != 0) r7 = r11 == 1 ? 32769u : 49155u;
  stw(m, r3 + l.sample_control, r7);
  stw(m, r3 + l.surface_info, r8);
  std_(m, r3 + l.state_dirty, ld(m, r3 + l.state_dirty) | (uint64_t(1) << 57));
  std_(m, r3 + l.constant_dirty, ld(m, r3 + l.constant_dirty) | (uint64_t(1) << 36));
  for (uint32_t k = 0; k < 16; k += 4) stw(m, r3 + l.scissor_set + k, lwz(m, l.surface_scissor + k));
  SetViewport(m, l, r3, l.surface_viewport, call);  // b 0x826b1c60
}

}  // namespace state

// D3DResource_AddRef (Crackdown sub_826ACC90) and D3DResource_Release (sub_826ACD08), as their code reads.
namespace refcount {
inline uint32_t lwz(const Memory& m, uint32_t ea) { return detail::At32(m.Host(ea)); }
inline void stw(const Memory& m, uint32_t ea, uint32_t v) { detail::Set32(m.Host(ea), v); }
inline uint64_t AddRef(const Memory& m, uint64_t r3) {
  const uint32_t r11 = uint32_t(r3) + 4;               // addi r11,r3,4
  const uint64_t r10 = uint64_t(lwz(m, r11)) + 1;      // lwarx r10; addi r10,r10,1
  stw(m, r11, uint32_t(r10));                          // stwcx. r10
  const uint64_t r31 = r10;                            // mr r31,r10
  if (uint32_t(r31) == 1) {                            // cmplwi cr6,r31,1
    const uint32_t word = lwz(m, uint32_t(r3));        // lwz r11,0(r3)
    if ((word & 0xF) == 4 && (word & 0x40000000u)) {   // clrlwi r10,r11,28; cmplwi 4; rlwinm. r11,r11,0,1,1
      AddRef(m, lwz(m, uint32_t(r3) + 24));            // lwz r3,24(r3); bl 0x826acc90
    }
  }
  return r31;                                          // mr r3,r31
}
template <class Destroy>
inline uint64_t Release(const Memory& m, uint64_t r3, Destroy& destroy, uint32_t depth = 0) {
  const uint32_t r31 = uint32_t(r3);                   // mr r31,r3
  const uint32_t r11 = r31 + 4;                        // addi r11,r31,4
  const uint64_t r10 = uint64_t(lwz(m, r11)) - 1;      // lwarx r10; addi r10,r10,-1
  stw(m, r11, uint32_t(r10));                          // stwcx. r10
  if (uint32_t(r10) != 0) return r10;                  // mr r3,r10; cmplwi cr6,r3,0; bne
  const uint32_t word = lwz(m, r31);                   // lwz r11,0(r31)
  if ((word & 0xF) == 4 && (word & 0x40000000u)) {     // clrlwi; cmplwi 4; rlwinm.
    Release(m, lwz(m, r31 + 24), destroy, depth + 1);  // lwz r3,24(r31); bl 0x826acd08
  }
  destroy(r31, depth);                                 // mr r3,r31; bl 0x826ac6c8
  return 0;                                            // li r3,0
}
}  // namespace refcount

}  // namespace any360::xdk::reference
