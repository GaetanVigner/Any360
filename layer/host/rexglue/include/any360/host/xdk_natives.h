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
//                       kept. Two sides that differ are run again on the same input: when they then agree, another
//                       thread wrote meanwhile, and the call is counted apart. Run once, in game, for every newly
//                       bound function.
//   any360_xdk_skip     the names of bound functions that run their own code, the others native ("strncpy,wcscpy"):
//                       to find which function a title's trouble comes from, by halves.
//   any360_xdk_bench    each call is timed both ways (the function's own code and the replacement, in alternating
//                       order) and the log gets, size class by size class, the calls and the nanoseconds of each:
//                       the title's own mix of sizes and what the replacement really saves.
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "any360/xdk/crt.h"
#include "any360/xdk/crt_math.h"
#include "any360/xdk/d3d.h"
#include "any360/xdk/d3d_state.h"
#include "any360/xdk/xaudio.h"
#include "any360/xdk/libpng.h"
#include "any360/xdk/zlib.h"
#include "d3d_selftest.h"
#include "any360/xdk/xapi.h"
#include "rexglue.h"

namespace any360::host::rexglue::d3d_draw {
bool DrawRecords();  // any360_d3d_records (d3d_draw.cpp)
}

namespace any360::host::rexglue::xdk {

enum class Function : uint32_t {
  kMemcpy, kCopyForward, kMemset, kMemmove, kXMemCpy, kWcslen, kStrchr, kStrncmp, kStrncpy,
  kStrrchr, kTolower, kToupper, kWcsncpy, kWcscpy, kStrncat, kWcscmp, kStrlen, kStrcmp, kMbtowc,
  kGetLastError, kSetLastError, kStricmp, kStrstr, kFinite, kIsnan, kSptype, kGetExp, kSetExp, kAddExp, kMixRamp, kDirtyConstants, kDirtyWords, kSetConstants, kFill,
  kD3dBlend, kD3dBoolConstants, kD3dIntConstants, kD3dPixelConstants, kD3dSetTexture, kD3dSetStreamSource,
  kD3dSamplerFilter, kD3dSetVertexDeclaration, kD3dSetShader, kD3dFlushCpuRange, kInflateFast, kPngFilterRow, kXMemSet, kSin, kCos, kAsin,
  kD3dRenderState, kAtan2, kPow, kD3dSurfaces, kCount
};

// How bound functions run: resolved from the settings at the first call.
enum class Mode : uint32_t { kUnresolved, kNative, kOriginal, kVerify, kBench, kSelect };
extern Mode g_mode;
// Guest memory as the runtime maps it (the physical view's host offset), resolved with the mode.
extern any360::xdk::Memory g_memory;

// Every mode but the plain native one: resolves the mode, runs the function's own code, verifies or times.
// `value` is the function's parameter of the title, if it has one (XMemCpy: the address of its byte step).
void RunSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t value = 0);
// The same for XAudio's mix (its own registers and memory: XAudioMixRamp below).
void RunMixSlow(PPCContext& ctx, uint8_t* base, GuestFunction original);
// And for the D3D runtime's upload of the dirty shader constants (D3dDirtyConstants below).
void RunConstantsSlow(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t cursor_offset, bool words);
// And for its copy of a title's shader constants into the device (D3dSetShaderConstants below).
void RunSetConstantsSlow(PPCContext& ctx, uint8_t* base, GuestFunction original);
// And for XAudio's fill (XAudioFill below).
void RunFillSlow(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t floats);
// And for zlib's inflate_fast (ZlibInflateFast below).
void RunInflateFastSlow(PPCContext& ctx, uint8_t* base, GuestFunction original);
// And for the C runtime's sin, cos and asin (CrtSin, CrtCos, CrtAsin below): `function` one of the three.
void RunCrtMathSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original,
                    const any360::xdk::CrtTrigBuild& trig, const any360::xdk::CrtAsinBuild& asin);
// And for the C runtime's atan2 and pow (CrtAtan2, CrtPow below): `function` one of the two.
void RunCrtMath2Slow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original,
                     const any360::xdk::CrtAtan2Build& atan2, const any360::xdk::CrtPowBuild& pow);
// And for libpng's png_read_filter_row (PngReadFilterRow below).
void RunPngFilterRowSlow(PPCContext& ctx, uint8_t* base, GuestFunction original);
// And for the D3D runtime's state setters (D3dState below): `native` the replacement on the registers (false: it
// declines), `writes` what it may write (false: it declines).
using D3dStateNative = bool (*)(const any360::xdk::Memory&, PPCContext&);
using D3dStateWrites = bool (*)(const PPCContext&, std::vector<any360::xdk::Range>&);
// `keeps_r3`: the function leaves r3 as it came (compared); otherwise it returns nothing and leaves r3 whatever its
// last use was (not compared).
void RunD3dStateSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original, D3dStateNative native,
                     D3dStateWrites writes, bool keeps_r3 = true);

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
// Whether the 32-bit global at `address` is 16: the step XMemSet adds after each vector it stores apart from the
// cache lines, in the title's build (0: not given, taken as not 16).
inline bool VectorStepIs16(uint32_t address) {
  if (!address) return false;
  const uint8_t* p = g_memory.Host(address);
  return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 16;
}
// The XDK's vector fill: r3 destination, r4 the byte, r5 size; returns the destination. `vector_step_address` is the
// address of the global its vector stores step by, read from the title's build of the function (the lis / lwz pair
// before its first vector loop); without it fills of 16 bytes and more are left to the function's own code.
inline void XMemSet(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t vector_step_address = 0) {
  if (g_mode == Mode::kNative) [[likely]] {
    const uint32_t size = ctx.r5.u32;
    if (any360::xdk::XMemSet(g_memory, ctx.r3.u32, ctx.r4.u32, size, size < 16 || VectorStepIs16(vector_step_address)))
        [[likely]] {
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kXMemSet, ctx, base, original, vector_step_address);
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

// The C runtime's string functions. Those that only read leave their value in r3.
// wcslen: r3 string; returns its length in 16-bit elements.
inline void CrtWcslen(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtWcslen(g_memory, ctx.r3.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kWcslen, ctx, base, original);
}
// strchr: r3 string, r4 character; returns the address of its first occurrence or 0.
inline void CrtStrchr(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrchr(g_memory, ctx.r3.u64, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrchr, ctx, base, original);
}
// strncmp: r3 and r4 strings, r5 count; returns the difference of the first bytes that differ.
inline void CrtStrncmp(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrncmp(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrncmp, ctx, base, original);
}
// strncpy: r3 destination, r4 source, r5 count; returns the destination.
inline void CrtStrncpy(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtStrncpy(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrncpy, ctx, base, original);
}
// strrchr: r3 string, r4 character; returns the address of its last occurrence or 0.
inline void CrtStrrchr(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrrchr(g_memory, ctx.r3.u64, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrrchr, ctx, base, original);
}
// tolower and toupper: r3 the character, changed in place.
inline void CrtTolower(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    ctx.r3.u64 = any360::xdk::CrtTolower(ctx.r3.u64);
    return;
  }
  RunSlow(Function::kTolower, ctx, base, original);
}
inline void CrtToupper(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    ctx.r3.u64 = any360::xdk::CrtToupper(ctx.r3.u64);
    return;
  }
  RunSlow(Function::kToupper, ctx, base, original);
}
// wcsncpy: r3 destination, r4 source, r5 count of 16-bit elements; returns the destination.
inline void CrtWcsncpy(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtWcsncpy(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kWcsncpy, ctx, base, original);
}
// wcscpy: r3 destination, r4 source; returns the destination.
inline void CrtWcscpy(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtWcscpy(g_memory, ctx.r3.u32, ctx.r4.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kWcscpy, ctx, base, original);
}
// strncat: r3 destination, r4 source, r5 count; returns the destination.
inline void CrtStrncat(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::CrtStrncat(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrncat, ctx, base, original);
}

