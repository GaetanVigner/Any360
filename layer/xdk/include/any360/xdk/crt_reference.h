// What the replaced functions do, written for clarity and not for speed: the models the unit tests compare the
// replacements of crt.h with. They say what reaches memory for the calls a replacement accepts; in a title, the
// any360_xdk_verify setting compares each replacement with the function's real code on every call, which is what
// ties these models to the titles.
#pragma once

#include <cstdint>

#include "crt.h"

namespace any360::xdk::reference {

// A forward copy, one byte at a time.
inline void CopyForward(const Memory& memory, uint32_t dest, uint32_t source, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) *memory.Host(dest + i) = *memory.Host(source + i);
}

// memcpy of the XDK's CRT: its save of the destination's register below the stack pointer (std r3,-8(r1): the 64
// bits of the register, big-endian), then the forward copy to the address in the register's low half.
inline void Memcpy(const Memory& memory, uint64_t dest_register, uint32_t source, uint32_t size,
                   uint32_t stack_pointer) {
  for (uint32_t i = 0; i < 8; ++i) *memory.Host(stack_pointer - 8 + i) = uint8_t(dest_register >> (8 * (7 - i)));
  CopyForward(memory, uint32_t(dest_register), source, size);
}

// memmove of the XDK's CRT, as its code does it: nothing for a destination that is the source; memcpy (above) for a
// destination below the source, the two addresses compared as signed; otherwise the bytes from the last to the
// first.
inline void Memmove(const Memory& memory, uint64_t dest_register, uint32_t source, uint32_t size,
                    uint32_t stack_pointer) {
  const uint32_t dest = uint32_t(dest_register);
  if (dest == source) return;
  if (int32_t(dest) < int32_t(source)) {
    Memcpy(memory, dest_register, source, size, stack_pointer);
    return;
  }
  for (uint32_t i = size; i-- > 0;) *memory.Host(dest + i) = *memory.Host(source + i);
}

// memset: the low byte of `value`, `size` times.
inline void Memset(const Memory& memory, uint32_t dest, uint32_t value, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) *memory.Host(dest + i) = uint8_t(value);
}

// The string functions, each written after its instructions (the registers named as the code names them).

// wcslen: r11 walks the halfwords to past the first zero one; r3 = ((r11 - r3) >> 1) - 1.
inline uint64_t Wcslen(const Memory& memory, uint32_t string) {
  uint32_t r11 = string;
  uint16_t r10;
  do {
    r10 = uint16_t(*memory.Host(r11) << 8 | *memory.Host(r11 + 1));
    r11 += 2;
  } while (r10 != 0);
  return uint64_t(int64_t(int32_t(r11 - string) >> 1) - 1);
}

// strchr: with r4 zero, to the terminator; otherwise to the first byte equal to r4 (0 at the terminator).
inline uint64_t Strchr(const Memory& memory, uint64_t r3, uint32_t r4) {
  uint32_t at = uint32_t(r3);
  uint32_t r6 = *memory.Host(at);
  if (int32_t(r4) == 0) {
    while (r6 != 0) r6 = *memory.Host(++at);
    return (r3 & 0xFFFFFFFF00000000ull) | at;
  }
  for (;;) {
    if (r6 == 0) return 0;
    if (int32_t(r6) == int32_t(r4)) return (r3 & 0xFFFFFFFF00000000ull) | at;
    r6 = *memory.Host(++at);
  }
}

// strncmp: nothing for a count that is not positive; r3 = r8 - r7 for each pair of bytes, until the counter runs
// out, the first string ends or the difference is not zero.
inline uint64_t Strncmp(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5) {
  if (int32_t(r5) <= 0) return 0;
  uint32_t ctr = r5;
  for (;;) {
    const uint64_t r8 = *memory.Host(r3++), r7 = *memory.Host(r4++);
    const uint64_t difference = r8 - r7;
    if (--ctr == 0 || r8 == 0) return difference;
    if (uint32_t(difference) != 0) return difference;
  }
}

// strncpy: bytes copied until the terminator has been or the count runs out, then count - 1 - copied zeros.
inline void Strncpy(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5) {
  uint32_t r11 = r3;
  if (r5 == 0) return;
  for (;;) {
    const uint8_t r10 = *memory.Host(r4++);
    *memory.Host(r11++) = r10;
    if (r10 == 0) break;
    if (--r5 == 0) return;
  }
  for (uint32_t zeros = r5 - 1; zeros != 0; --zeros) *memory.Host(r11++) = 0;
}

}  // namespace any360::xdk::reference
