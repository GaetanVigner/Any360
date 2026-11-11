// any360::xdk: native replacements of the functions titles link statically from the XDK's libraries, the C runtime
// first. The same work as the recompiled function, done by the host: portable C++ with no dependency and nothing of
// any one title (a title only binds its address of the function), so each one is unit-tested
// (tests/xdk_crt_tests.cpp, against the reference models of crt_reference.h) and benchmarked
// (tests/xdk_crt_bench.cpp) here, outside the titles. docs/NATIVE_FUNCTIONS.md is the rule they follow.
//
// A replacement handles the common case and declines the rest (returns false): the caller then runs the function's
// own code. Nothing has been written when it declines.
#pragma once

#include <cstdint>
#include <cstring>

namespace any360::xdk {

// Guest memory as a recompiled title maps it: guest address 0 at `base`; from 0xE0000000 (the physical view) the
// host address is `physical_offset` further on the hosts whose runtime maps that view at an offset.
struct Memory {
  uint8_t* base = nullptr;
  uint32_t physical_offset = 0;
  uint8_t* Host(uint32_t address) const { return base + address + (address >= 0xE0000000u ? physical_offset : 0u); }
};

// [address, address + size) is plain memory under one host pointer: it does not wrap, does not cross into the
// physical view at 0xE0000000, and holds nothing of the GPU's register window (0x7F000000-0x7FFFFFFF), which plain
// stores would bypass.
inline bool IsPlainMemory(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000ull) &&
         (end <= 0x7F000000ull || address >= 0x80000000u);
}

// A forward copy gives what memmove gives unless the destination starts inside the source, where it repeats the
// bytes it has just copied.
inline bool IsPlainForwardCopy(uint32_t dest, uint32_t source, uint32_t size) {
  return dest <= source || dest - source >= size;
}

namespace detail {

inline uint64_t Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline void Store64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline void Store32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// 32 bytes at once (two 16-byte moves once compiled).
struct Bytes32 {
  uint64_t a, b, c, d;
};
inline Bytes32 Load256(const uint8_t* p) {
  Bytes32 v;
  std::memcpy(&v, p, 32);
  return v;
}
inline void Store256(uint8_t* p, const Bytes32& v) { std::memcpy(p, &v, 32); }

// memmove for the sizes the titles copy most (up to 128 bytes: the two ends of the range, overlapping in the
// middle, every load before any store, so overlap is as memmove's), the host's memmove above. What the benchmark
// kept (xdk_crt_bench, memcpy on the three titles' mixes of sizes): memcpy is 1.2x its first version over the
// three. Most of that is the one test of plain memory (ArePlain: 1.13x with the host's memmove alone behind it);
// these paths add the rest on the title a third of whose copies are 65 to 128 bytes (1.3x against 1.1x) and
// nothing on the two others. On the first title measured alone the part above 64 bytes had been taken out: one
// title's sizes are not the titles' sizes. One path without a branch on the size for 16 to 64 bytes was slower
// and stays out.
inline void Move(uint8_t* d, const uint8_t* s, uint32_t n) {
  if (n <= 32) {
    if (n >= 16) {
      const uint64_t a = Load64(s), b = Load64(s + 8), c = Load64(s + n - 16), e = Load64(s + n - 8);
      Store64(d, a);
      Store64(d + 8, b);
      Store64(d + n - 16, c);
      Store64(d + n - 8, e);
    } else if (n >= 8) {
      const uint64_t a = Load64(s), b = Load64(s + n - 8);
      Store64(d, a);
      Store64(d + n - 8, b);
    } else if (n >= 4) {
      const uint32_t a = Load32(s), b = Load32(s + n - 4);
      Store32(d, a);
      Store32(d + n - 4, b);
    } else if (n) {
      const uint8_t a = s[0], b = s[n >> 1], c = s[n - 1];
      d[0] = a;
      d[n >> 1] = b;
      d[n - 1] = c;
    }
    return;
  }
  if (n <= 64) {
    const Bytes32 a = Load256(s), b = Load256(s + n - 32);
    Store256(d, a);
    Store256(d + n - 32, b);
    return;
  }
  if (n <= 128) {
    const Bytes32 a = Load256(s), b = Load256(s + 32), c = Load256(s + n - 64), e = Load256(s + n - 32);
    Store256(d, a);
    Store256(d + 32, b);
    Store256(d + n - 64, c);
    Store256(d + n - 32, e);
    return;
  }
  std::memmove(d, s, n);
}

// memset, the sizes up to 128 bytes as overlapping stores of the repeated byte (1.35x the plain memset over the
// three titles' mixes of sizes, between 1.2x and 1.5x on each), the host's memset above.
inline void Fill(uint8_t* d, uint8_t value, uint32_t n) {
  const uint64_t pattern = 0x0101010101010101ull * value;
  if (n <= 32) {
    if (n >= 16) {
      Store64(d, pattern);
      Store64(d + 8, pattern);
      Store64(d + n - 16, pattern);
      Store64(d + n - 8, pattern);
    } else if (n >= 8) {
      Store64(d, pattern);
      Store64(d + n - 8, pattern);
    } else if (n >= 4) {
      Store32(d, uint32_t(pattern));
      Store32(d + n - 4, uint32_t(pattern));
    } else if (n) {
      d[0] = value;
      d[n >> 1] = value;
      d[n - 1] = value;
    }
    return;
  }
  if (n <= 128) {
    const Bytes32 block{pattern, pattern, pattern, pattern};
    Store256(d, block);
    Store256(d + n - 32, block);
    if (n > 64) {
      Store256(d + 32, block);
      Store256(d + n - 64, block);
    }
    return;
  }
  std::memset(d, value, n);
}

inline uint64_t ByteSwap64(uint64_t v) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap64(v);
#else
  return (v >> 56) | ((v >> 40) & 0xFF00ull) | ((v >> 24) & 0xFF0000ull) | ((v >> 8) & 0xFF000000ull) |
         ((v << 8) & 0xFF00000000ull) | ((v << 24) & 0xFF0000000000ull) | ((v << 40) & 0xFF000000000000ull) | (v << 56);
#endif
}