// wcscmp: r3 and r4 strings of 16-bit elements; returns -1, 0 or 1.
inline void CrtWcscmp(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtWcscmp(g_memory, ctx.r3.u32, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kWcscmp, ctx, base, original);
}
// strlen and strcmp, the builds that are loops of bytes (Havok's own in the titles read: checked and measured
// through these, bound in no title): r3 string (and r4); return the length, the difference of the first bytes
// that differ.
inline void CrtStrlen(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrlen(g_memory, ctx.r3.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrlen, ctx, base, original);
}
inline void CrtStrcmp(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrcmp(g_memory, ctx.r3.u32, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrcmp, ctx, base, original);
}
// _stricmp: r3 and r4 strings; returns the difference of the first bytes that differ, letters lowered.
inline void CrtStricmp(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStricmp(g_memory, ctx.r3.u32, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStricmp, ctx, base, original);
}
// strstr: r3 string, r4 the string looked for; returns the address of its first occurrence or 0.
inline void CrtStrstr(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtStrstr(g_memory, ctx.r3.u64, ctx.r4.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kStrstr, ctx, base, original);
}
// mbtowc: r3 destination (or 0), r4 source, r5 count; returns 1 for a character, 0 for none or the terminator.
inline void CrtMbtowc(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::CrtMbtowc(g_memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kMbtowc, ctx, base, original);
}
// XAPI's GetLastError (returns the thread's last error) and SetLastError (r3 the error, left as it came): the
// thread is the one the control region in r13 names.
inline void XapiGetLastError(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t r3;
    if (any360::xdk::XapiGetLastError(g_memory, ctx.r13.u32, r3)) [[likely]] {
      ctx.r3.u64 = r3;
      return;
    }
    original(ctx, base);
    return;
  }
  RunSlow(Function::kGetLastError, ctx, base, original);
}
inline void XapiSetLastError(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::XapiSetLastError(g_memory, ctx.r13.u32, ctx.r3.u32)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunSlow(Function::kSetLastError, ctx, base, original);
}

// The C runtime's helpers on a double's bits: f1 the double (r4 an exponent for the last two). The first four
// return a value in r3, the last two a double in f1. Each stores f1 at r1 + 16 as its code does. The host's
// flush-to-zero mode is left off as the function's first instruction (a store of a double) leaves it.
#define ANY360_XDK_DOUBLE_TO_R3(Name, function)                                                    \
  inline void Name(PPCContext& ctx, uint8_t* base, GuestFunction original) {                        \
    if (g_mode == Mode::kNative) [[likely]] {                                                       \
      uint64_t r3;                                                                                  \
      if (any360::xdk::Name(g_memory, ctx.r1.u32, ctx.f1.u64, r3)) [[likely]] {                     \
        ctx.fpscr.disableFlushMode();                                                               \
        ctx.r3.u64 = r3;                                                                            \
        return;                                                                                     \
      }                                                                                             \
      original(ctx, base);                                                                          \
      return;                                                                                       \
    }                                                                                               \
    RunSlow(Function::function, ctx, base, original);                                               \
  }
ANY360_XDK_DOUBLE_TO_R3(CrtFinite, kFinite)
ANY360_XDK_DOUBLE_TO_R3(CrtIsnan, kIsnan)
ANY360_XDK_DOUBLE_TO_R3(CrtSptype, kSptype)
ANY360_XDK_DOUBLE_TO_R3(CrtGetExp, kGetExp)
#undef ANY360_XDK_DOUBLE_TO_R3
#define ANY360_XDK_DOUBLE_TO_F1(Name, function)                                                    \
  inline void Name(PPCContext& ctx, uint8_t* base, GuestFunction original) {                        \
    if (g_mode == Mode::kNative) [[likely]] {                                                       \
      uint64_t f1;                                                                                  \
      if (any360::xdk::Name(g_memory, ctx.r1.u32, ctx.f1.u64, ctx.r4.u32, f1)) [[likely]] {         \
        ctx.fpscr.disableFlushMode();                                                               \
        ctx.f1.u64 = f1;                                                                            \
        return;                                                                                     \
      }                                                                                             \
      original(ctx, base);                                                                          \
      return;                                                                                       \
    }                                                                                               \
    RunSlow(Function::function, ctx, base, original);                                               \
  }
ANY360_XDK_DOUBLE_TO_F1(CrtSetExp, kSetExp)
ANY360_XDK_DOUBLE_TO_F1(CrtAddExp, kAddExp)
#undef ANY360_XDK_DOUBLE_TO_F1

