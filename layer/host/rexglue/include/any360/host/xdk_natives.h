// Native replacements for functions of the libraries a title links statically (the XDK's C runtime first): the same
// work done by the host's routines instead of recompiled PowerPC loops of byte-swapped loads and stores. Each is
// written once here and bound in a title at the address library recognition finds the function at
// (tools/library_scan.py; titles/<title>/modules.json):
//
//   #include "any360/host/xdk_natives.h"
//   ANY360_XDK_BIND(82645BF0, CrtMemcpy)      // sub_82645BF0 runs CrtMemcpy, its own code kept as the fallback
//
// A replacement does the common case and hands everything else to the function's own code (`original`): ranges that
// are not plain memory (the GPU's registers), copies whose overlap the host routine would treat differently.
//
// Settings: any360_xdk_natives (on by default; off: every bound function runs its own code) and any360_xdk_verify
// (off by default): each call runs the function's own code first, then the replacement on the same input, and
// compares what both wrote and returned; a difference is logged and the function's own result kept. A title turns
// it on once for each newly bound function, on a real run, before trusting the binding.
#pragma once

#include <cstdint>

#include "rexglue.h"

namespace any360::host::rexglue::xdk {

// memcpy as the XDK's CRT builds it for long copies: r3 destination, r4 source, r5 size; copies forward, returns the
// destination and leaves it at r1 - 8 (its own save of r3 below the stack pointer).
void CrtMemcpy(PPCContext& ctx, uint8_t* base, GuestFunction original);
// The CRT's plain forward copy (bytes to alignment, then words): r3 destination, r4 source, r5 size; returns the
// destination.
void CrtCopyForward(PPCContext& ctx, uint8_t* base, GuestFunction original);
// memset: r3 destination, r4 fill byte, r5 size; returns the destination.
void CrtMemset(PPCContext& ctx, uint8_t* base, GuestFunction original);

// Calls handled natively, handed to the function's own code, verified and found different, since the start.
struct Counters {
  uint64_t native = 0, original = 0, verified = 0, different = 0;
};
Counters GetCounters();

}  // namespace any360::host::rexglue::xdk

// `sub_<address>` (the name the title's generated code calls) runs `handler`, with the function's own code
// (`__imp__sub_<address>`, what the weak `sub_<address>` otherwise aliases) as its fallback.
#define ANY360_XDK_BIND(address, handler)                                                                  \
  extern "C" void __imp__sub_##address(PPCContext& __restrict ctx, uint8_t* base);                         \
  extern "C" void sub_##address(PPCContext& __restrict ctx, uint8_t* base) {                               \
    ::any360::host::rexglue::xdk::handler(ctx, base, &__imp__sub_##address);                               \
  }