// Both ranges of a copy are plain memory: one test for the usual case (everything below the GPU's registers).
inline bool ArePlain(uint32_t dest, uint32_t source, uint32_t size) {
  if (((uint64_t(dest > source ? dest : source)) + size) <= 0x7F000000ull) return true;
  return IsPlainMemory(dest, size) && IsPlainMemory(source, size);
}

}  // namespace detail

// The calls each function takes, as one test: what the function declines is exactly what its test refuses, and a
// host that runs both the function and the title's own code (to compare or to time them) asks the same test.
inline bool CrtMemcpyAccepts(uint32_t dest, uint32_t source, uint32_t size) {
  return IsPlainForwardCopy(dest, source, size) && detail::ArePlain(dest, source, size);
}
inline bool CrtMemsetAccepts(uint32_t dest, uint32_t size) { return IsPlainMemory(dest, size); }
inline bool CrtMemmoveAccepts(uint32_t dest, uint32_t source, uint32_t size) {
  if (dest == source) return true;
  if (int32_t(dest) < int32_t(source)) return CrtMemcpyAccepts(dest, source, size);
  // The backward copy is memmove's unless the destination is below an overlapping source, where the signed compare
  // can only send it across 0x80000000 (not plain memory anyway).
  return detail::ArePlain(dest, source, size) && !(dest < source && source - dest < size);
}

// memcpy as the XDK's CRT builds it for long copies: copies `size` bytes forward from `source` to the destination
// and leaves the destination's register, all 64 bits of it as a big-endian value, in the 8 bytes below the stack
// pointer (its own save of r3: std r3,-8(r1)). `dest_register` is that register: its low half is the address, its
// high half whatever the caller's arithmetic left there (not always zero: a title's calls showed it).
// Declines a copy whose destination starts inside its source and ranges that are not plain memory.
inline bool CrtMemcpy(const Memory& memory, uint64_t dest_register, uint32_t source, uint32_t size,
                      uint32_t stack_pointer) {
  const uint32_t dest = uint32_t(dest_register);
  if (!CrtMemcpyAccepts(dest, source, size)) return false;
  detail::Store64(memory.Host(stack_pointer - 8), detail::ByteSwap64(dest_register));
  detail::Move(memory.Host(dest), memory.Host(source), size);
  return true;
}