// XAudio's mix with a volume ramp (any360/xdk/xaudio.h): r3 destination, r4 source, r5 and r6 the gains and their
// step, r7 samples. The title's registers to and from the replacement's: vectors in the guest's element order
// there, host lane 3 - i holding element i here. Only the registers the function writes are given back.
inline void ToVmx(const PPCContext& ctx, any360::xdk::VmxRegisters& x) {
  const uint64_t r[] = {0,         ctx.r1.u64, 0,          ctx.r3.u64, ctx.r4.u64,  ctx.r5.u64,
                        ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64, ctx.r11.u64};
  std::memcpy(x.gpr, r, sizeof r);
  x.gpr[31] = ctx.r31.u64;
}
inline void VectorTo(decltype(PPCContext::v0)& v, const uint32_t (&e)[4]) {
  for (int i = 0; i < 4; ++i) v.u32[3 - i] = e[i];
}
inline void FromVmx(const any360::xdk::VmxRegisters& x, PPCContext& ctx) {
  ctx.r3.u64 = x.gpr[3];
  ctx.r4.u64 = x.gpr[4];
  ctx.r5.u64 = x.gpr[5];
  ctx.r6.u64 = x.gpr[6];
  ctx.r7.u64 = x.gpr[7];
  ctx.r8.u64 = x.gpr[8];
  ctx.r9.u64 = x.gpr[9];
  ctx.r10.u64 = x.gpr[10];
  ctx.r11.u64 = x.gpr[11];
  VectorTo(ctx.v0, x.vr[0]);
  VectorTo(ctx.v10, x.vr[10]);
  VectorTo(ctx.v11, x.vr[11]);
  VectorTo(ctx.v12, x.vr[12]);
  VectorTo(ctx.v13, x.vr[13]);
}
// The vectors a mix of at least one group writes besides v0 and v10-v13.
inline void GroupVectorsFromVmx(const any360::xdk::VmxRegisters& x, PPCContext& ctx) {
  VectorTo(ctx.v1, x.vr[1]);
  VectorTo(ctx.v2, x.vr[2]);
  VectorTo(ctx.v3, x.vr[3]);
  VectorTo(ctx.v4, x.vr[4]);
  VectorTo(ctx.v5, x.vr[5]);
  VectorTo(ctx.v6, x.vr[6]);
  VectorTo(ctx.v7, x.vr[7]);
  VectorTo(ctx.v8, x.vr[8]);
  VectorTo(ctx.v9, x.vr[9]);
  VectorTo(ctx.v29, x.vr[29]);
  VectorTo(ctx.v30, x.vr[30]);
  VectorTo(ctx.v31, x.vr[31]);
}
inline void XAudioMixRamp(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    any360::xdk::VmxRegisters x;
    ToVmx(ctx, x);
    const bool groups = any360::xdk::detail::MixRampGroups(uint32_t(x.gpr[7])) != 0;
    ctx.fpscr.enableFlushMode();  // as the function's first vector instruction does
    if (any360::xdk::XAudioMixRamp(g_memory, x)) [[likely]] {
      FromVmx(x, ctx);
      if (groups) GroupVectorsFromVmx(x, ctx);
      return;
    }
    original(ctx, base);
    return;
  }
  RunMixSlow(ctx, base, original);
}

// The D3D runtime's upload of the shader constants a mask marks as changed (any360/xdk/d3d.h): r3 the device,
// r4 the mask, r5 the first bit's register index, r6 the constants; `cursor_offset` is where the runtime's device
// keeps its command buffer's write pointer (the buffer's end next to it). Leaves r4 and r5 as the function does;
// a run without room in the buffer is the function's own code (it calls the runtime to make room).
inline void D3dDirtyConstants(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t cursor_offset) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t index = ctx.r5.u64, cursor = 0;
    // With any360_d3d_records, Any360's register records (the PM4 form when they would not fit).
    if ((d3d_draw::DrawRecords() &&
         any360::xdk::D3dDirtyConstantsRecords(g_memory, ctx.r3.u32, cursor_offset, ctx.r4.u64, index, ctx.r6.u32, cursor)) ||
        any360::xdk::D3dDirtyConstants(g_memory, ctx.r3.u32, cursor_offset, ctx.r4.u64, index, ctx.r6.u32, cursor))
        [[likely]] {
      ctx.r4.u64 = cursor;
      ctx.r5.u64 = index;
      return;
    }
    original(ctx, base);
    return;
  }
  RunConstantsSlow(ctx, base, original, cursor_offset, false);
}
// The same upload for constants of 4 bytes a bit (any360/xdk/d3d.h, D3dDirtyWords).
inline void D3dDirtyWords(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t cursor_offset) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint64_t index = ctx.r5.u64, cursor = 0;
    if ((d3d_draw::DrawRecords() &&
         any360::xdk::D3dDirtyWordsRecords(g_memory, ctx.r3.u32, cursor_offset, ctx.r4.u64, index, ctx.r6.u32, cursor)) ||
        any360::xdk::D3dDirtyWords(g_memory, ctx.r3.u32, cursor_offset, ctx.r4.u64, index, ctx.r6.u32, cursor)) [[likely]] {
      ctx.r4.u64 = cursor;
      ctx.r5.u64 = index;
      return;
    }
    original(ctx, base);
    return;
  }
  RunConstantsSlow(ctx, base, original, cursor_offset, true);
}

// The D3D runtime's copy of a title's shader constants into the device (any360/xdk/d3d.h): r3 the device, r4 the
// first register, r5 the constants, r6 how many, r7 the dirty bits; r3 left as it came.
inline void D3dSetShaderConstants(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  d3d::SawDevice(ctx, base, ctx.r3.u32);  // the D3D self-test, when asked, starts here
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::D3dSetShaderConstants(g_memory, ctx.r3.u64, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64,
                                           ctx.r1.u32)) [[likely]] {
      return;
    }
    original(ctx, base);
    return;
  }
  RunSetConstantsSlow(ctx, base, original);
}

// XAudio's fill of a block's buffer with its value (any360/xdk/xaudio.h): r3 the block; `floats` the build's size
// (eight times its rounds). Leaves f0 the value as a double, as the function's lfs does (flush mode off first).
inline void XAudioFill(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t floats) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint32_t bits;
    if (any360::xdk::XAudioFill(g_memory, ctx.r3.u32, floats, bits)) [[likely]] {
      ctx.fpscr.disableFlushMode();
      ctx.f0.f64 = double(std::bit_cast<float>(bits));
      return;
    }
    original(ctx, base);
    return;
  }
  RunFillSlow(ctx, base, original, floats);
}

