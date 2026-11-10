#include "any360/host/xdk_natives.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <bit>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_BOOL(any360_xdk_natives, true, "Kernel",
                    "Any360: the library functions a title binds (the CRT's memcpy and memset) run as host routines; "
                    "off: their own recompiled code");
REXCVAR_DEFINE_BOOL(any360_xdk_verify, false, "Kernel",
                    "Any360: every call of a bound library function runs its own code too and compares the results "
                    "(slow; logs differences and keeps the function's own result)");

namespace any360::host::rexglue::xdk {
namespace {

std::atomic<uint64_t> g_native{0}, g_original{0}, g_verified{0}, g_different{0};
// Verified calls of each replacement, in the order of kNames.
enum { kMemcpy, kCopyForward, kMemset, kCount };
constexpr const char* kNames[kCount] = {"memcpy", "forward copy", "memset"};
std::atomic<uint64_t> g_verified_by[kCount] = {};

uint8_t* Host(uint8_t* base, uint32_t address) { return rex::memory::GuestPtr<uint8_t*>(base, address); }

// One host pointer covers the range (the physical view from 0xE0000000 sits at an offset on some hosts), and none
// of it is the GPU's register window, which plain stores would bypass.
bool IsPlainMemory(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000ull) &&
         (end <= 0x7F000000ull || address >= 0x80000000u);
}

// A forward copy gives what memmove gives unless the destination starts inside the source, where it repeats the
// bytes it has just copied.
bool IsPlainForwardCopy(uint32_t dest, uint32_t source, uint32_t size) {
  return dest <= source || dest - source >= size;
}

// The replacement of one call: `native` does the work on ctx. With verification on, the function's own code runs
// first on a copy of the registers; what it wrote in [dest, dest + size) and in the 8 bytes below the stack pointer,
// and the r3 it returned, are then compared with the replacement's.
template <typename Native>
void Run(int which, PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t dest, uint32_t size,
         Native native) {
  constexpr uint32_t kMostVerified = 4u << 20;
  if (!REXCVAR_GET(any360_xdk_verify) || size > kMostVerified) {
    native();
    g_native.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint8_t* const target = Host(base, dest);
  uint8_t* const slot = Host(base, ctx.r1.u32 - 8);
  const uint32_t source = ctx.r4.u32;
  std::vector<uint8_t> before(target, target + size);
  uint8_t slot_before[8];
  std::memcpy(slot_before, slot, 8);
  PPCContext reference = ctx;
  original(reference, base);
  std::vector<uint8_t> expected(target, target + size);
  uint8_t slot_expected[8];
  std::memcpy(slot_expected, slot, 8);
  std::memcpy(target, before.data(), size);
  std::memcpy(slot, slot_before, 8);
  native();
  g_native.fetch_add(1, std::memory_order_relaxed);
  g_verified_by[which].fetch_add(1, std::memory_order_relaxed);
  const uint64_t verified = g_verified.fetch_add(1, std::memory_order_relaxed) + 1;
  if (verified == 1000 || verified % 100000 == 0) {
    REXLOG_INFO("Any360 xdk: {} calls verified against the functions' own code ({} {}, {} {}, {} {}), {} different; "
                "{} handed to it",
                verified, kNames[0], g_verified_by[0].load(), kNames[1], g_verified_by[1].load(), kNames[2],
                g_verified_by[2].load(), g_different.load(), g_original.load());
  }
  const bool same_bytes = std::memcmp(target, expected.data(), size) == 0;
  const bool same_slot = std::memcmp(slot, slot_expected, 8) == 0;
  if (same_bytes && same_slot && ctx.r3.u32 == reference.r3.u32) return;
  if (g_different.fetch_add(1, std::memory_order_relaxed) < 20) {
    REXLOG_ERROR("Any360 xdk: {} differs from the function's own code: dest {:08X} source/fill {:08X} size {}: {}{}{}",
                 kNames[which], dest, source, size, same_bytes ? "" : "bytes written ", same_slot ? "" : "stack slot ",
                 ctx.r3.u32 == reference.r3.u32 ? "" : "r3");
  }
  // The function's own result stands.
  std::memcpy(target, expected.data(), size);
  std::memcpy(slot, slot_expected, 8);
  ctx.r3.u64 = reference.r3.u64;
}

void Original(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  g_original.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void CrtMemcpy(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
  if (!REXCVAR_GET(any360_xdk_natives) || !IsPlainForwardCopy(dest, source, size) || !IsPlainMemory(dest, size) ||
      !IsPlainMemory(source, size)) {
    Original(ctx, base, original);
    return;
  }
  Run(kMemcpy, ctx, base, original, dest, size, [&] {
    // std r3,-8(r1): the function's own save of the destination.
    const uint64_t saved = std::byteswap(ctx.r3.u64);
    std::memcpy(Host(base, ctx.r1.u32 - 8), &saved, sizeof(saved));
    std::memmove(Host(base, dest), Host(base, source), size);
  });
}

void CrtCopyForward(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
  if (!REXCVAR_GET(any360_xdk_natives) || !IsPlainForwardCopy(dest, source, size) || !IsPlainMemory(dest, size) ||
      !IsPlainMemory(source, size)) {
    Original(ctx, base, original);
    return;
  }
  Run(kCopyForward, ctx, base, original, dest, size,
      [&] { std::memmove(Host(base, dest), Host(base, source), size); });
}

void CrtMemset(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t dest = ctx.r3.u32, size = ctx.r5.u32;
  if (!REXCVAR_GET(any360_xdk_natives) || !IsPlainMemory(dest, size)) {
    Original(ctx, base, original);
    return;
  }
  Run(kMemset, ctx, base, original, dest, size,
      [&] { std::memset(Host(base, dest), int(ctx.r4.u32 & 0xFF), size); });
}

Counters GetCounters() {
  return {g_native.load(), g_original.load(), g_verified.load(), g_different.load()};
}

}  // namespace any360::host::rexglue::xdk
