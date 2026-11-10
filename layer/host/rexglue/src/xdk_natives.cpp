#include "any360/host/xdk_natives.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define ANY360_XDK_TSC 1
#endif

REXCVAR_DEFINE_BOOL(any360_xdk_natives, true, "Kernel",
                    "Any360: the library functions a title binds (the CRT's memcpy and memset) run as host routines; "
                    "off: their own recompiled code");
REXCVAR_DEFINE_BOOL(any360_xdk_verify, false, "Kernel",
                    "Any360: every call of a bound library function runs its own code too and compares the results "
                    "(slow; logs differences and keeps the function's own result)");
REXCVAR_DEFINE_BOOL(any360_xdk_bench, false, "Kernel",
                    "Any360: every call of a bound library function is timed both ways, its own code and the host "
                    "routine; the log gets the calls and nanoseconds of each by size (slow)");

namespace any360::host::rexglue::xdk {

Mode g_mode = Mode::kUnresolved;
any360::xdk::Memory g_memory;

namespace {

constexpr uint32_t kFunctions = uint32_t(Function::kCount);
constexpr const char* kNames[kFunctions] = {"memcpy", "forward copy", "memset"};
// Calls larger than this are not verified or timed (their snapshot would cost more than they tell).
constexpr uint32_t kMostChecked = 4u << 20;

std::atomic<uint64_t> g_verified{0}, g_different{0}, g_declined{0};
std::atomic<uint64_t> g_verified_by[kFunctions] = {};

// Timed calls by function and size class (class c: sizes with bit_width c, so 0, 1, 2-3, 4-7, ...).
struct Timed {
  std::atomic<uint64_t> calls{0}, original{0}, native{0};
};
Timed g_timed[kFunctions][33];
// Calls by exact size below kExactSizes: which sizes a title really passes (the benchmark's mix is made of them).
constexpr uint32_t kExactSizes = 256;
std::atomic<uint32_t> g_exact[kFunctions][kExactSizes] = {};
std::atomic<uint64_t> g_timed_calls{0};
uint64_t g_ticks_start = 0, g_timer_overhead = 0;
std::chrono::steady_clock::time_point g_clock_start;

uint64_t Ticks() {
#ifdef ANY360_XDK_TSC
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

Mode Resolve(uint8_t* base) {
  // The physical view's host offset: what the runtime adds for guest addresses from 0xE0000000.
  g_memory.base = base;
  g_memory.physical_offset = uint32_t(rex::memory::GuestPtr<uint8_t*>(base, 0xE0000000u) - (base + 0xE0000000u));
  Mode mode = Mode::kNative;
  if (!REXCVAR_GET(any360_xdk_natives)) {
    mode = Mode::kOriginal;
  } else if (REXCVAR_GET(any360_xdk_verify)) {
    mode = Mode::kVerify;
  } else if (REXCVAR_GET(any360_xdk_bench)) {
    mode = Mode::kBench;
    // What reading the clock twice costs, taken off every timed call.
    uint64_t least = UINT64_MAX;
    for (int i = 0; i < 20000; ++i) {
      const uint64_t a = Ticks(), b = Ticks();
      least = std::min(least, b - a);
    }
    g_timer_overhead = least;
    g_ticks_start = Ticks();
    g_clock_start = std::chrono::steady_clock::now();
  }
  REXLOG_INFO("Any360 xdk: bound library functions run {}",
              mode == Mode::kNative     ? "as host routines"
              : mode == Mode::kOriginal ? "their own code (any360_xdk_natives off)"
              : mode == Mode::kVerify   ? "both ways, compared (any360_xdk_verify)"
                                        : "both ways, timed (any360_xdk_bench)");
  g_mode = mode;
  return mode;
}

// `dest_register`: r3 whole (memcpy saves all of it).
bool Native(Function function, uint64_t dest_register, uint32_t second, uint32_t size, uint32_t stack_pointer) {
  switch (function) {
    case Function::kMemcpy: return any360::xdk::CrtMemcpy(g_memory, dest_register, second, size, stack_pointer);
    case Function::kCopyForward: return any360::xdk::CrtCopyForward(g_memory, uint32_t(dest_register), second, size);
    default: return any360::xdk::CrtMemset(g_memory, uint32_t(dest_register), second, size);
  }
}

// The timed calls so far, a line a function: for each size class its calls and the nanoseconds a call of the
// function's own code and of the host routine.
void ReportTimes() {
  const double elapsed_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - g_clock_start).count();
  const double ns_a_tick = elapsed_ns / double(Ticks() - g_ticks_start);
  for (uint32_t f = 0; f < kFunctions; ++f) {
    uint64_t calls = 0, original = 0, native = 0;
    std::string classes;
    for (uint32_t c = 0; c < 33; ++c) {
      const uint64_t n = g_timed[f][c].calls.load();
      if (!n) continue;
      const uint64_t overhead = n * g_timer_overhead;
      const uint64_t o = g_timed[f][c].original.load(), v = g_timed[f][c].native.load();
      const uint64_t o_net = o > overhead ? o - overhead : 0, v_net = v > overhead ? v - overhead : 0;
      calls += n, original += o_net, native += v_net;
      const uint32_t low = c ? 1u << (c - 1) : 0, high = c ? (c == 32 ? UINT32_MAX : (1u << c) - 1) : 0;
      classes += " [" + std::to_string(low) + "-" + std::to_string(high) + ": " + std::to_string(n) + " calls, " +
                 std::to_string(int(double(o_net) * ns_a_tick * 10 / double(n) + 0.5) / 10.0).substr(0, 6) + " -> " +
                 std::to_string(int(double(v_net) * ns_a_tick * 10 / double(n) + 0.5) / 10.0).substr(0, 6) + " ns]";
    }
    if (!calls) continue;
    REXLOG_INFO("Any360 xdk bench: {}: {} calls, own code {:.1f} ns a call, host routine {:.1f} ns ({:.1f}x); by size:{}",
                kNames[f], calls, double(original) * ns_a_tick / double(calls), double(native) * ns_a_tick / double(calls),
                native ? double(original) / double(native) : 0.0, classes);
    // The sizes below kExactSizes that make up most of the calls, most frequent first.
    std::vector<std::pair<uint32_t, uint32_t>> exact;  // calls, size
    for (uint32_t size = 0; size < kExactSizes; ++size) {
      if (const uint32_t n = g_exact[f][size].load()) exact.emplace_back(n, size);
    }
    std::sort(exact.rbegin(), exact.rend());
    std::string sizes;
    for (size_t i = 0; i < exact.size() && i < 24; ++i) {
      sizes += " " + std::to_string(exact[i].second) + " x " + std::to_string(exact[i].first);
    }
    REXLOG_INFO("Any360 xdk bench: {}: most frequent sizes (size x calls):{}", kNames[f], sizes);
  }
}

}  // namespace

void RunSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original) {
  Mode mode = g_mode;
  if (mode == Mode::kUnresolved) mode = Resolve(base);
  const uint64_t dest_register = ctx.r3.u64;
  const uint32_t dest = ctx.r3.u32, second = ctx.r4.u32, size = ctx.r5.u32, stack_pointer = ctx.r1.u32;
  if (mode == Mode::kNative) {
    if (!Native(function, dest_register, second, size, stack_pointer)) original(ctx, base);
    return;
  }
  // Verifying and timing run both sides on the same input: only the calls the replacement accepts, known before
  // anything runs.
  const bool accepts = function == Function::kMemset
                           ? any360::xdk::IsPlainMemory(dest, size)
                           : any360::xdk::IsPlainForwardCopy(dest, second, size) &&
                                 any360::xdk::IsPlainMemory(dest, size) && any360::xdk::IsPlainMemory(second, size);
  if (mode == Mode::kOriginal || !accepts || size > kMostChecked) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint8_t* const target = g_memory.Host(dest);
  uint8_t* const slot = g_memory.Host(stack_pointer - 8);
  const std::vector<uint8_t> before(target, target + size);
  uint8_t slot_before[8];
  std::memcpy(slot_before, slot, 8);
  const uint32_t f = uint32_t(function);

  if (mode == Mode::kVerify) {
    // The function's own code on a copy of the registers; what it wrote in [dest, dest + size) and in the 8 bytes
    // below the stack pointer, and the r3 it returned, against the replacement's.
    PPCContext reference = ctx;
    original(reference, base);
    const std::vector<uint8_t> expected(target, target + size);
    uint8_t slot_expected[8];
    std::memcpy(slot_expected, slot, 8);
    std::memcpy(target, before.data(), size);
    std::memcpy(slot, slot_before, 8);
    Native(function, dest_register, second, size, stack_pointer);
    g_verified_by[f].fetch_add(1, std::memory_order_relaxed);
    const uint64_t verified = g_verified.fetch_add(1, std::memory_order_relaxed) + 1;
    if (verified == 1000 || verified % 1000000 == 0) {
      REXLOG_INFO("Any360 xdk: {} calls verified against the functions' own code ({} {}, {} {}, {} {}), {} different; "
                  "{} left to it",
                  verified, kNames[0], g_verified_by[0].load(), kNames[1], g_verified_by[1].load(), kNames[2],
                  g_verified_by[2].load(), g_different.load(), g_declined.load());
    }
    const bool same_bytes = std::memcmp(target, expected.data(), size) == 0;
    const bool same_slot = std::memcmp(slot, slot_expected, 8) == 0;
    if (same_bytes && same_slot && ctx.r3.u32 == reference.r3.u32) return;
    if (g_different.fetch_add(1, std::memory_order_relaxed) < 20) {
      uint64_t slot_now, slot_wanted;
      std::memcpy(&slot_now, slot, 8);
      std::memcpy(&slot_wanted, slot_expected, 8);
      REXLOG_ERROR("Any360 xdk: {} differs from the function's own code: r3 {:016X} source/fill {:08X} size {}: {}{}{}"
                   "(stack slot {:016X}, its own code {:016X})",
                   kNames[f], dest_register, second, size, same_bytes ? "" : "bytes written ",
                   same_slot ? "" : "stack slot ", ctx.r3.u32 == reference.r3.u32 ? "" : "r3 ",
                   any360::xdk::detail::ByteSwap64(slot_now), any360::xdk::detail::ByteSwap64(slot_wanted));
    }
    // The function's own result stands.
    std::memcpy(target, expected.data(), size);
    std::memcpy(slot, slot_expected, 8);
    ctx.r3.u64 = reference.r3.u64;
    return;
  }

  // Bench: both sides timed on the same input, in alternating order (the second finds the memory in the cache).
  const uint64_t call = g_timed_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t original_ticks, native_ticks;
  PPCContext copy = ctx;
  if (call & 1) {
    const uint64_t a = Ticks();
    original(copy, base);
    const uint64_t b = Ticks();
    std::memcpy(target, before.data(), size);
    std::memcpy(slot, slot_before, 8);
    const uint64_t c = Ticks();
    Native(function, dest_register, second, size, stack_pointer);
    const uint64_t d = Ticks();
    original_ticks = b - a, native_ticks = d - c;
  } else {
    const uint64_t a = Ticks();
    Native(function, dest_register, second, size, stack_pointer);
    const uint64_t b = Ticks();
    std::memcpy(target, before.data(), size);
    std::memcpy(slot, slot_before, 8);
    const uint64_t c = Ticks();
    original(copy, base);
    const uint64_t d = Ticks();
    native_ticks = b - a, original_ticks = d - c;
  }
  Timed& timed = g_timed[f][std::bit_width(size)];
  timed.calls.fetch_add(1, std::memory_order_relaxed);
  timed.original.fetch_add(original_ticks, std::memory_order_relaxed);
  timed.native.fetch_add(native_ticks, std::memory_order_relaxed);
  if (size < kExactSizes) g_exact[f][size].fetch_add(1, std::memory_order_relaxed);
  if (call == 200000 || call % 2000000 == 0) ReportTimes();
}

Counters GetCounters() { return {g_verified.load(), g_different.load()}; }

}  // namespace any360::host::rexglue::xdk