// What the register save of a function that keeps r`first`-r31 leaves below r1 (the CRT's __savegprlr_<first>): the
// registers as doublewords from r1 - 8 * (33 - first), the return address (mflr r12) as a word at r1 - 8. A
// replacement that does the whole function writes it as the function would; r12 is left the return address, as its
// restore leaves it. The caller has checked that [r1 - 8 * (33 - first), r1) is plain memory.
inline void SaveGprLr(PPCContext& ctx, uint32_t first) {
  const uint32_t r1 = ctx.r1.u32;
  const uint64_t gpr[10] = {ctx.r22.u64, ctx.r23.u64, ctx.r24.u64, ctx.r25.u64, ctx.r26.u64,
                            ctx.r27.u64, ctx.r28.u64, ctx.r29.u64, ctx.r30.u64, ctx.r31.u64};
  for (uint32_t n = first; n < 32; ++n) {
    any360::xdk::detail::Store64(g_memory.Host(r1 - 8 * (33 - n)), any360::xdk::detail::ByteSwap64(gpr[n - 22]));
  }
  any360::xdk::detail::GuestStore32(g_memory, r1 - 8, uint32_t(ctx.lr));
  ctx.r12.u64 = uint32_t(ctx.lr);
}
inline void SaveGprLr22(PPCContext& ctx) { SaveGprLr(ctx, 22); }

// libpng 1.0.5's png_read_filter_row (any360/xdk/libpng.h): r3 png_ptr, r4 row_info, r5 row, r6 prev_row, r7 the
// filter. Leaves what the function leaves below r1: r29-r31 and the return address saved, and its 112-byte frame's
// back chain (stwu r1,-112(r1): the caller's r1 at r1 - 112). Declined (a bad filter, the function warns): its own code.
inline void PngReadFilterRow(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (any360::xdk::IsPlainMemory(ctx.r1.u32 - 112, 112) &&
        any360::xdk::PngReadFilterRow(g_memory, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32)) [[likely]] {
      SaveGprLr(ctx, 29);
      any360::xdk::detail::GuestStore32(g_memory, ctx.r1.u32 - 112, ctx.r1.u32);
      return;
    }
    original(ctx, base);
    return;
  }
  RunPngFilterRowSlow(ctx, base, original);
}

// zlib 1.1.4's inflate_fast (any360/xdk/zlib.h): r3 bl, r4 bd, r5 tl, r6 td, r7 the blocks state, r8 the stream;
// returns r3 (0, 1). A call handed back mid-way (an invalid code) goes on in the function's own code from the state
// the native one left; one declined is the function's own code from the start.
inline void ZlibInflateFast(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    uint32_t result = 0;
    const any360::xdk::InflateFastCall call{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    if (any360::xdk::IsPlainMemory(ctx.r1.u32 - 88, 88) &&
        any360::xdk::InflateFast(g_memory, call, result) == any360::xdk::InflateOutcome::kDone) [[likely]] {
      SaveGprLr22(ctx);
      ctx.r3.u64 = result;
      return;
    }
    original(ctx, base);  // declined, or handed back: the function goes on from the state left
    return;
  }
  RunInflateFastSlow(ctx, base, original);
}

// The C runtime's sin, cos and asin (any360/xdk/crt_math.h): f1 the argument and the result; sin and cos leave the
// words the function stores below r1. `build` says where the title's build keeps its coefficients. The host's
// flush-to-zero mode is turned off first, as the function's first instruction does (and stays off, as after it).
inline void CrtSin(PPCContext& ctx, uint8_t* base, GuestFunction original, const any360::xdk::CrtTrigBuild& build) {
  ctx.fpscr.disableFlushMode();
  if (g_mode == Mode::kNative) [[likely]] {
    double result;
    if (any360::xdk::CrtSin(g_memory, build, ctx.r1.u32, ctx.f1.f64, result)) [[likely]] {
      ctx.f1.f64 = result;
      return;
    }
    original(ctx, base);
    return;
  }
  RunCrtMathSlow(Function::kSin, ctx, base, original, build, {});
}
inline void CrtCos(PPCContext& ctx, uint8_t* base, GuestFunction original, const any360::xdk::CrtTrigBuild& build) {
  ctx.fpscr.disableFlushMode();
  if (g_mode == Mode::kNative) [[likely]] {
    double result;
    if (any360::xdk::CrtCos(g_memory, build, ctx.r1.u32, ctx.f1.f64, result)) [[likely]] {
      ctx.f1.f64 = result;
      return;
    }
    original(ctx, base);
    return;
  }
  RunCrtMathSlow(Function::kCos, ctx, base, original, build, {});
}
inline void CrtAsin(PPCContext& ctx, uint8_t* base, GuestFunction original, const any360::xdk::CrtAsinBuild& build) {
  ctx.fpscr.disableFlushMode();
  if (g_mode == Mode::kNative) [[likely]] {
    double result;
    if (any360::xdk::CrtAsin(g_memory, build, ctx.f1.f64, result)) [[likely]] {
      ctx.f1.f64 = result;
      return;
    }
    original(ctx, base);
    return;
  }
  RunCrtMathSlow(Function::kAsin, ctx, base, original, {}, build);
}
// atan2 (f1 = y, f2 = x) and pow (f1 = x, f2 = y): f1 the result; both leave their two arguments in the caller's
// parameter words (r1 + 16 and r1 + 24), as the functions store them.
inline void CrtAtan2(PPCContext& ctx, uint8_t* base, GuestFunction original, const any360::xdk::CrtAtan2Build& build) {
  ctx.fpscr.disableFlushMode();
  if (g_mode == Mode::kNative) [[likely]] {
    double result;
    if (any360::xdk::CrtAtan2(g_memory, build, ctx.r1.u32, ctx.f1.f64, ctx.f2.f64, result)) [[likely]] {
      ctx.f1.f64 = result;
      return;
    }
    original(ctx, base);
    return;
  }
  RunCrtMath2Slow(Function::kAtan2, ctx, base, original, build, {});
}
inline void CrtPow(PPCContext& ctx, uint8_t* base, GuestFunction original, const any360::xdk::CrtPowBuild& build) {
  ctx.fpscr.disableFlushMode();
  if (g_mode == Mode::kNative) [[likely]] {
    double result;
    if (any360::xdk::CrtPow(g_memory, build, ctx.r1.u32, ctx.f1.f64, ctx.f2.f64, result)) [[likely]] {
      ctx.f1.f64 = result;
      return;
    }
    original(ctx, base);
    return;
  }
  RunCrtMath2Slow(Function::kPow, ctx, base, original, {}, build);
}

