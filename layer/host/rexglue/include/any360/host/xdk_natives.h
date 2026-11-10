// A ReXGlue title's binding of any360::xdk's native replacements (layer/xdk: the functions a title links statically
// from the XDK's libraries, the C runtime first, done by the host's routines instead of recompiled PowerPC loops of
// byte-swapped loads and stores). Each replacement is written, unit-tested and benchmarked once in layer/xdk and
// bound in a title at the address library recognition finds the function at (tools/library_scan.py):
//
//   #include "any360/host/xdk_natives.h"
//   ANY360_XDK_BIND(82645BF0, CrtMemcpy)      // sub_82645BF0 runs CrtMemcpy, its own code kept as the fallback
//
// A replacement does the common case and hands everything else to the function's own code: ranges that are not
// plain memory (the GPU's registers), copies whose overlap the host routine would treat differently.
//
// Settings (docs/NATIVE_FUNCTIONS.md says when each is used):
//   any360_xdk_natives  on by default; off: every bound function runs its own code.
//   any360_xdk_verify   each call runs the function's own code first, then the replacement on the same input, and
//                       compares what both wrote and returned, and what either wrote around the destination
//                       (outside it: nothing should be); a difference is logged and the function's own result
//                       kept. Run once, in game, for every newly bound function.
//   any360_xdk_bench    each call is timed both ways (the function's own code and the replacement, in alternating
//                       order) and the log gets, size class by size class, the calls and the nanoseconds of each:
//                       the title's own mix of sizes and what the replacement really saves.
#pragma once

#include <cstdint>

#include "any360/xdk/crt.h"
#include "any360/xdk/xapi.h"
#include "rexglue.h"

namespace any360::host::rexglue::xdk {

enum class Function : uint32_t { kMemcpy, kCopyForward, kMemset, kMemmove, kXMemCpy, kCount };

// How bound functions run: resolved from the settings at the first call.
enum class Mode : uint32_t { kUnresolved, kNative, kOriginal, kVerify, kBench };
extern Mode g_mode;
// Guest memory as the runtime maps it (the physical view's host offset), resolved with the mode.
extern any360::xdk::Memory g_memory;

// Every mode but the plain native one: resolves the mode, runs the function's own code, verifies or times.
// `value` is the function's parameter of the title, if it has one (XMemCpy: the address of its byte step).
void RunSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t value = 0);

// r3 destination, r4 source, r5 size; returns the destination (r3 as it came).
inline void CrtMemcpy(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtMemcpy(g_memory, ctx.r3.u64, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kMemcpy, ctx, base, original);
}
inline void CrtCopyForward(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtCopyForward(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kCopyForward, ctx, base, original);
}
// r3 destination, r4 fill byte, r5 size; returns the destination.
inline void CrtMemset(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtMemset(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kMemset, ctx, base, original);
}

// r3 destination, r4 source, r5 size; returns the destination. Below the source it is memcpy.
inline void CrtMemmove(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtMemmove(g_memory, ctx.r3.u64, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kMemmove, ctx, base, original);
}
// Whether the 32-bit global at `address` is 1: the count XMemCpy's byte loop subtracts in the title's build
// (0: not given, taken as not 1).
inline bool ByteStepIsOne(uint32_t address) {
  if (!address) return false;
  const uint8_t* p = g_memory.Host(address);
  return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}
// The XDK's vector copy: r3 destination, r4 source, r5 size; returns the destination. `byte_step_address` is the
// address of the global its byte loop subtracts from the count, read from the title's build of the function (the
// lis / lwz pair before the loop); without it copies below 16 bytes are left to the function's own code.
inline void XMemCpy(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t byte_step_address = 0) {
  if (g_mode == Mode::kNative) [[likely]] {
    const uint32_t size = ctx.r5.u32;
    if (any360::xdk::XMemCpy(g_memory, ctx.r3.u32, ctx.r4.u32, size, size >= 16 || ByteStepIsOne(byte_step_address)))
        [[likely]] {
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kXMemCpy, ctx, base, original, byte_step_address);
}

// Calls verified against the functions' own code and found different, since the start (any360_xdk_verify).
struct Counters {
  uint64_t verified = 0, different = 0;
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
// The same for a handler that takes a parameter of the title (XMemCpy: the address of its byte step).
#define ANY360_XDK_BIND_WITH(address, handler, value)                                                      \
  extern "C" void __imp__sub_##address(PPCContext& __restrict ctx, uint8_t* base);                         \
  extern "C" void sub_##address(PPCContext& __restrict ctx, uint8_t* base) {                               \
    ::any360::host::rexglue::xdk::handler(ctx, base, &__imp__sub_##address, value);                        \
  }
