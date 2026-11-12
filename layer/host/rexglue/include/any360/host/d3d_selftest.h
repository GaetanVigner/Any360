// The D3D self-test: a title's D3D runtime functions run against their native replacements on a copy of the
// title's own device, without playing. Set any360_d3d_selftest: from the any360_d3d_selftest_after-th D3D call that
// names the device (its first menu frames: the runtime's ring is up), every registered case runs `calls` times both
// ways on generated inputs: the original (the title's recompiled function) and the replacement each start from the
// same memory and registers; what each leaves in the device copy, in the case's scratch memory and on the stack is
// compared, then both are timed. The work goes in slices of any360_d3d_selftest_slice_ms a frame (the title's
// other threads keep running). The results go to the log and to d3d_selftest.txt in the working folder, and the
// process ends.
//
// A title registers its cases with its own addresses (ANY360_D3D_CASE in the title's binding); the cases' inputs and
// the replacements are Any360's (docs/D3D.md).
#pragma once

#include <cstdint>
#include <random>

#include "any360/xdk/crt.h"
#include "rexglue.h"

namespace any360::host::rexglue::d3d {

// What a case gets to build one call.
struct CallSetup {
  uint8_t* base = nullptr;
  uint32_t device = 0;           // a copy of the title's device (its fields, pointers to the title's memory)
  uint32_t scratch = 0;          // guest memory for the case (kScratchBytes), random bytes before each call
  uint32_t stack = 0;            // the call's stack pointer (free below it, the 4 KB above it compared)
  std::mt19937_64* rng = nullptr;
  uint8_t* Host(uint32_t address) const;
};
constexpr uint32_t kScratchBytes = 256u << 10;
// The device's bytes compared and put back (the runtime's device is under 16 KB in the titles read).
constexpr uint32_t kDeviceBytes = 32u << 10;

struct Case {
  const char* name = nullptr;
  GuestFunction original = nullptr;
  // The replacement on these registers; false: it declined the call (counted, not compared).
  bool (*native)(PPCContext& ctx, uint8_t* base) = nullptr;
  // The registers of one call, and what it reads in memory (the scratch, device fields: done on the device as it is,
  // put back after the case).
  void (*setup)(CallSetup& s, PPCContext& ctx) = nullptr;
  // The first register the two sides leave different, or null (default: r3 only).
  const char* (*differs)(const PPCContext& a, const PPCContext& b) = nullptr;
  uint32_t calls = 3000;
};

// Guest memory as the self-test sees it (set when it starts): the cases' replacements run on it, inline, as the
// title's bindings run on theirs.
inline any360::xdk::Memory g_selftest_memory;
inline const any360::xdk::Memory& SelfTestMemory() { return g_selftest_memory; }

// Registers a case (from a static initializer).
bool Register(const Case& c);

// A D3D call that names the device (`ctx` the calling thread's registers: the cases start from them, r13 and the
// floating-point state included). With any360_d3d_selftest set, the self-test starts at the
// any360_d3d_selftest_after-th one (the first calls come while the runtime sets the device up) and runs a slice at
// most once a frame after that. Otherwise one test of a negative number.
void FirstDevice(const PPCContext& ctx, uint8_t* base, uint32_t device);
inline int64_t g_device_calls_left = 1;  // > 0: calls to wait; 0: running; < 0: off or done
inline void SawDevice(const PPCContext& ctx, uint8_t* base, uint32_t device) {
  if (g_device_calls_left >= 0) [[unlikely]] {
    if (g_device_calls_left == 0 || --g_device_calls_left == 0) FirstDevice(ctx, base, device);
  }
}

}  // namespace any360::host::rexglue::d3d

// A case `id` of the title's function `original_function` (its recompiled code), with the replacement and inputs
// of the kind (a type in any360::host::rexglue::d3d with static Native, Setup and Differs: d3d_cases.h).
#define ANY360_D3D_CASE(id, name_text, original_function, ...)                                           \
  [[maybe_unused]] static const bool any360_d3d_case_##id = ::any360::host::rexglue::d3d::Register(       \
      {name_text, &original_function, &::any360::host::rexglue::d3d::__VA_ARGS__::Native,               \
       &::any360::host::rexglue::d3d::__VA_ARGS__::Setup, ::any360::host::rexglue::d3d::__VA_ARGS__::Differs})