// The D3D runtime's GPU wait step (any360/xdk/d3d.h): r3 the wait's state; returns nonzero while the wait goes on.
// Its own code runs; then, while the graphics are bound to Any360 and any360_d3d_wait_sleep is on, the thread sleeps
// until the GPU's progress count moves (or any360_d3d_wait_longest_us passes) instead of going straight back to its
// caller's look. Not bound: the step is the function's own code alone (the runtime's GPU has its own progress).
struct D3dWaitSettings {
  bool sleeps = false;
  uint32_t longest_us = 0;
};
const D3dWaitSettings& D3dWait();
// Steps run, slept, and slept to their time-out: counted, logged every few seconds.
void CountD3dWait(bool slept, bool moved);
inline void D3dGpuWaitStep(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  if (!ctx.r3.u32) return;
  const D3dWaitSettings& settings = D3dWait();
  if (!settings.sleeps || !GraphicsBound()) {
    CountD3dWait(false, false);
    return;
  }
  struct Gpu {
    uint64_t Progress() { return GpuProgress(); }
    void WaitProgress(uint64_t seen, std::chrono::microseconds longest) { WaitGpuProgress(seen, uint32_t(longest.count())); }
  } gpu;
  static thread_local any360::xdk::D3dGpuWait wait;
  const bool moved = wait.Sleep(gpu, std::chrono::microseconds(settings.longest_us));
  CountD3dWait(true, moved);
}