// The CRT's plain forward copy (bytes to alignment, then words). Declines as CrtMemcpy.
inline bool CrtCopyForward(const Memory& memory, uint32_t dest, uint32_t source, uint32_t size) {
  if (!CrtMemcpyAccepts(dest, source, size)) return false;
  detail::Move(memory.Host(dest), memory.Host(source), size);
  return true;
}

// memset: `size` bytes of the low byte of `value` from `dest`. Declines ranges that are not plain memory.
inline bool CrtMemset(const Memory& memory, uint32_t dest, uint32_t value, uint32_t size) {
  if (!CrtMemsetAccepts(dest, size)) return false;
  detail::Fill(memory.Host(dest), uint8_t(value), size);
  return true;
}

// memmove of the XDK's CRT (the same instructions in the three titles read): nothing when the destination is the
// source; when the destination is below the source (a signed compare of the two addresses), memcpy itself, which it
// jumps to (so memcpy's save of the destination's register below the stack pointer is made); otherwise a copy from
// the last byte back to the first, with nothing left on the stack. Declines what memcpy declines on memcpy's side
// and ranges that are not plain memory on the other.
inline bool CrtMemmove(const Memory& memory, uint64_t dest_register, uint32_t source, uint32_t size,
                       uint32_t stack_pointer) {
  const uint32_t dest = uint32_t(dest_register);
  if (!CrtMemmoveAccepts(dest, source, size)) return false;
  if (dest == source) return true;
  if (int32_t(dest) < int32_t(source)) return CrtMemcpy(memory, dest_register, source, size, stack_pointer);
  detail::Move(memory.Host(dest), memory.Host(source), size);
  return true;
}

// The C runtime's string functions (the same instructions in the three titles read). They take guest memory an
// element at a time in their code's own order, so ranges that overlap come out as the code leaves them. Those that
// only read give the value the function leaves in r3, all 64 bits of it, and decline (nothing is written, by
// nature) a string that reaches the GPU's register window.
inline bool IsRegisterWindow(uint32_t address) { return address - 0x7F000000u < 0x01000000u; }
// The bytes from `address` on that are plain memory under one host pointer: up to the register window, to the
// physical view at 0xE0000000 or to the end of the address space; none inside the window. The functions that read
// a string a byte at a time ask once and read that far without a test at every byte (xdk_crt_bench: with the test
// they were 1.2 to 1.3 times slower than their models); a string that has not ended there is left to the
// function's own code.
inline uint32_t PlainSpan(uint32_t address) {
  if (address < 0x7F000000u) return 0x7F000000u - address;
  if (address < 0x80000000u) return 0;
  if (address < 0xE0000000u) return 0xE0000000u - address;
  return uint32_t(0x100000000ull - address);
}

// wcslen: the 16-bit elements before the first zero one.
inline bool CrtWcslen(const Memory& memory, uint32_t string, uint64_t& r3) {
  uint32_t at = string;
  for (;; at += 2) {
    if (IsRegisterWindow(at)) return false;
    const uint8_t* p = memory.Host(at);
    if (!(p[0] | p[1])) break;
  }
  // Its code counts the bytes to past the zero, halves them as a signed 32-bit value and takes one off.
  r3 = uint64_t(int64_t(int32_t(at + 2 - string) >> 1) - 1);
  return true;
}

// strchr: the address of the first byte equal to `character` (the 32 bits of r4, compared with the byte as a whole:
// a value above 255 is never found), of the terminator when `character` is 0, or 0. `string_register` is r3 whole:
// the address found keeps its high half.
inline bool CrtStrchr(const Memory& memory, uint64_t string_register, uint32_t character, uint64_t& r3) {
  const uint32_t string = uint32_t(string_register), span = PlainSpan(string);
  if (!span) return false;
  const uint8_t* bytes = memory.Host(string);
  for (uint32_t i = 0; i < span; ++i) {
    const uint8_t byte = bytes[i];
    if (character ? byte == character : byte == 0) {
      r3 = (string_register & 0xFFFFFFFF00000000ull) | (string + i);
      return true;
    }
    if (!byte) {
      r3 = 0;
      return true;
    }
  }
  return false;
}

