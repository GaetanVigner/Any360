// The D3D runtime's functions of d3d.h as their PowerPC code does them, instruction by instruction: the models the
// unit tests compare the replacements with. Written for clarity, not speed.
#pragma once

#include <bit>
#include <cstdint>

#include "d3d.h"

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

}  // namespace any360::xdk::reference