// The D3D runtime's state setters (any360/xdk/d3d_state.h), one kind each: r3 the device. `Kind` has Native (the
// replacement on the registers), Writes (the device bytes, sources and stack it may write) and kFunction. The
// device's layout (g_d3d_layout) is a constant, so that its offsets compile to immediates as the runtime's own do: the
// default layout, the runtime Crackdown links (a title with another runtime gets kinds on its own layout).
inline constexpr any360::xdk::D3dLayout g_d3d_layout{};
// A kind whose function returns nothing (its r3 left as its code last used it) says so with kVoid.
template <class Kind>
constexpr bool KeepsR3() {
  if constexpr (requires { Kind::kVoid; }) return !Kind::kVoid;
  return true;
}
template <class Kind>
inline void D3dState(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (g_mode == Mode::kNative) [[likely]] {
    if (Kind::Native(g_memory, ctx)) [[likely]] return;
    original(ctx, base);
    return;
  }
  RunD3dStateSlow(Kind::kFunction, ctx, base, original, &Kind::Native, &Kind::Writes, KeepsR3<Kind>());
}
namespace d3d_state {
inline void AddRange(std::vector<any360::xdk::Range>& w, uint32_t at, uint32_t size) { w.push_back({at, size}); }
// The blend setters: the blend word and flags, the render targets' words, the dirty bits.
inline bool BlendWrites(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
  const any360::xdk::D3dLayout& l = g_d3d_layout;
  const uint32_t device = ctx.r3.u32;
  if (!any360::xdk::D3dBlendAccepts(l, device)) return false;
  AddRange(w, device + l.blend_control, 4);
  AddRange(w, device + l.blend_flags, 4);
  for (uint32_t t : l.target_blend) AddRange(w, device + t, 4);
  AddRange(w, device + l.state_dirty, 8);
  return true;
}
template <any360::xdk::D3dBlendField kField>
struct BlendField {
  static constexpr Function kFunction = Function::kD3dBlend;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetBlendField(m, g_d3d_layout, ctx.r3.u32, kField, ctx.r4.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) { return BlendWrites(ctx, w); }
};
struct BlendEnable {
  static constexpr Function kFunction = Function::kD3dBlend;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetBlendEnable(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) { return BlendWrites(ctx, w); }
};
struct SeparateAlphaBlend {
  static constexpr Function kFunction = Function::kD3dBlend;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetSeparateAlphaBlend(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) { return BlendWrites(ctx, w); }
};
// r4 the first register, r5 the source, r6 the count.
template <bool kPixel>
struct BoolConstants {
  static constexpr Function kFunction = Function::kD3dBoolConstants;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetBoolConstants(m, g_d3d_layout, ctx.r3.u32, kPixel, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    any360::xdk::Range words;
    if (!any360::xdk::D3dBoolConstantsAccepts(g_d3d_layout, ctx.r3.u32, kPixel, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                              words))
      return false;
    w.push_back(words);
    AddRange(w, ctx.r3.u32 + g_d3d_layout.constant_dirty, 8);
    return true;
  }
};
template <bool kPixel>
struct IntConstants {
  static constexpr Function kFunction = Function::kD3dIntConstants;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetIntConstants(m, g_d3d_layout, ctx.r3.u32, kPixel, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    any360::xdk::Range words;
    if (!any360::xdk::D3dIntConstantsAccepts(g_d3d_layout, ctx.r3.u32, kPixel, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                             words))
      return false;
    w.push_back(words);
    AddRange(w, ctx.r3.u32 + g_d3d_layout.constant_dirty, 8);
    return true;
  }
};
// SetPixelShaderConstantF: r4 the first register, r5 the constants, r6 how many, r7 the dirty bits.
struct PixelConstants {
  static constexpr Function kFunction = Function::kD3dPixelConstants;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetPixelShaderConstants(m, g_d3d_layout, ctx.r3.u64, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                                   ctx.r7.u64, ctx.r1.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    any360::xdk::Range constants;
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    if (!any360::xdk::D3dSetShaderConstantsAccepts(ctx.r3.u64, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32, ctx.r1.u32,
                                                   constants, l.float_constant_base[1], l.float_dirty[1]))
      return false;
    w.push_back(constants);
    AddRange(w, ctx.r3.u32 + l.float_dirty[1], 8);
    AddRange(w, ctx.r1.u32 + 48, 8);
    return true;
  }
};
// The resource binders: what they may write, from the same plan as the replacement's.
inline bool BinderWrites(const PPCContext& ctx, std::vector<any360::xdk::Range>& w, uint32_t slot, uint32_t stack_word) {
  const any360::xdk::D3dLayout& l = g_d3d_layout;
  const uint32_t device = ctx.r3.u32;
  const uint8_t* d = g_memory.Host(device);
  any360::xdk::detail::Release r;
  if (!any360::xdk::detail::PlanRelease(g_memory, l, d, device, any360::xdk::detail::At32(d + slot), stack_word, r))
    return false;
  AddRange(w, device + slot, 4);
  AddRange(w, device + l.fetch_dirty, 8);
  AddRange(w, stack_word, 8);
  if (r.kind == any360::xdk::detail::Release::kFence) AddRange(w, r.old + 8, 4);
  if (r.kind == any360::xdk::detail::Release::kRecord) AddRange(w, r.record, 8), AddRange(w, device + l.release_cursor, 4);
  return true;
}
// SetRenderState_ViewportEnable: r4 the value.
struct ViewportEnable {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetViewportEnable(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32;
    if (!any360::xdk::IsPlainMemory(device, l.vte_control + 4)) return false;
    AddRange(w, device + l.vte_control, 4);
    AddRange(w, device + l.clip_control, 4);
    AddRange(w, device + l.state_dirty, 8);
    return true;
  }
};
// SetRenderState_DepthBias: r4 the bias's float bits, kept at the caller's r1 + 28; the host's denormal flush turned
// off first, as the runtime's code does (its lfs).
struct DepthBias {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    ctx.fpscr.disableFlushMode();
    return any360::xdk::D3dSetDepthBias(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32 + 28);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32;
    if (!any360::xdk::IsPlainMemory(device, l.poly_offset + 16) || !any360::xdk::IsPlainMemory(ctx.r1.u32 + 28, 4)) return false;
    AddRange(w, ctx.r1.u32 + 28, 4);
    AddRange(w, device + l.poly_offset + 4, 4);
    AddRange(w, device + l.poly_offset + 12, 4);
    AddRange(w, device + l.mode_control, 4);
    AddRange(w, device + l.constant_dirty, 8);
    AddRange(w, device + l.state_dirty, 8);
    return true;
  }
};
// The runtime's function SetRenderTarget and SetDepthStencilSurface call (the title sets it): r3 the device, r4 a
// surface: the viewport and scissor set to the surface, their packets written (Crackdown sub_826AF2D8).
inline GuestFunction g_surface_viewport = nullptr;
// That function as the setters' bl runs it: r1 their frame (112 bytes), r3 the device, r4 the surface.
struct SurfaceViewport {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t device;
  void operator()(uint32_t surface) const {
    const uint64_t r1 = ctx.r1.u64;
    ctx.r1.u64 = r1 - 112;
    ctx.r3.u64 = device;
    ctx.r4.u64 = surface;
    g_surface_viewport(ctx, base);
    ctx.r1.u64 = r1;
  }
};
// What the two setters write: their device fields, the viewport function's (Crackdown's runtime: SURFACE_INFO, +10308,
// the viewport's floats at +10376, +10556, the viewport and scissor as set at +12368, the command buffer's cursor)
// and, for a render target, the surface's word 28. Only those: the device's other fields are other threads' too (put
// back when verified, their writes would be lost). The function's packets go past the cursor (the same on both sides
// when verified). Declined when it could make room for them on the way (one header and a packet a tile, 8 words each,
// past its end): run twice when verified, that would send twice.
inline constexpr uint32_t kViewportTiles = 12468;  // the tiling's tile count (the viewport function's loop)
inline constexpr any360::xdk::Range kViewportWrites[] = {{10240, 4}, {10308, 8}, {10376, 24}, {10556, 4}, {12368, 44}};
inline bool SurfaceWrites(const PPCContext& ctx, uint32_t surface, std::vector<any360::xdk::Range>& w) {
  const any360::xdk::D3dLayout& l = g_d3d_layout;
  const uint32_t device = ctx.r3.u32;
  if (!g_surface_viewport || !any360::xdk::detail::SurfaceSetterAccepts(l, device, surface)) return false;
  const any360::xdk::Memory& m = g_memory;
  const uint32_t cursor = any360::xdk::detail::Get32(m, device + l.command_cursor);
  const uint32_t end = any360::xdk::detail::Get32(m, device + l.command_end);
  const uint32_t tiles = any360::xdk::detail::Get32(m, device + kViewportTiles);
  if (tiles > 64 || cursor > end || end - cursor < 4 * 8 * (tiles + 4)) return false;
  for (const any360::xdk::Range& r : kViewportWrites) AddRange(w, device + r.at, r.size);
  AddRange(w, device + l.command_cursor, 4);
  AddRange(w, device + l.state_dirty, 8);
  AddRange(w, device + l.constant_dirty, 8);
  AddRange(w, device + l.surfaces, 20);
  AddRange(w, device + l.color_info, 20);
  AddRange(w, device + l.color_mask, 4);
  AddRange(w, device + l.depth_control, 4);
  AddRange(w, device + l.depth_word, 4);
  AddRange(w, device + l.tiling_flags, 1);
  AddRange(w, device + l.depth_flags, 1);
  return true;
}
// SetRenderTarget: r4 the target (0-3), r5 the surface.
struct SetRenderTarget {
  static constexpr Function kFunction = Function::kD3dSurfaces;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    if (!g_surface_viewport) return false;
    const uint32_t device = ctx.r3.u32;
    return any360::xdk::D3dSetRenderTarget(m, g_d3d_layout, device, ctx.r4.u32, ctx.r5.u32,
                                           SurfaceViewport{ctx, m.base, device});
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    if (ctx.r4.u32 > 3 || !SurfaceWrites(ctx, ctx.r5.u32, w)) return false;
    if (ctx.r5.u32) AddRange(w, ctx.r5.u32 + 28, 4);
    return true;
  }
};
// SetDepthStencilSurface: r4 the surface.
struct SetDepthStencilSurface {
  static constexpr Function kFunction = Function::kD3dSurfaces;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    if (!g_surface_viewport) return false;
    const uint32_t device = ctx.r3.u32;
    return any360::xdk::D3dSetDepthStencilSurface(m, g_d3d_layout, device, ctx.r4.u32,
                                                  SurfaceViewport{ctx, m.base, device});
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    return SurfaceWrites(ctx, ctx.r4.u32, w);
  }
};
// The render state fields' setters (any360/xdk/d3d_state.h): r4 the value; each writes its fields and the state's
// dirty bits.
struct DeviceField {
  uint32_t any360::xdk::D3dLayout::*field;
  uint32_t size;
};
template <bool (*kSet)(const any360::xdk::Memory&, const any360::xdk::D3dLayout&, uint32_t, uint32_t),
          DeviceField... kFields>
