// any360::xdk: native replacements of the functions a title links statically from the XDK's libraries, the C runtime
// first. The same work as the recompiled function, done by the host: portable C++ with no dependency, so each one is
// unit-tested (tests/xdk_crt_tests.cpp, against the reference models of crt_reference.h) and benchmarked
// (tests/xdk_crt_bench.cpp) here, outside any title. docs/NATIVE_FUNCTIONS.md is the rule they follow.
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

// memmove for the sizes the titles copy most (up to 64 bytes: the two ends of the range, overlapping in the
// middle, every load before any store, so overlap is as memmove's), the host's memmove above. What the benchmark
// kept (xdk_crt_bench, on a title's mix of sizes): this is 1.15x the plain memmove; the same path carried on to 128
// bytes gained nothing, and one path without a branch on the size for 16 to 64 bytes was slower.
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
  std::memmove(d, s, n);
}

// memset, the sizes up to 128 bytes as overlapping stores of the repeated byte (1.35x the plain memset on a
// title's mix of sizes, two fills in three of which are 120 bytes), the host's memset above.
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

// memcpy as the XDK's CRT builds it for long copies: copies `size` bytes forward from `source` to the destination
// and leaves the destination's register, all 64 bits of it as a big-endian value, in the 8 bytes below the stack
// pointer (its own save of r3: std r3,-8(r1)). `dest_register` is that register: its low half is the address, its
// high half whatever the caller's arithmetic left there (not always zero: a title's calls showed it).
// Declines a copy whose destination starts inside its source and ranges that are not plain memory.
inline bool CrtMemcpy(const Memory& memory, uint64_t dest_register, uint32_t source, uint32_t size,
                      uint32_t stack_pointer) {
  const uint32_t dest = uint32_t(dest_register);
  if (!IsPlainForwardCopy(dest, source, size) || !detail::ArePlain(dest, source, size)) return false;
  detail::Store64(memory.Host(stack_pointer - 8), detail::ByteSwap64(dest_register));
  detail::Move(memory.Host(dest), memory.Host(source), size);
  return true;
}

// The CRT's plain forward copy (bytes to alignment, then words). Declines as CrtMemcpy.
inline bool CrtCopyForward(const Memory& memory, uint32_t dest, uint32_t source, uint32_t size) {
  if (!IsPlainForwardCopy(dest, source, size) || !detail::ArePlain(dest, source, size)) return false;
  detail::Move(memory.Host(dest), memory.Host(source), size);
  return true;
}

// memset: `size` bytes of the low byte of `value` from `dest`. Declines ranges that are not plain memory.
inline bool CrtMemset(const Memory& memory, uint32_t dest, uint32_t value, uint32_t size) {
  if (!IsPlainMemory(dest, size)) return false;
  detail::Fill(memory.Host(dest), uint8_t(value), size);
  return true;
}

}  // namespace any360::xdk
