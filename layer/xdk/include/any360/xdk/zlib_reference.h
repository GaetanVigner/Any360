// The reference model of zlib 1.1.4's inflate_fast (any360/xdk/zlib.h has the native one): the function's own
// PowerPC written out step by step, register by register, the plainest way, as the titles' builds have it (the same
// instructions in every copy read so far: db/xdk_seed.json). The unit tests hold the native one
// to it.
//
// r3 bl, r4 bd: the literal/length and distance tables' lookup bits; r5 tl, r6 td: the tables (8-byte entries: the
// operation or extra bits, the bits to drop, a 32-bit base); r7 the inflate_blocks state (bitk +28, bitb +32, window
// +40, end +44, read +48, write +52); r8 the z_stream (next_in +0, avail_in +4, total_in +8, msg +24). Returns r3:
// 0 (Z_OK), 1 (Z_STREAM_END) or -3 (Z_DATA_ERROR, msg set). The build's own constants: the address of its
// inflate_mask table and of its two messages.
//
// Shifts are the PowerPC's (an amount of 32 to 63 gives 0). The copies are its byte loops; one of 2 bytes, which
// the function's loop never ends on (it counts down from 0), is not followed: the model stops there and returns
// kStopped. So it does at an address at or past `limit` (when not 0: the tests' guest memory is smaller than the
// guest's), which corrupted tables reach, and after 1000 table links in one symbol (a table entry that links to
// itself and drops no bits: the function never leaves it). The unit tests never compare such a call.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "any360/xdk/crt.h"