struct RenderStateField {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) { return kSet(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32); }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32;
    if (!any360::xdk::detail::DeviceAccepts(l, device)) return false;
    (AddRange(w, device + l.*(kFields.field), kFields.size), ...);
    AddRange(w, device + l.state_dirty, 8);
    return true;
  }
};
using D3dLayout_ = any360::xdk::D3dLayout;
using CullMode = RenderStateField<any360::xdk::D3dSetCullMode, DeviceField{&D3dLayout_::mode_control, 4}>;
using AlphaTestEnable = RenderStateField<any360::xdk::D3dSetAlphaTestEnable, DeviceField{&D3dLayout_::color_control, 4}>;
using AlphaFunc = RenderStateField<any360::xdk::D3dSetAlphaFunc, DeviceField{&D3dLayout_::color_control, 4}>;
using ZEnable = RenderStateField<any360::xdk::D3dSetZEnable, DeviceField{&D3dLayout_::depth_enable_set, 4},
                                 DeviceField{&D3dLayout_::depth_control, 4}>;
using ZWriteEnable = RenderStateField<any360::xdk::D3dSetZWriteEnable, DeviceField{&D3dLayout_::depth_control, 4}>;
using ClipPlaneEnable = RenderStateField<any360::xdk::D3dSetClipPlaneEnable, DeviceField{&D3dLayout_::clip_plane_flag, 4},
                                         DeviceField{&D3dLayout_::clip_control, 4}>;
