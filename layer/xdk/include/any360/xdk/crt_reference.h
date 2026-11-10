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

// memset: the low byte of `value`, `size` times.
inline void Memset(const Memory& memory, uint32_t dest, uint32_t value, uint32_t size) {
  for (uint32_t i = 0; i < size; ++i) *memory.Host(dest + i) = uint8_t(value);
}

}  // namespace any360::xdk::reference