namespace any360::xdk::reference {

struct InflateFastBuild {
  uint32_t masks = 0;         // inflate_mask: 17 words, 0 and (1 << n) - 1
  uint32_t msg_literal = 0;   // "invalid literal/length code"
  uint32_t msg_distance = 0;  // "invalid distance code"
  uint32_t limit = 0;         // the tests' guest memory size; 0: none
};

inline constexpr uint32_t kStopped = 0xDEADDEAD;

inline uint32_t InflateFast(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                            uint32_t r8, const InflateFastBuild& build) {
  bool stopped = false;
  const auto outside = [&](uint32_t a, uint32_t size) {
    if (build.limit && (a >= build.limit || build.limit - a < size)) stopped = true;
    return stopped;
  };
  const auto l8 = [&](uint32_t a) -> uint32_t { return outside(a, 1) ? 0 : *memory.Host(a); };
  const auto l32 = [&](uint32_t a) -> uint32_t {
    if (outside(a, 4)) return 0;
    uint32_t v;
    std::memcpy(&v, memory.Host(a), 4);
    return std::byteswap(v);
  };
  const auto s8 = [&](uint32_t a, uint32_t v) {
    if (!outside(a, 1)) *memory.Host(a) = uint8_t(v);
  };
  const auto s32 = [&](uint32_t a, uint32_t v) {
    if (outside(a, 4)) return;
    v = std::byteswap(v);
    std::memcpy(memory.Host(a), &v, 4);
  };
  const auto slw = [](uint32_t v, uint32_t n) -> uint32_t { return (n & 32) ? 0 : v << (n & 31); };
  const auto srw = [](uint32_t v, uint32_t n) -> uint32_t { return (n & 32) ? 0 : v >> (n & 31); };

  uint32_t r9, r10, r11, r22, r23, r24, r25, r26, r27, r28, r29, r30, r31;
  r11 = l32(r7 + 48);  // read
  r31 = l32(r7 + 52);  // write: q
  r24 = l32(r8 + 0);   // next_in: p
  r25 = l32(r8 + 4);   // avail_in: n
  r29 = l32(r7 + 32);  // bitb: b
  r30 = l32(r7 + 28);  // bitk: k
  r26 = r31 < r11 ? r11 - r31 - 1 : l32(r7 + 44) - r31;  // m
  r27 = build.masks;
  r23 = l32(r27 + (r4 << 2));  // md
  r22 = l32(r27 + (r3 << 2));  // ml
  uint32_t result = 0, message = 0, links = 0;

  for (;;) {
    while (r30 < 20) {  // loc_82A883E8: GRABBITS(20)
      r29 |= slw(l8(r24), r30), r25 -= 1, r24 += 1, r30 += 8;
    }
    r9 = ((r22 & r29) << 3) + r5;
    r4 = l8(r9);
    r11 = l8(r9 + 1);
    if (r4 == 0) {  // a literal
      r10 = l32(r9 + 4);
      r29 = srw(r29, r11);
      r30 -= r11;
      s8(r31, r10);
      r31 += 1, r26 -= 1;
      goto check;
    }
    r10 = srw(r29, r11);  // loc_82A88420
    r11 = r30 - r11;
    if (!(r4 & 16)) {
      for (links = 0;; ) {  // loc_82A88430
        if (r4 & 64) goto literal_exit;
        if (++links > 1000) return kStopped;
        r3 = l32(r9 + 4);
        r4 = l32(r27 + (r4 << 2)) & r10;
        r4 += r3;
        r9 += r4 << 3;
        r4 = l8(r9);
        if (r4 == 0) {  // loc_82A88638
          const uint32_t bits = l8(r9 + 1);
          r9 = l32(r9 + 4);
          r30 = r11 - bits;
          s8(r31, r9);
          r29 = srw(r10, bits);
          r31 += 1, r26 -= 1;
          goto check;
        }
        r3 = l8(r9 + 1);
        r11 -= r3;
        r10 = srw(r10, r3);
        if (r4 & 16) break;
      }
    }
    // loc_82A88474: the length
    r4 &= 15;
    r3 = l32(r9 + 4);
    r11 -= r4;
    r28 = (l32(r27 + (r4 << 2)) & r10) + r3;
    r10 = srw(r10, r4);
    while (r11 < 15) {  // GRABBITS(15)
      r10 |= slw(l8(r24), r11), r25 -= 1, r24 += 1, r11 += 8;
    }
    r9 = ((r23 & r10) << 3) + r6;
    for (links = 0;;) {  // loc_82A884EC
      r3 = l8(r9 + 1);
      r4 = l8(r9);
      r11 -= r3;
      r10 = srw(r10, r3);
      if (r4 & 16) break;
      if (r4 & 64) {  // loc_82A886B4
        message = build.msg_distance;
        goto error_exit;
      }
      if (++links > 1000) return kStopped;
      r3 = l32(r9 + 4);  // loc_82A884C8
      r4 = (l32(r27 + (r4 << 2)) & r10) + r3;
      r9 += r4 << 3;
    }
    r4 &= 15;
    while (r11 < r4) {  // GRABBITS(e)
      r10 |= slw(l8(r24), r11), r25 -= 1, r24 += 1, r11 += 8;
    }
    r30 = r11 - r4;
    r29 = srw(r10, r4);
    r26 -= r28;                                         // m -= c
    r11 = (l32(r27 + (r4 << 2)) & r10) + l32(r9 + 4);  // d
    r10 = l32(r7 + 40);                                // window
    r11 = r31 - r11;                                   // r = q - d
    if (r11 < r10) {
      r9 = l32(r7 + 44);
      r4 = r9 - r10;
      do r11 += r4;
      while (r11 < r10);
      r10 = r9 - r11;  // e: bytes to the window's end
      if (r28 > r10) {
        r9 = r28 - r10;
        do s8(r31, l8(r11)), r11 += 1, r31 += 1;
        while (--r10 && !stopped);
        r11 = l32(r7 + 40);
        do s8(r31, l8(r11)), r11 += 1, r31 += 1;
        while (--r9 && !stopped);
        if (stopped) return kStopped;
        goto check;
      }
    }
    // Two bytes, then the rest (loc_82A885B8 / loc_82A885F8).
    s8(r31, l8(r11)), r11 += 1, r31 += 1;
    s8(r31, l8(r11)), r11 += 1, r31 += 1;
    r9 = r28 - 2;
    if (r9 == 0) return kStopped;
    do s8(r31, l8(r11)), r11 += 1, r31 += 1;
    while (--r9 && !stopped);
  check:  // loc_82A88654
    if (stopped) return kStopped;
    if (r26 < 258 || r25 < 10) break;
  }
  // loc_82A88664: UNGRAB, UPDATE, Z_OK.
  {
    const uint32_t consumed = l32(r8 + 4) - r25, c = (r30 >> 3) < consumed ? (r30 >> 3) : consumed;
    s32(r7 + 32, r29);
    s32(r7 + 28, r30 - (c << 3));
    s32(r8 + 4, c + r25);
    const uint32_t old_next = l32(r8 + 0), total = l32(r8 + 8);
    s32(r8 + 0, r24 - c);
    s32(r8 + 8, total - old_next + (r24 - c));
    s32(r7 + 52, r31);
    return 0;
  }
literal_exit:  // loc_82A886C0
  if (r4 & 32) {
    result = 1;  // the end of the block
  } else {
    message = build.msg_literal;
  error_exit:
    s32(r8 + 24, message);
    result = 0xFFFFFFFDu;
  }
  {  // loc_82A88710, with k in r11 and b in r10
    const uint32_t consumed = l32(r8 + 4) - r25, c = (r11 >> 3) < consumed ? (r11 >> 3) : consumed;
    s32(r7 + 32, r10);
    s32(r7 + 28, r11 - (c << 3));
    s32(r8 + 4, c + r25);
    const uint32_t old_next = l32(r8 + 0), total = l32(r8 + 8);
    s32(r8 + 0, r24 - c);
    s32(r8 + 8, total - old_next + (r24 - c));
    s32(r7 + 52, r31);
  }
  return result;
}

}  // namespace any360::xdk::reference