using SetIndices = RenderStateField<any360::xdk::D3dSetIndices, DeviceField{&D3dLayout_::index_buffer, 4}>;
// SetRenderState_BlendFactor: r4 the color; the host's denormal flush turned off first, as the runtime's code does.
// (Its doubles through the stack below r1 are its own scratch, not written here.)
struct BlendFactor {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    ctx.fpscr.disableFlushMode();
    return any360::xdk::D3dSetBlendFactor(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32;
    if (!any360::xdk::detail::DeviceAccepts(l, device) || !any360::xdk::IsPlainMemory(l.blend_byte_scale, 4)) return false;
    AddRange(w, device + l.blend_color, 16);
    AddRange(w, device + l.state_dirty, 8);
    return true;
  }
};
// SetRenderState_SlopeScaleDepthBias: r4 the scale's float bits, kept at the caller's r1 + 28; denormals kept.
struct SlopeScaleDepthBias {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    ctx.fpscr.disableFlushMode();
    return any360::xdk::D3dSetSlopeScaleDepthBias(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32 + 28);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32, stack_word = ctx.r1.u32 + 28;
    if (!any360::xdk::detail::DeviceAccepts(l, device) || !any360::xdk::IsPlainMemory(stack_word, 4) ||
        !any360::xdk::IsPlainMemory(l.zero_float, 4) || !any360::xdk::IsPlainMemory(l.slope_scale_factor, 4) ||
        any360::xdk::detail::Overlap(stack_word, 4, device, l.device_bytes) ||
        any360::xdk::detail::Overlap(stack_word, 4, l.zero_float, 4) ||
        any360::xdk::detail::Overlap(stack_word, 4, l.slope_scale_factor, 4))
      return false;
    AddRange(w, stack_word, 4);
    AddRange(w, device + l.poly_offset, 4);
    AddRange(w, device + l.poly_offset + 8, 4);
    AddRange(w, device + l.mode_control, 4);
    AddRange(w, device + l.constant_dirty, 8);
    AddRange(w, device + l.state_dirty, 8);
    return true;
  }
};
// SetClipPlane: r4 the plane's index (0-5), r5 its four floats.
struct SetClipPlane {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetClipPlane(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32, plane = ctx.r5.u32;
    if (ctx.r4.u32 > 5 || !any360::xdk::detail::DeviceAccepts(l, device) || !any360::xdk::IsPlainMemory(plane, 16) ||
        any360::xdk::detail::Overlap(plane, 16, device, l.device_bytes))
      return false;
    AddRange(w, device + l.clip_planes + 16 * ctx.r4.u32, 16);
    AddRange(w, device + l.constant_dirty, 8);
    return true;
  }
};
// The render state getters: r3 the value.
template <any360::xdk::D3dRenderStateRead kState>
struct GetRenderState {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    uint32_t value;
    if (!any360::xdk::D3dGetRenderState(m, g_d3d_layout, ctx.r3.u32, kState, value)) return false;
    ctx.r3.u64 = value;
    return true;
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>&) {
    return any360::xdk::detail::DeviceAccepts(g_d3d_layout, ctx.r3.u32);
  }
};
// GetRenderState_BlendFactor: r3 the color; denormals kept. (Its words through the stack below r1 are its scratch.)
struct GetBlendFactor {
  static constexpr Function kFunction = Function::kD3dRenderState;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    ctx.fpscr.disableFlushMode();
    uint32_t color;
    if (!any360::xdk::D3dGetBlendFactor(m, g_d3d_layout, ctx.r3.u32, color)) return false;
    ctx.r3.u64 = color;
    return true;
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>&) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    return any360::xdk::detail::DeviceAccepts(l, ctx.r3.u32) && any360::xdk::IsPlainMemory(l.blend_float_scale, 4) &&
           any360::xdk::IsPlainMemory(l.blend_rounding, 4);
  }
};
// SetSamplerState for a stage's anisotropy: r4 the stage, r5 the table index.
struct SamplerAnisotropy {
  static constexpr Function kFunction = Function::kD3dSamplerFilter;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetSamplerAnisotropy(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32, stage = ctx.r4.u32;
    if (stage >= 32 || !any360::xdk::detail::DeviceAccepts(l, device)) return false;
    const uint32_t f = device + l.fetch_constants + 24 * stage;
    if (any360::xdk::detail::Get32(g_memory, f + 16) & 0xC00u) {
      const uint32_t table_at = l.sampler_filter_table + ((ctx.r5.u32 << 2) & 0xFFFFFFFCu);
      if (!any360::xdk::IsPlainMemory(table_at, 4) || any360::xdk::detail::Overlap(table_at, 4, device, l.device_bytes))
        return false;
    }
    AddRange(w, f + 12, 4);
    AddRange(w, device + l.fetch_dirty, 8);
    AddRange(w, device + l.stage_filter_index + stage, 1);
    return true;
  }
};
// SetSamplerState for a flag of the stage's LOD byte (`kBit` 1 or 0): r4 the stage, r5 the value.
template <uint32_t kBit>
struct SamplerLodFlag {
  static constexpr Function kFunction = Function::kD3dSamplerFilter;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetSamplerLodFlag(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kBit);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t device = ctx.r3.u32, stage = ctx.r4.u32;
    if (stage >= 32 || !any360::xdk::detail::DeviceAccepts(l, device)) return false;
    AddRange(w, device + l.fetch_constants + 24 * stage + 16, 4);
    AddRange(w, device + l.fetch_dirty, 8);
    AddRange(w, device + l.stage_lod_limit + stage, 1);
    return true;
  }
};
// SetTexture: r4 the stage, r5 the texture, r6 the dirty bits.
struct SetTexture {
  static constexpr Function kFunction = Function::kD3dSetTexture;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetTexture(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u64, ctx.r1.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t stage = ctx.r4.u32, texture = ctx.r5.u32;
    if (stage >= 32 || !any360::xdk::IsPlainMemory(ctx.r3.u32, l.device_bytes)) return false;
    if (texture && (!any360::xdk::IsPlainMemory(texture + 28, 24) ||
                    any360::xdk::detail::Overlap(texture + 28, 24, ctx.r3.u32, l.device_bytes)))
      return false;
    AddRange(w, ctx.r3.u32 + l.fetch_constants + 24 * stage, 24);
    return BinderWrites(ctx, w, l.texture_bound + 4 * stage, ctx.r1.u32 - 80);
  }
};
// SetStreamSource: r4 the stream, r5 the buffer, r6 the offset, r7 the stride, r8 the dirty bits.
struct SetStreamSource {
  static constexpr Function kFunction = Function::kD3dSetStreamSource;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetStreamSource(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                                           ctx.r8.u64, ctx.r1.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t stream = ctx.r4.u32, buffer = ctx.r5.u32;
    if (stream > 15 || !any360::xdk::IsPlainMemory(ctx.r3.u32, l.device_bytes)) return false;
    if (buffer && (!any360::xdk::IsPlainMemory(buffer + 24, 8) ||
                   any360::xdk::detail::Overlap(buffer + 24, 8, ctx.r3.u32, l.device_bytes)))
      return false;
    AddRange(w, ctx.r3.u32 + l.vertex_fetch_last - 8 * stream, 8);
    AddRange(w, ctx.r3.u32 + l.stream_stride + stream, 1);
    AddRange(w, ctx.r3.u32 + l.state_dirty, 8);
    return BinderWrites(ctx, w, l.stream_bound + 4 * stream, ctx.r1.u32 - 64);
  }
};
// SetSamplerState for a stage's minification or magnification filter: r4 the stage, r5 the filter.
template <bool kMagnification>
struct SamplerFilter {
  static constexpr Function kFunction = Function::kD3dSamplerFilter;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetSamplerFilter(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kMagnification);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    if (ctx.r4.u32 >= 32 || !any360::xdk::IsPlainMemory(ctx.r3.u32, l.device_bytes)) return false;
    AddRange(w, ctx.r3.u32 + l.fetch_constants + 24 * ctx.r4.u32, 24);
    AddRange(w, ctx.r3.u32 + l.fetch_dirty, 8);
    return true;
  }
};
// SetVertexDeclaration: r4 the declaration.
struct SetVertexDeclaration {
  static constexpr Function kFunction = Function::kD3dSetVertexDeclaration;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetVertexDeclaration(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    if (!any360::xdk::IsPlainMemory(ctx.r3.u32, g_d3d_layout.device_bytes)) return false;
    return BinderWrites(ctx, w, g_d3d_layout.vertex_declaration, ctx.r1.u32 - 48);
  }
};
// SetPixelShader / SetVertexShader: r4 the shader. Verify compares the device words it sets and the fetch
// constants its program writes.
template <bool kVertex>
struct SetShader {
  static constexpr Function kFunction = Function::kD3dSetShader;
  static constexpr bool kVoid = true;
  static bool Native(const any360::xdk::Memory& m, PPCContext& ctx) {
    return any360::xdk::D3dSetShader(m, g_d3d_layout, ctx.r3.u32, ctx.r4.u32, kVertex, ctx.r1.u32);
  }
  static bool Writes(const PPCContext& ctx, std::vector<any360::xdk::Range>& w) {
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    if (!any360::xdk::IsPlainMemory(ctx.r3.u32, l.device_bytes)) return false;
    any360::xdk::detail::ShaderProgram program;
    bool has_program = false;
    if (!any360::xdk::D3dShaderProgramOf(g_memory, l, ctx.r3.u32, ctx.r4.u32, kVertex, program, has_program)) return false;
    if (!BinderWrites(ctx, w, kVertex ? l.vertex_shader : l.pixel_shader, ctx.r1.u32 - 48)) return false;
    AddRange(w, ctx.r3.u32 + l.state_dirty, 8);
    AddRange(w, ctx.r3.u32 + l.float_dirty[kVertex ? 0 : 1], 8);
    AddRange(w, ctx.r3.u32 + l.constant_dirty, 8);
    AddRange(w, ctx.r3.u32 + l.shader_flags, 1);
    if (has_program && program.write_low < program.write_high)
      AddRange(w, ctx.r3.u32 + l.fetch_constants + program.write_low, program.write_high - program.write_low);
    return true;
  }
};
// The runtime's CPU range write-back (dcbf, sync): nothing in recompiled code.
struct FlushCpuRange {
  static constexpr Function kFunction = Function::kD3dFlushCpuRange;
  static bool Native(const any360::xdk::Memory&, PPCContext&) { return any360::xdk::D3dFlushCpuRange(); }
  static bool Writes(const PPCContext&, std::vector<any360::xdk::Range>&) { return true; }
};
}  // namespace d3d_state

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
