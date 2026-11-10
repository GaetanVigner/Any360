// any360::xdk: native replacements of functions of the XDK's XAPI library (xapilib), as crt.h has the C runtime's.
// The rule they follow: docs/NATIVE_FUNCTIONS.md.
#pragma once

#include <cstdint>
#include <cstring>

#include "crt.h"

namespace any360::xdk {

// The XDK's vector copy (r3 destination, r4 source, r5 size; returns the destination): the function the three titles
// read carry next to XAPI, which copies 16-byte vectors, asks for the source's cache lines ahead (dcbt) and zeroes
// the destination's 128-byte cache lines ahead of writing them (dcbz128). Its name is not in the titles; what it
// does is XMemCpy's, the name used here.
//
// For two ranges that do not overlap it writes exactly the `size` bytes of the destination, a forward copy. When
// they overlap the lines it zeroes ahead can be the source's own bytes, not read yet: the result is neither
// memcpy's nor memmove's, and the function's own code keeps those calls. Below 16 bytes its code copies the last
// bytes in a loop that subtracts a global of the title from the count (1 in the titles read): `small_sizes` says
// the caller has checked that global, without which those sizes are the function's own code's too.
inline bool XMemCpyAccepts(uint32_t dest, uint32_t source, uint32_t size, bool small_sizes) {
  if (size < 16 && !small_sizes) return false;
  const bool apart = dest >= source ? dest - source >= size : source - dest >= size;
  return apart && detail::ArePlain(dest, source, size);
}

// The copy is the host's memmove alone: the sizes the titles pass it are long (The Simpsons Game: 97% of 22 M
// calls between 1 and 2 KB), where the paths crt.h writes by hand for short copies only add their tests
// (xdk_xapi_bench: 0.97x on the titles' mixes with them).
inline bool XMemCpy(const Memory& memory, uint32_t dest, uint32_t source, uint32_t size, bool small_sizes) {
  if (!XMemCpyAccepts(dest, source, size, small_sizes)) return false;
  std::memmove(memory.Host(dest), memory.Host(source), size);
  return true;
}

}  // namespace any360::xdk