// strncmp: the difference of the first two bytes that differ within `count` bytes (a signed 32-bit count: none
// when it is not positive), 0 when there is none or the strings end together first.
inline bool CrtStrncmp(const Memory& memory, uint32_t left, uint32_t right, uint32_t count, uint64_t& r3) {
  r3 = 0;
  if (int32_t(count) <= 0) return true;
  const uint32_t left_span = PlainSpan(left), right_span = PlainSpan(right);
  const uint32_t span = left_span < right_span ? left_span : right_span;
  if (!span) return false;
  const uint8_t* a = memory.Host(left);
  const uint8_t* b = memory.Host(right);
  for (uint32_t i = 0; i < span; ++i) {
    r3 = uint64_t(int64_t(a[i]) - int64_t(b[i]));
    if (i + 1 == count || !a[i] || a[i] != b[i]) return true;
  }
  return false;
}

// strncpy: the source's bytes up to its terminator, `count` at most, then zeros up to `count` bytes; a byte at a
// time from the first, as its code does. Declines ranges of `count` bytes that are not plain memory.
inline bool CrtStrncpyAccepts(uint32_t dest, uint32_t source, uint32_t count) {
  return IsPlainMemory(dest, count) && IsPlainMemory(source, count);
}
inline bool CrtStrncpy(const Memory& memory, uint32_t dest, uint32_t source, uint32_t count) {
  if (!CrtStrncpyAccepts(dest, source, count)) return false;
  uint8_t* to = memory.Host(dest);
  const uint8_t* from = memory.Host(source);
  uint32_t i = 0;
  while (i < count) {
    const uint8_t byte = from[i];
    to[i++] = byte;
    if (!byte) break;
  }
  for (; i < count; ++i) to[i] = 0;
  return true;
}

// strrchr: the address of the last byte equal to `character` before the terminator (of the terminator when
// `character` is 0), or 0; the 32 bits of r4 compared with the byte as a whole, the register's high half kept, as
// strchr.
inline bool CrtStrrchr(const Memory& memory, uint64_t string_register, uint32_t character, uint64_t& r3) {
  const uint64_t high = string_register & 0xFFFFFFFF00000000ull;
  const uint32_t string = uint32_t(string_register), span = PlainSpan(string);
  if (!span) return false;
  const uint8_t* bytes = memory.Host(string);
  r3 = 0;
  for (uint32_t i = 0; i < span; ++i) {
    const uint8_t byte = bytes[i];
    if (character ? byte == character : byte == 0) r3 = high | (string + i);
    if (!byte) return true;
  }
  return false;
}

// tolower and toupper of the "C" locale, on the whole register: 32 added to (taken from) r3 when its low half is
// an upper-case (lower-case) ASCII letter.
inline uint64_t CrtTolower(uint64_t r3) { return uint32_t(r3) - 65u <= 25u ? r3 + 32 : r3; }
inline uint64_t CrtToupper(uint64_t r3) { return uint32_t(r3) - 97u <= 25u ? r3 - 32 : r3; }

// The bytes a function is about to write, known before it writes (a host that compares the function with the
// title's own code keeps them).
struct Range {
  uint32_t at = 0, size = 0;
};
inline bool AreApart(uint32_t a, uint32_t b, uint32_t size) { return a >= b ? a - b >= size : b - a >= size; }

// wcsncpy: strncpy on 16-bit elements (`count` of them). Declines ranges that are not plain memory.
inline bool CrtWcsncpyPlan(uint32_t dest, uint32_t source, uint32_t count, Range& write) {
  if (count > 0x00800000u) return false;
  write = {dest, 2 * count};
  return IsPlainMemory(dest, write.size) && IsPlainMemory(source, write.size);
}
inline bool CrtWcsncpy(const Memory& memory, uint32_t dest, uint32_t source, uint32_t count) {
  Range write;
  if (!CrtWcsncpyPlan(dest, source, count, write)) return false;
  uint8_t* to = memory.Host(dest);
  const uint8_t* from = memory.Host(source);
  uint32_t i = 0;
  while (i < count) {
    const uint8_t high = from[2 * i], low = from[2 * i + 1];
    to[2 * i] = high;
    to[2 * i + 1] = low;
    ++i;
    if (!(high | low)) break;
  }
  for (; i < count; ++i) to[2 * i] = to[2 * i + 1] = 0;
  return true;
}

