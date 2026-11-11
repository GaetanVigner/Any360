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

// strrchr: r3 is the last place the byte was seen, r9 walks; with r4 zero, r3 itself walks to the terminator.
inline uint64_t Strrchr(const Memory& memory, uint64_t r3, uint32_t r4) {
  const uint64_t high = r3 & 0xFFFFFFFF00000000ull;
  uint32_t r9 = uint32_t(r3);
  uint32_t r5 = *memory.Host(r9);
  if (int32_t(r4) == 0) {
    while (r5 != 0) r5 = *memory.Host(++r9);
    return high | r9;
  }
  uint64_t found = 0;
  if (int32_t(r5) == int32_t(r4)) found = high | r9;
  while (r5 != 0) {
    r5 = *memory.Host(++r9);
    if (int32_t(r4) == int32_t(r5)) found = high | r9;
  }
  return found;
}

inline uint64_t Tolower(uint64_t r3) {
  const uint32_t r11 = uint32_t(r3) - 65;
  return r11 > 25 ? r3 : r3 + 32;
}
inline uint64_t Toupper(uint64_t r3) {
  const uint32_t r11 = uint32_t(r3) - 97;
  return r11 > 25 ? r3 : r3 - 32;
}

// wcsncpy: strncpy's steps on halfwords.
inline void Wcsncpy(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5) {
  uint32_t r11 = r3;
  if (r5 == 0) return;
  for (;;) {
    const uint8_t high = *memory.Host(r4), low = *memory.Host(r4 + 1);
    r4 += 2;
    *memory.Host(r11) = high;
    *memory.Host(r11 + 1) = low;
    r11 += 2;
    if (!(high | low)) break;
    if (--r5 == 0) return;
  }
  for (uint32_t zeros = r5 - 1; zeros != 0; --zeros, r11 += 2) *memory.Host(r11) = *memory.Host(r11 + 1) = 0;
}

// wcscpy: halfwords copied until the zero one has been.
inline void Wcscpy(const Memory& memory, uint32_t r3, uint32_t r4) {
  for (uint32_t r11 = r3;; r11 += 2, r4 += 2) {
    const uint8_t high = *memory.Host(r4), low = *memory.Host(r4 + 1);
    *memory.Host(r11) = high;
    *memory.Host(r11 + 1) = low;
    if (!(high | low)) return;
  }
}

// strncat: r11 to the destination's terminator; then bytes copied until the source's terminator has been, or, the
// count run out, a terminator written.
inline void Strncat(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5) {
  uint32_t r11 = r3;
  while (*memory.Host(r11) != 0) ++r11;
  while (r5 != 0) {
    const uint8_t r10 = *memory.Host(r4++);
    --r5;
    *memory.Host(r11++) = r10;
    if (r10 == 0) return;
  }
  *memory.Host(r11) = 0;
}

// wcscmp: r11 = the first string's halfword less the second's; while it is zero and the second's is not, the next
// two; then -1, 0 or 1 as r11 is negative, zero or positive.
inline uint64_t Wcscmp(const Memory& memory, uint32_t r3, uint32_t r4) {
  const auto half = [&](uint32_t at) { return uint32_t(*memory.Host(at)) << 8 | *memory.Host(at + 1); };
  int64_t r11;
  for (;;) {
    r11 = int64_t(half(r3));
    const int64_t r10 = int64_t(half(r4));
    r11 = r11 - r10;
    if (r11 != 0 || r10 == 0) break;
    r4 += 2;
    r3 += 2;
  }
  if (r11 < 0) return uint64_t(int64_t(-1));
  return r11 > 0 ? 1 : 0;
}

// strlen, the loop of bytes: r3 walks to past the terminator; r3 = the low half of (r3 - r11 - 1).
inline uint64_t Strlen(const Memory& memory, uint32_t r3) {
  const uint32_t r11 = r3;
  uint8_t r10;
  do {
    r10 = *memory.Host(r3);
    ++r3;
  } while (r10 != 0);
  return uint32_t(r3 - r11 - 1);
}

