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

// The XDK's vector fill (r3 destination, r4 the byte, r5 size; returns the destination), XMemCpy's counterpart: the
// function writes the low byte of r4 into the `size` bytes of the destination, memset's result. Below 16 bytes it
// stores them one by one; from 16 on, the bytes up to the next 16 (stvlx), 16-byte vectors up to the next 128, the
// 128-byte cache lines zeroed (dcbz128) then filled, the last vectors and the last bytes (stvrx). The vectors it
// stores apart from the cache lines are placed by a global of the title added to the address after each one (16 in
// the titles read: `vector_step_16` says the caller has checked it): sizes from 16 on are the function's own code
// without it. Its name is not in the titles; what it does is XMemSet's, the name used here. Its register save (r28-
// r31 and the return address below r1) is a frame no caller can see, as XMemCpy's.
inline bool XMemSetAccepts(uint32_t dest, uint32_t size, bool vector_step_16) {
  return (size < 16 || vector_step_16) && IsPlainMemory(dest, size);
}
inline bool XMemSet(const Memory& memory, uint32_t dest, uint32_t byte, uint32_t size, bool vector_step_16) {
  if (!XMemSetAccepts(dest, size, vector_step_16)) return false;
  std::memset(memory.Host(dest), int(byte & 0xFF), size);
  return true;
}

// GetLastError and SetLastError: the thread's last error, a 32-bit field of the kernel's thread (+352) that the
// processor's control region (r13) names at +256. While that region's field at +336 is not zero (a DPC runs: there
// is no thread to speak of) GetLastError gives 0 and SetLastError stores nothing. Both decline a control region or
// a thread that is not plain memory. SetLastError leaves r3 as it came; its plan says what it writes.
namespace detail {
inline uint32_t LoadBig32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
// Whether the control region can be read, and where the thread's last error is (0: a DPC runs, nowhere).
inline bool LastErrorField(const Memory& memory, uint32_t pcr, uint32_t& field) {
  field = 0;
  if (!IsPlainMemory(pcr, 340)) return false;
  if (LoadBig32(memory.Host(pcr + 336))) return true;
  field = LoadBig32(memory.Host(pcr + 256)) + 352;
  return IsPlainMemory(field, 4);
}
}  // namespace detail
inline bool XapiGetLastError(const Memory& memory, uint32_t pcr, uint64_t& r3) {
  uint32_t field;
  if (!detail::LastErrorField(memory, pcr, field)) return false;
  r3 = field ? detail::LoadBig32(memory.Host(field)) : 0;
  return true;
}
inline bool XapiSetLastErrorPlan(const Memory& memory, uint32_t pcr, Range& write) {
  uint32_t field;
  write = {0, 0};
  if (!detail::LastErrorField(memory, pcr, field)) return false;
  if (field) write = {field, 4};
  return true;
}
inline bool XapiSetLastError(const Memory& memory, uint32_t pcr, uint32_t error) {
  Range write;
  if (!XapiSetLastErrorPlan(memory, pcr, write)) return false;
  if (write.size) {
    uint8_t* to = memory.Host(write.at);
    to[0] = uint8_t(error >> 24), to[1] = uint8_t(error >> 16), to[2] = uint8_t(error >> 8), to[3] = uint8_t(error);
  }
  return true;
}

}  // namespace any360::xdk