// wcscpy: the source's 16-bit elements up to and with its zero one. Declines a source that reaches the register
// window or runs past 16 MB, a destination that overlaps it (its code would copy what it has just written) and
// ranges that are not plain memory.
inline bool CrtWcscpyPlan(const Memory& memory, uint32_t dest, uint32_t source, Range& write) {
  uint64_t length = 0;
  if (!CrtWcslen(memory, source, length) || length >= 0x00800000u) return false;
  write = {dest, uint32_t(2 * (length + 1))};
  return AreApart(dest, source, write.size) && detail::ArePlain(dest, source, write.size);
}
inline bool CrtWcscpy(const Memory& memory, uint32_t dest, uint32_t source) {
  Range write;
  if (!CrtWcscpyPlan(memory, dest, source, write)) return false;
  std::memcpy(memory.Host(dest), memory.Host(source), write.size);
  return true;
}

// strncat: after the destination's terminator, the source's bytes up to its terminator, `count` at most, then a
// terminator. Declines strings that reach the register window, a destination string of 16 MB or more, a written
// range that overlaps the source and ranges that are not plain memory.
inline bool CrtStrncatPlan(const Memory& memory, uint32_t dest, uint32_t source, uint32_t count, Range& write) {
  uint32_t end = dest;
  for (; *memory.Host(end); ++end) {
    if (IsRegisterWindow(end) || end - dest >= 0x01000000u) return false;
  }
  uint32_t length = 0;
  for (; length < count && length < 0x01000000u; ++length) {
    if (IsRegisterWindow(source + length)) return false;
    if (!*memory.Host(source + length)) break;
  }
  if (length >= 0x01000000u) return false;
  write = {end, length + 1};
  return AreApart(end, source, write.size) && detail::ArePlain(end, source, write.size);
}
inline bool CrtStrncat(const Memory& memory, uint32_t dest, uint32_t source, uint32_t count) {
  Range write;
  if (!CrtStrncatPlan(memory, dest, source, count, write)) return false;
  uint8_t* to = memory.Host(write.at);
  std::memcpy(to, memory.Host(source), write.size - 1);
  to[write.size - 1] = 0;
  return true;
}

// wcscmp: -1, 0 or 1 in the whole register, as the first 16-bit elements that differ compare (unsigned), 0 when the
// strings end together.
inline bool CrtWcscmp(const Memory& memory, uint32_t left, uint32_t right, uint64_t& r3) {
  for (uint32_t i = 0;; i += 2) {
    if (IsRegisterWindow(left + i) || IsRegisterWindow(right + i)) return false;
    const uint8_t* a = memory.Host(left + i);
    const uint8_t* b = memory.Host(right + i);
    const uint32_t x = uint32_t(a[0]) << 8 | a[1], y = uint32_t(b[0]) << 8 | b[1];
    if (x != y) {
      r3 = x < y ? ~0ull : 1;
      return true;
    }
    if (!x) {
      r3 = 0;
      return true;
    }
  }
}