// strcmp, the loop of bytes: r3 = r10 - r9 for each pair of bytes, until the first string ends or r3 is not zero.
inline uint64_t Strcmp(const Memory& memory, uint32_t r3, uint32_t r4) {
  uint32_t r11 = r3;
  for (;;) {
    const int64_t r10 = *memory.Host(r11), r9 = *memory.Host(r4);
    const int64_t difference = r10 - r9;
    if (r10 == 0) return uint64_t(difference);
    ++r11;
    ++r4;
    if (difference != 0) return uint64_t(difference);
  }
}

// mbtowc: 0 with no source or no count; the byte read; for the terminator, a zero halfword stored when there is a
// destination, and 0; otherwise the byte stored as a halfword when there is a destination, and 1.
inline uint64_t Mbtowc(const Memory& memory, uint32_t r3, uint32_t r4, uint32_t r5) {
  if (r4 == 0 || r5 == 0) return 0;
  const uint8_t r11 = *memory.Host(r4);
  if (r11 == 0) {
    if (r3 != 0) *memory.Host(r3) = 0, *memory.Host(r3 + 1) = 0;
    return 0;
  }
  if (r3 != 0) *memory.Host(r3) = 0, *memory.Host(r3 + 1) = r11;
  return 1;
}

// _stricmp: r6 and r5 the next bytes of the second and the first string; r3 = r5 - r6; out at the second's end;
// equal, the next two; otherwise each lowered when it is 'A' to 'Z', r3 = r5 - r6 again, and out unless that is 0.
inline uint64_t Stricmp(const Memory& memory, uint32_t r3, uint32_t r4) {
  uint32_t r9 = r3 - 1;
  r4 = r4 - 1;
  for (;;) {
    int64_t r6 = *memory.Host(++r4), r5 = *memory.Host(++r9);
    int64_t difference = r5 - r6;
    if (r6 == 0) return uint64_t(difference);
    if (difference == 0) continue;
    if (r6 >= 65 && r6 <= 90) r6 |= 32;
    if (r5 >= 65 && r5 <= 90) r5 |= 32;
    difference = r5 - r6;
    if (difference != 0) return uint64_t(difference);
  }
}

// strstr: r3 as it came for an empty needle; 0 for an empty string; from each place of the string, the needle's
// bytes against the string's until one differs or either ends; found when the needle has ended.
inline uint64_t Strstr(const Memory& memory, uint64_t r3, uint32_t r4) {
  if (*memory.Host(r4) == 0) return r3;
  if (*memory.Host(uint32_t(r3)) == 0) return 0;
  for (;;) {
    uint32_t r11 = r4;
    const uint32_t r9 = uint32_t(r3) - r4;
    for (;;) {
      const uint8_t r10 = *memory.Host(r11);
      if (r10 == 0) break;
      if (*memory.Host(r9 + r11) != r10) break;
      ++r11;
      if (*memory.Host(r9 + r11) == 0) break;
    }
    if (*memory.Host(r11) == 0) return r3;
    ++r3;
    if (*memory.Host(uint32_t(r3)) == 0) return 0;
  }
}

