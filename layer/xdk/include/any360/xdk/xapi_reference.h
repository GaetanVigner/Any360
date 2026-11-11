// What the XAPI functions of xapi.h do, step by step as their code does it: the models the unit tests compare the
// replacements with. Written from the instructions of the three titles' builds of the function (the same steps,
// other registers).
#pragma once

#include <algorithm>
#include <cstdint>

#include "crt_reference.h"
#include "xapi.h"

namespace any360::xdk::reference {

// The XDK's vector copy. `byte_step` is the title's global its byte loop subtracts from the count (1).
// Loads and stores are made in the function's order, and the cache lines it zeroes ahead are zeroed when it zeroes
// them: for ranges that overlap this is what the function leaves, which no plain copy gives.
inline void XMemCpy(const Memory& memory, uint32_t dest, uint32_t source, uint32_t size, uint32_t byte_step = 1) {
  // `count` bytes (128 at most) read, then written.
  const auto copy = [&](uint32_t to, uint32_t from, uint32_t count) {
    uint8_t block[128];
    for (uint32_t i = 0; i < count; ++i) block[i] = *memory.Host(from + i);
    for (uint32_t i = 0; i < count; ++i) *memory.Host(to + i) = block[i];
  };
  // dcbz128: the 128-byte cache line holding `address`.
  const auto zero_line = [&](uint32_t address) {
    const uint32_t line = address & ~127u;
    for (uint32_t i = 0; i < 128; ++i) *memory.Host(line + i) = 0;
  };
  uint32_t to = dest, from = source, left = size;
  if (size < 16) {
    // Eight bytes at once from a source aligned for them, then whole words through the CRT's forward copy from a
    // source aligned for words, then the bytes left.
    if (left >= 8 && (from & 7) == 0) {
      copy(to, from, 8);
      to += 8, from += 8, left -= 8;
    }
    if ((from & 3) == 0 && left >= 4) {
      const uint32_t bytes = left & ~3u;
      CopyForward(memory, to, from, bytes);
      to += bytes, from += bytes, left -= bytes;
    }
    while (left) {
      *memory.Host(to++) = *memory.Host(from++);
      left -= byte_step;
    }
    return;
  }
  // Up to the destination's next 16 bytes, then the count of 16-byte blocks from there to its next cache line.
  const uint32_t head = (0u - dest) & 15;
  uint32_t blocks = uint32_t(int32_t(0u - dest) >> 4) & 7;
  if (head) {
    copy(to, from, head);
    to += head, from += head, left -= head;
  }
  blocks = std::min(blocks, left >> 4);
  // The bytes in whole cache lines after those blocks: the first of those lines is zeroed now.
  const uint32_t lines = (left - 16 * blocks) & ~127u;
  if (lines >= 128) zero_line(to + 127);
  for (uint32_t i = 0; i < blocks; ++i) {
    copy(to, from, 16);
    to += 16, from += 16, left -= 16;
  }
  if (from & 15) {
    // A source not aligned to 16 bytes: the whole lines, each read in full before it is written, the next line
    // zeroed first while two lines or more are left.
    for (uint32_t n = lines; n >= 128; n -= 128) {
      if (n >= 256) zero_line(to + 128);
      copy(to, from, 128);
      to += 128, from += 128, left -= 128;
    }
  } else {
    // An aligned source: 128 bytes a turn while that many are left, the next line zeroed first while more than
    // two lines are left; the first 16 bytes are written before the other 112 are read.
    while (left >= 128) {
      if (left > 256) zero_line(to + 128);
      copy(to, from, 16);
      copy(to + 16, from + 16, 112);
      to += 128, from += 128, left -= 128;
    }
  }
  for (; left >= 16; left -= 16) {
    copy(to, from, 16);
    to += 16, from += 16;
  }
  if (left) copy(to, from, left);
}

// GetLastError: r11 = the word at r13 + 336; not zero, 0; otherwise r11 = the word at r13 + 256 and r3 = the word at
// r11 + 352. SetLastError: the same test, then r3 stored at r11 + 352.
inline uint64_t GetLastError(const Memory& memory, uint32_t r13) {
  const auto word = [&](uint32_t at) {
    return uint32_t(*memory.Host(at)) << 24 | uint32_t(*memory.Host(at + 1)) << 16 | uint32_t(*memory.Host(at + 2)) << 8 |
           *memory.Host(at + 3);
  };
  uint32_t r11 = word(r13 + 336);
  if (r11 != 0) return 0;
  r11 = word(r13 + 256);
  return word(r11 + 352);
}
inline void SetLastError(const Memory& memory, uint32_t r13, uint32_t r3) {
  const auto word = [&](uint32_t at) {
    return uint32_t(*memory.Host(at)) << 24 | uint32_t(*memory.Host(at + 1)) << 16 | uint32_t(*memory.Host(at + 2)) << 8 |
           *memory.Host(at + 3);
  };
  uint32_t r11 = word(r13 + 336);
  if (r11 != 0) return;
  r11 = word(r13 + 256);
  for (uint32_t i = 0; i < 4; ++i) *memory.Host(r11 + 352 + i) = uint8_t(r3 >> (24 - 8 * i));
}

}  // namespace any360::xdk::reference