// strlen and strcmp as loops of bytes. The three titles carry these two inside Havok's code (in Test Drive
// Unlimited's map; at the head of Havok in Crackdown's): middleware, kept as it is, so no title binds them. They
// are here, with their models and tests, for what they do: the C runtime's own strlen and strcmp are not the same
// instructions in the three titles and will each need its build read, and this is the value they must give.
// strlen: the bytes before the terminator, in the register's low half. strcmp: the difference of the first two
// bytes that differ, in the whole register, 0 when the strings end together.
inline bool CrtStrlen(const Memory& memory, uint32_t string, uint64_t& r3) {
  const uint32_t span = PlainSpan(string);
  if (!span) return false;
  const uint8_t* bytes = memory.Host(string);
  // The host's search for the terminator: it looks at no byte past the first zero's own aligned block.
  const void* end = std::memchr(bytes, 0, span);
  if (!end) return false;
  r3 = uint32_t(static_cast<const uint8_t*>(end) - bytes);
  return true;
}
inline bool CrtStrcmp(const Memory& memory, uint32_t left, uint32_t right, uint64_t& r3) {
  const uint32_t left_span = PlainSpan(left), right_span = PlainSpan(right);
  const uint32_t span = left_span < right_span ? left_span : right_span;
  const uint8_t* a = memory.Host(left);
  const uint8_t* b = memory.Host(right);
  for (uint32_t i = 0; i < span; ++i) {
    if (a[i] != b[i] || !a[i]) {
      r3 = uint64_t(int64_t(a[i]) - int64_t(b[i]));
      return true;
    }
  }
  return false;
}

// mbtowc of the "C" locale: a byte is a character. With no source or a count of 0, nothing and 0; otherwise the
// source's first byte as a 16-bit element at `dest` (when there is one) and 1, or 0 for the terminator. Declines a
// source in the register window and a destination that is not plain memory. The plan says what is written.
inline bool CrtMbtowcPlan(uint32_t dest, uint32_t source, uint32_t count, Range& write) {
  write = {dest, 0};
  if (!source || !count) return true;
  if (IsRegisterWindow(source)) return false;
  if (!dest) return true;
  write.size = 2;
  return IsPlainMemory(dest, 2);
}
inline bool CrtMbtowc(const Memory& memory, uint32_t dest, uint32_t source, uint32_t count, uint64_t& r3) {
  Range write;
  if (!CrtMbtowcPlan(dest, source, count, write)) return false;
  r3 = 0;
  if (!source || !count) return true;
  const uint8_t byte = *memory.Host(source);
  if (dest) {
    uint8_t* to = memory.Host(dest);
    to[0] = 0;
    to[1] = byte;
  }
  r3 = byte ? 1 : 0;
  return true;
}

// _stricmp: the difference of the first two bytes that differ once the upper-case ASCII letters of both are
// lowered, in the whole register, 0 when the strings end together. Its code leaves at the second string's end
// before it lowers anything: the value is then the first string's byte as it is ("B" against "" gives 66).
inline bool CrtStricmp(const Memory& memory, uint32_t left, uint32_t right, uint64_t& r3) {
  const uint32_t left_span = PlainSpan(left), right_span = PlainSpan(right);
  const uint32_t span = left_span < right_span ? left_span : right_span;
  const uint8_t* first = memory.Host(left);
  const uint8_t* second = memory.Host(right);
  for (uint32_t i = 0; i < span; ++i) {
    uint32_t a = first[i], b = second[i];
    if (!b) {
      r3 = a;
      return true;
    }
    if (a == b) continue;
    if (b - 65u <= 25u) b |= 32;
    if (a - 65u <= 25u) a |= 32;
    if (a != b) {
      r3 = uint64_t(int64_t(a) - int64_t(b));
      return true;
    }
  }
  return false;
}

// strstr: the address of the first place in the string where `needle` is, in the whole register (r3 as it came
// when `needle` is empty), or 0. Both strings must end in plain memory; the search is the host's.
inline bool CrtStrstr(const Memory& memory, uint64_t string_register, uint32_t needle, uint64_t& r3) {
  const uint32_t needle_span = PlainSpan(needle);
  if (!needle_span) return false;
  const uint8_t* wanted = memory.Host(needle);
  if (!wanted[0]) {
    r3 = string_register;
    return true;
  }
  const uint32_t string = uint32_t(string_register), span = PlainSpan(string);
  if (!span) return false;
  const uint8_t* bytes = memory.Host(string);
  if (!std::memchr(wanted, 0, needle_span) || !std::memchr(bytes, 0, span)) return false;
  const char* found = std::strstr(reinterpret_cast<const char*>(bytes), reinterpret_cast<const char*>(wanted));
  r3 = found ? string_register + uint64_t(found - reinterpret_cast<const char*>(bytes)) : 0;
  return true;
}

}  // namespace any360::xdk