// The helpers on a double's bits, each after its instructions: the double stored at r1 + 16, then read back in
// halfwords and words.
namespace doubles {
inline void Store(const Memory& memory, uint32_t at, uint64_t v) {
  for (uint32_t i = 0; i < 8; ++i) *memory.Host(at + i) = uint8_t(v >> (56 - 8 * i));
}
inline uint32_t Half(const Memory& memory, uint32_t at) { return uint32_t(*memory.Host(at)) << 8 | *memory.Host(at + 1); }
inline uint32_t Word(const Memory& memory, uint32_t at) { return Half(memory, at) << 16 | Half(memory, at + 2); }
inline uint64_t Load(const Memory& memory, uint32_t at) { return uint64_t(Word(memory, at)) << 32 | Word(memory, at + 4); }
}  // namespace doubles
// _finite: r11 = (the top halfword & 0x7FF0) - 0x7FF0; r3 = 1 unless that is zero.
inline uint64_t Finite(const Memory& memory, uint32_t r1, uint64_t f1) {
  doubles::Store(memory, r1 + 16, f1);
  const uint32_t r11 = (doubles::Half(memory, r1 + 16) & 0x7FF0) - 32752;
  return r11 == 0 ? 0 : 1;
}
// _isnan: with r11 = the top halfword & 0x7FF8: 0x7FF0 and a bit set in the low 19 of the top word or in the low
// word, 1; otherwise 1 when r11 is 0x7FF8, else 0.
inline uint64_t Isnan(const Memory& memory, uint32_t r1, uint64_t f1) {
  doubles::Store(memory, r1 + 16, f1);
  const uint32_t r11 = doubles::Half(memory, r1 + 16) & 0x7FF8;
  if (r11 == 32752) {
    if ((doubles::Word(memory, r1 + 16) & 0x7FFFF) != 0) return 1;
    if (doubles::Word(memory, r1 + 20) != 0) return 1;
  }
  return r11 == 32760 ? 1 : 0;
}
// _sptype: the top word 0x7FF00000 and the low one 0, 1; 0xFFF00000 and 0, 2; then as _isnan's two cases, 3 for
// 0x7FF8 and 4 for 0x7FF0 with a bit set; 0 otherwise.
inline uint64_t Sptype(const Memory& memory, uint32_t r1, uint64_t f1) {
  doubles::Store(memory, r1 + 16, f1);
  const uint32_t r10 = doubles::Word(memory, r1 + 16), r9 = doubles::Word(memory, r1 + 20);
  if (r10 == 0x7FF00000u) {
    if (r9 == 0) return 1;
  } else if (r10 == 0xFFF00000u && r9 == 0) {
    return 2;
  }
  const uint32_t r11 = doubles::Half(memory, r1 + 16) & 0x7FF8;
  if (r11 == 32760) return 3;
  if (r11 == 32752 && ((r10 & 0x7FFFF) != 0 || r9 != 0)) return 4;
  return 0;
}
// _get_exp: r11 = ((the top halfword >> 4) & 0x7FF) - 1022; r3 = its low 16 bits sign-extended.
inline uint64_t GetExp(const Memory& memory, uint32_t r1, uint64_t f1) {
  doubles::Store(memory, r1 + 16, f1);
  const uint32_t r11 = ((doubles::Half(memory, r1 + 16) >> 4) & 0x7FF) - 1022;
  return uint64_t(int64_t(int16_t(uint16_t(r11))));
}
// _set_exp: the double at r1 + 16 and at r1 - 16; r11 = (r4 + 1022) << 4, or'd with the top halfword & 0x800F, its
// low 16 bits stored over the top of the copy at r1 - 16, which is the double returned.
inline uint64_t SetExp(const Memory& memory, uint32_t r1, uint64_t f1, uint32_t r4) {
  doubles::Store(memory, r1 + 16, f1);
  uint32_t r11 = r4 + 1022;
  doubles::Store(memory, r1 - 16, f1);
  r11 = r11 << 4;
  const uint32_t r10 = doubles::Half(memory, r1 + 16) & 32783;
  r11 |= r10;
  *memory.Host(r1 - 16) = uint8_t(r11 >> 8);
  *memory.Host(r1 - 15) = uint8_t(r11);
  return doubles::Load(memory, r1 - 16);
}
// _add_exp: the double at r1 - 8, r1 + 16 and r1 - 16; r11 = (((the top halfword >> 4) & 0x7FF) + r4) << 4, then
// as _set_exp.
inline uint64_t AddExp(const Memory& memory, uint32_t r1, uint64_t f1, uint32_t r4) {
  doubles::Store(memory, r1 - 8, f1);
  doubles::Store(memory, r1 + 16, f1);
  const uint32_t r10 = doubles::Half(memory, r1 - 8);
  uint32_t r11 = doubles::Half(memory, r1 + 16);
  doubles::Store(memory, r1 - 16, f1);
  r11 = (r11 >> 4) & 0x7FF;
  r11 = (r11 + r4) << 4;
  r11 |= r10 & 32783;
  *memory.Host(r1 - 16) = uint8_t(r11 >> 8);
  *memory.Host(r1 - 15) = uint8_t(r11);
  return doubles::Load(memory, r1 - 16);
}

}  // namespace any360::xdk::reference
