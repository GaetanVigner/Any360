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
REXCVAR_DEFINE_STRING(any360_xdk_skip, "", "Kernel",
                      "Any360: bound library functions that run their own code while the others run as host routines, "
                      "by name, comma-separated (memcpy, forward copy, memset, memmove, XMemCpy, wcslen, strchr, "
                      "strncmp, strncpy, strrchr, tolower, toupper, wcsncpy, wcscpy, strncat, wcscmp, strlen, strcmp, "
                      "mbtowc, GetLastError, SetLastError, _stricmp, strstr, _finite, _isnan, _sptype, _get_exp, "
                      "_set_exp, _add_exp)");
REXCVAR_DEFINE_BOOL(any360_xdk_bench, false, "Kernel",
                    "Any360: every call of a bound library function is timed both ways, its own code and the host "
                    "routine; the log gets the calls and nanoseconds of each by size (slow)");

REXCVAR_DEFINE_BOOL(any360_d3d_wait_sleep, true, "GPU",
                    "Any360: the D3D runtime's GPU wait step a title binds sleeps until the GPU moves on (while the "
                    "graphics are on Any360); off: it spins as its recompiled code does");
REXCVAR_DEFINE_INT32(any360_d3d_wait_longest_us, 2000, "GPU",
                     "Any360: the longest the D3D GPU wait step sleeps before its own checks run again (the device "
                     "stopping, the GPU hang), in microseconds");

namespace any360::host::rexglue::xdk {

Mode g_mode = Mode::kUnresolved;
any360::xdk::Memory g_memory;

namespace {

constexpr uint32_t kFunctions = uint32_t(Function::kCount);
constexpr const char* kNames[kFunctions] = {"memcpy", "forward copy", "memset",  "memmove", "XMemCpy",
                                            "wcslen", "strchr",       "strncmp", "strncpy", "strrchr",
                                            "tolower", "toupper",     "wcsncpy", "wcscpy",  "strncat",
                                            "wcscmp",  "strlen",      "strcmp",  "mbtowc",  "GetLastError",
                                            "SetLastError", "_stricmp", "strstr", "_finite", "_isnan", "_sptype",
                                            "_get_exp", "_set_exp", "_add_exp"};
// Calls larger than this are not verified or timed (their snapshot would cost more than they tell).
constexpr uint32_t kMostChecked = 4u << 20;
// The bytes compared on each side of a destination, to see a write outside it.
constexpr uint32_t kMargin = 256;

std::atomic<uint64_t> g_verified{0}, g_different{0};
// With any360_xdk_skip: the functions that run their own code.
bool g_skipped[kFunctions] = {};
std::atomic<uint64_t> g_verified_by[kFunctions] = {}, g_declined_by[kFunctions] = {};
// Calls during which bytes next to the destination changed, while the function's own code ran and while the
// replacement ran. A function that writes outside its destination does it at every such call and only on its own
// side; a few calls on either side are another thread writing its own data there at that moment.
std::atomic<uint64_t> g_around_own[kFunctions] = {}, g_around_native[kFunctions] = {};
std::atomic<uint64_t> g_around_logged{0};
// Calls whose two sides differed at first and agreed when both were run again on the same input: another thread
// wrote the source or the destination between the two sides (a copy of a global another thread updates). A
// function that is wrong for an input is wrong at every try.
std::atomic<uint64_t> g_settled[kFunctions] = {};
constexpr int kTries = 3;

// Timed calls by function and size class (class c: sizes with bit_width c, so 0, 1, 2-3, 4-7, ...).
struct Timed {
  std::atomic<uint64_t> calls{0}, original{0}, native{0};
};
Timed g_timed[kFunctions][33];
// Calls by exact size below kExactSizes: which sizes a title really passes (the benchmark's mix is made of them).
constexpr uint32_t kExactSizes = 4096;
std::atomic<uint32_t> g_exact[kFunctions][kExactSizes] = {};
std::atomic<uint64_t> g_timed_calls{0};
// Which side runs first alternates call by call of each function (the second finds the memory in the cache). By
// function: with one count for all, two functions a title always calls one after the other could each keep the
// same place (GetLastError after SetLastError), and the figures of both would lean one way.
std::atomic<uint32_t> g_order[kFunctions] = {};
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
  } else if (const std::string skip = REXCVAR_GET(any360_xdk_skip); !skip.empty()) {
    mode = Mode::kSelect;
    std::string native;
    for (uint32_t f = 0; f < kFunctions; ++f) {
      g_skipped[f] = ("," + skip + ",").find("," + std::string(kNames[f]) + ",") != std::string::npos;
      if (!g_skipped[f]) native += (native.empty() ? "" : ", ") + std::string(kNames[f]);
    }
    REXLOG_INFO("Any360 xdk: any360_xdk_skip \"{}\": as host routines: {}", skip, native);
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
              : mode == Mode::kSelect   ? "as host routines but those skipped (any360_xdk_skip)"
              : mode == Mode::kVerify   ? "both ways, compared (any360_xdk_verify)"
                                        : "both ways, timed (any360_xdk_bench)");
  g_mode = mode;
  return mode;
}

// One call's arguments: r3 whole (memcpy saves all of it), r4, r5, the stack pointer and the function's parameter
// of the title.
struct Call {
  uint64_t dest_register;
  uint32_t dest, second, size, stack_pointer, value;
  // The processor's control region (r13), for the functions that look the thread up.
  uint32_t pcr;
  // What the call leaves in r3, for the functions that write and return a value (ReturnsValue).
  uint64_t result;
  // The bits of f1 as the call came, and what the call leaves there for the functions that return a double.
  uint64_t f1, f1_result;
  // The bytes the call is about to write: [dest, dest + size) for most, found by Accepts for the string functions
  // that write to where a string ends or as far as one goes.
  any360::xdk::Range write;
};

// Whether the replacement takes the call, known before anything runs: the test the replacement itself makes. Says
// what the call will write.
bool Accepts(Function function, Call& c) {
  c.write = {c.dest, c.size};
  switch (function) {
    case Function::kWcsncpy: return any360::xdk::CrtWcsncpyPlan(c.dest, c.second, c.size, c.write);
    case Function::kWcscpy: return any360::xdk::CrtWcscpyPlan(g_memory, c.dest, c.second, c.write);
    case Function::kStrncat: return any360::xdk::CrtStrncatPlan(g_memory, c.dest, c.second, c.size, c.write);
    case Function::kMbtowc: return any360::xdk::CrtMbtowcPlan(c.dest, c.second, c.size, c.write);
    case Function::kFinite:
    case Function::kIsnan:
    case Function::kSptype:
    case Function::kGetExp:
    case Function::kSetExp:
    case Function::kAddExp: return any360::xdk::CrtDoublePlan(c.stack_pointer, c.write);
    case Function::kSetLastError: return any360::xdk::XapiSetLastErrorPlan(g_memory, c.pcr, c.write);
    case Function::kMemcpy:
    case Function::kCopyForward: return any360::xdk::CrtMemcpyAccepts(c.dest, c.second, c.size);
    case Function::kMemset: return any360::xdk::CrtMemsetAccepts(c.dest, c.size);
    case Function::kMemmove: return any360::xdk::CrtMemmoveAccepts(c.dest, c.second, c.size);
    case Function::kStrncpy: return any360::xdk::CrtStrncpyAccepts(c.dest, c.second, c.size);
    default: return any360::xdk::XMemCpyAccepts(c.dest, c.second, c.size, c.size >= 16 || ByteStepIsOne(c.value));
  }
}

bool Native(Function function, Call& c) {
  switch (function) {
    case Function::kMbtowc: return any360::xdk::CrtMbtowc(g_memory, c.dest, c.second, c.size, c.result);
    case Function::kFinite: return any360::xdk::CrtFinite(g_memory, c.stack_pointer, c.f1, c.result);
    case Function::kIsnan: return any360::xdk::CrtIsnan(g_memory, c.stack_pointer, c.f1, c.result);
    case Function::kSptype: return any360::xdk::CrtSptype(g_memory, c.stack_pointer, c.f1, c.result);
    case Function::kGetExp: return any360::xdk::CrtGetExp(g_memory, c.stack_pointer, c.f1, c.result);
    case Function::kSetExp: return any360::xdk::CrtSetExp(g_memory, c.stack_pointer, c.f1, c.second, c.f1_result);
    case Function::kAddExp: return any360::xdk::CrtAddExp(g_memory, c.stack_pointer, c.f1, c.second, c.f1_result);
    case Function::kSetLastError: return any360::xdk::XapiSetLastError(g_memory, c.pcr, c.dest);
    case Function::kMemcpy: return any360::xdk::CrtMemcpy(g_memory, c.dest_register, c.second, c.size, c.stack_pointer);
    case Function::kCopyForward: return any360::xdk::CrtCopyForward(g_memory, c.dest, c.second, c.size);
    case Function::kMemset: return any360::xdk::CrtMemset(g_memory, c.dest, c.second, c.size);
    case Function::kMemmove: return any360::xdk::CrtMemmove(g_memory, c.dest_register, c.second, c.size, c.stack_pointer);
    case Function::kStrncpy: return any360::xdk::CrtStrncpy(g_memory, c.dest, c.second, c.size);
    case Function::kWcsncpy: return any360::xdk::CrtWcsncpy(g_memory, c.dest, c.second, c.size);
    case Function::kWcscpy: return any360::xdk::CrtWcscpy(g_memory, c.dest, c.second);
    case Function::kStrncat: return any360::xdk::CrtStrncat(g_memory, c.dest, c.second, c.size);
    default: return any360::xdk::XMemCpy(g_memory, c.dest, c.second, c.size, c.size >= 16 || ByteStepIsOne(c.value));
  }
}

// The functions that only read: their replacement gives the value they leave in r3.
bool IsPure(Function function) {
  return function == Function::kWcslen || function == Function::kStrchr || function == Function::kStrncmp ||
         function == Function::kStrrchr || function == Function::kTolower || function == Function::kToupper ||
         function == Function::kWcscmp || function == Function::kStrlen || function == Function::kStrcmp ||
         function == Function::kGetLastError || function == Function::kStricmp || function == Function::kStrstr;
}
// The functions that write and leave a value of their own in r3 (the others that write return r3 as it came).
bool ReturnsValue(Function function) {
  return function == Function::kMbtowc || function == Function::kFinite || function == Function::kIsnan ||
         function == Function::kSptype || function == Function::kGetExp;
}
// The functions that leave a double in f1.
bool ReturnsDouble(Function function) { return function == Function::kSetExp || function == Function::kAddExp; }
// The functions whose first instruction stores a double: the host's flush-to-zero mode is off from there on.
bool TakesDouble(Function function) { return function >= Function::kFinite && function <= Function::kAddExp; }
bool Pure(Function function, const Call& c, uint64_t& r3) {
  switch (function) {
    case Function::kWcslen: return any360::xdk::CrtWcslen(g_memory, c.dest, r3);
    case Function::kStrchr: return any360::xdk::CrtStrchr(g_memory, c.dest_register, c.second, r3);
    case Function::kStrrchr: return any360::xdk::CrtStrrchr(g_memory, c.dest_register, c.second, r3);
    case Function::kTolower: r3 = any360::xdk::CrtTolower(c.dest_register); return true;
    case Function::kToupper: r3 = any360::xdk::CrtToupper(c.dest_register); return true;
    case Function::kWcscmp: return any360::xdk::CrtWcscmp(g_memory, c.dest, c.second, r3);
    case Function::kStrlen: return any360::xdk::CrtStrlen(g_memory, c.dest, r3);
    case Function::kStrcmp: return any360::xdk::CrtStrcmp(g_memory, c.dest, c.second, r3);
    case Function::kGetLastError: return any360::xdk::XapiGetLastError(g_memory, c.pcr, r3);
    case Function::kStricmp: return any360::xdk::CrtStricmp(g_memory, c.dest, c.second, r3);
    case Function::kStrstr: return any360::xdk::CrtStrstr(g_memory, c.dest_register, c.second, r3);
    default: return any360::xdk::CrtStrncmp(g_memory, c.dest, c.second, c.size, r3);
  }
}
// How much such a call had to read, for the timings by size: the length found, the distance to the character, the
// count asked.
uint32_t PureWork(Function function, const Call& c, uint64_t r3) {
  switch (function) {
    case Function::kWcslen:
    case Function::kStrlen: return uint32_t(r3);
    case Function::kWcscmp:
    case Function::kStrcmp:
    case Function::kStricmp:
    case Function::kGetLastError: return 0;
    case Function::kStrstr:
    case Function::kStrchr:
    case Function::kStrrchr: return r3 ? uint32_t(r3) - c.dest : 0;
    case Function::kTolower:
    case Function::kToupper: return 0;
    default: return std::min<uint32_t>(c.size, 1u << 20);
  }
}

// Whether a call reads nothing of what it writes, so that running it twice on the memory as it is gives the same
// bytes twice: fills, and copies whose source is apart from what is written. Not strncat (it looks for the
// terminator it then writes over) nor a copy onto its own source.
bool RunsTwiceInPlace(Function function, const Call& c) {
  switch (function) {
    case Function::kMemset:
    case Function::kXMemCpy:
    case Function::kWcscpy:
    case Function::kFinite:
    case Function::kIsnan:
    case Function::kSptype:
    case Function::kGetExp:
    case Function::kSetExp:
    case Function::kAddExp:
    case Function::kSetLastError: return true;
    case Function::kMemcpy:
    case Function::kCopyForward:
    case Function::kMemmove:
    case Function::kStrncpy:
    case Function::kWcsncpy: return any360::xdk::AreApart(c.write.at, c.second, c.write.size);
    case Function::kMbtowc: return c.write.size == 0 || c.second < c.write.at || c.second - c.write.at >= c.write.size;
    default: return false;
  }
}

// The 8 bytes below the stack pointer are compared for the functions that are leaves (memcpy leaves its
// destination there; the others leave it alone). XMemCpy builds a frame there, which no caller can see.
bool ComparesStackSlot(Function function) { return function != Function::kXMemCpy; }

// The bytes just before and just after a destination that are safe to read: on the same 4 KB pages as its first
// and last bytes, plain memory, and away from the stack the function itself uses (below the stack pointer).
struct Margins {
  uint32_t before = 0, after = 0;  // guest addresses of the margins' starts
  uint32_t before_size = 0, after_size = 0;
};
Margins MarginsOf(const Call& c) {
  Margins m;
  if (!c.write.size) return m;
  const auto usable = [&](uint32_t address, uint32_t size) {
    if (!size || !any360::xdk::IsPlainMemory(address, size)) return false;
    const uint64_t stack_low = uint64_t(c.stack_pointer) - 2048, stack_high = c.stack_pointer;
    return uint64_t(address) + size <= stack_low || address >= stack_high;
  };
  const uint32_t page = c.write.at & ~0xFFFu;
  m.before_size = std::min(kMargin, c.write.at - page);
  m.before = c.write.at - m.before_size;
  if (!usable(m.before, m.before_size)) m.before_size = 0;
  const uint32_t end = c.write.at + c.write.size;  // plain memory: no wrap
  const uint64_t page_end = (uint64_t(end - 1) | 0xFFF) + 1;
  m.after_size = uint32_t(std::min<uint64_t>(kMargin, page_end - end));
  m.after = end;
  if (!usable(m.after, m.after_size)) m.after_size = 0;
  return m;
}
std::vector<uint8_t> ReadMargins(const Margins& m) {
  std::vector<uint8_t> bytes(m.before_size + m.after_size);
  if (m.before_size) std::memcpy(bytes.data(), g_memory.Host(m.before), m.before_size);
  if (m.after_size) std::memcpy(bytes.data() + m.before_size, g_memory.Host(m.after), m.after_size);
  return bytes;
}

std::string CountsByFunction(const std::atomic<uint64_t>* counts) {
  std::string text;
  for (uint32_t f = 0; f < kFunctions; ++f) {
    if (const uint64_t n = counts[f].load()) text += (text.empty() ? "" : ", ") + std::string(kNames[f]) + " " + std::to_string(n);
  }
  return text.empty() ? "none" : text;
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
  REXLOG_INFO("Any360 xdk bench: calls left to the functions' own code (not timed): {}", CountsByFunction(g_declined_by));
}

}  // namespace

void RunSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t value) {
  Mode mode = g_mode;
  if (mode == Mode::kUnresolved) mode = Resolve(base);
  // Both sides start as the function's own code does: with a store of a double, which turns the host's
  // flush-to-zero mode off. Done on the title's registers, so that the copies the sides run on agree with them.
  if (TakesDouble(function)) ctx.fpscr.disableFlushMode();
  Call call{ctx.r3.u64, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r1.u32, value, ctx.r13.u32, ctx.r3.u64, ctx.f1.u64, ctx.f1.u64, {}};
  const uint64_t dest_register = call.dest_register;
  const uint32_t second = call.second, stack_pointer = call.stack_pointer;
  uint32_t dest = call.dest, size = call.size;
  if (mode == Mode::kSelect) {
    if (g_skipped[uint32_t(function)]) {
      original(ctx, base);
      return;
    }
    mode = Mode::kNative;
  }
  if (mode == Mode::kNative) {
    if (IsPure(function)) {
      uint64_t r3 = 0;
      if (Pure(function, call, r3)) ctx.r3.u64 = r3;
      else original(ctx, base);
    } else if (!Native(function, call)) {
      original(ctx, base);
    } else if (ReturnsValue(function)) {
      ctx.r3.u64 = call.result;
    } else if (ReturnsDouble(function)) {
      ctx.f1.u64 = call.f1_result;
    }
    return;
  }
  const uint32_t f = uint32_t(function);
  if (IsPure(function)) {
    // Nothing is written: the function's own code runs on a copy of the registers and its r3 stands; the
    // replacement's r3 is compared with it, or the two are timed.
    uint64_t r3 = 0;
    if (mode == Mode::kOriginal) {
      original(ctx, base);
      return;
    }
    PPCContext reference = ctx;
    if (mode == Mode::kVerify) {
      original(reference, base);
      const bool taken = Pure(function, call, r3);
      ctx.r3.u64 = reference.r3.u64;
      if (!taken) {
        g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
        return;
      }
      g_verified_by[f].fetch_add(1, std::memory_order_relaxed);
      g_verified.fetch_add(1, std::memory_order_relaxed);
      if (r3 != reference.r3.u64 && g_different.fetch_add(1, std::memory_order_relaxed) < 20) {
        REXLOG_ERROR("Any360 xdk: {} differs from the function's own code: r3 {:016X} r4 {:08X} r5 {:08X}: {:016X}, its "
                     "own code {:016X}",
                     kNames[f], dest_register, second, size, r3, reference.r3.u64);
      }
      return;
    }
    const uint64_t timed_call = g_timed_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t original_ticks, native_ticks;
    bool taken;
    if (g_order[f].fetch_add(1, std::memory_order_relaxed) & 1) {
      const uint64_t a = Ticks();
      original(reference, base);
      const uint64_t b = Ticks();
      taken = Pure(function, call, r3);
      const uint64_t c = Ticks();
      original_ticks = b - a, native_ticks = c - b;
    } else {
      const uint64_t a = Ticks();
      taken = Pure(function, call, r3);
      const uint64_t b = Ticks();
      original(reference, base);
      const uint64_t c = Ticks();
      native_ticks = b - a, original_ticks = c - b;
    }
    ctx.r3.u64 = reference.r3.u64;
    if (!taken) {
      g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t work = PureWork(function, call, reference.r3.u64);
    Timed& timed = g_timed[f][std::bit_width(work)];
    timed.calls.fetch_add(1, std::memory_order_relaxed);
    timed.original.fetch_add(original_ticks, std::memory_order_relaxed);
    timed.native.fetch_add(native_ticks, std::memory_order_relaxed);
    if (work < kExactSizes) g_exact[f][work].fetch_add(1, std::memory_order_relaxed);
    if (timed_call == 200000 || timed_call % 2000000 == 0) ReportTimes();
    return;
  }
  // Verifying and timing run both sides on the same input: only the calls the replacement accepts, known before
  // anything runs.
  if (mode == Mode::kOriginal || !Accepts(function, call) || call.write.size > kMostChecked) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // From here the destination is what the call writes.
  dest = call.write.at, size = call.write.size;
  uint8_t* const target = g_memory.Host(dest);
  uint8_t* const slot = g_memory.Host(stack_pointer - 8);
  const bool with_slot = ComparesStackSlot(function);
  // Most calls read nothing of what they write, so both sides can run one after the other on the memory as it is:
  // what the call writes never goes back to what it was. It matters: a title fills a structure on one thread while
  // another reads it, and bytes put back between the two sides showed that thread a pointer that was gone again
  // (The Simpsons Game crashed in a level that way, with every call verified). The few that read what they write
  // (a copy onto its own source, strncat's terminator) have the destination put back between the sides.
  const bool in_place = RunsTwiceInPlace(function, call);
  std::vector<uint8_t> before;
  if (!in_place) before.assign(target, target + size);
  uint8_t slot_before[8];
  std::memcpy(slot_before, slot, 8);
  const auto put_back = [&] {
    if (in_place) return;
    std::memcpy(target, before.data(), size);
    if (with_slot) std::memcpy(slot, slot_before, 8);
  };

  if (mode == Mode::kVerify) {
    // What each side wrote in [dest, dest + size) and in the 8 bytes below the stack pointer, and the r3 each
    // returned; and the bytes around the destination, which neither should change. The function's own code runs
    // on a copy of the registers and runs last when the call runs in place: its result is what stays.
    const Margins margins = MarginsOf(call);
    const std::vector<uint8_t> around_before = ReadMargins(margins);
    PPCContext reference = ctx;
    std::vector<uint8_t> first;
    uint8_t slot_first[8];
    bool own_code_outside = false, native_outside = false, same_bytes = false, same_slot = false, same_r3 = false;
    // When the two sides differ, both again (kTries in all): what another thread wrote between them the first time
    // is the same for both the second time.
    int tries = 0;
    for (; tries < kTries; ++tries) {
      if (tries) {
        put_back();
        reference = ctx;
        call.result = ctx.r3.u64;
      }
      if (in_place) {
        Native(function, call);
        first.assign(target, target + size);
        std::memcpy(slot_first, slot, 8);
        const std::vector<uint8_t> around_native = ReadMargins(margins);
        original(reference, base);
        if (!tries) {
          native_outside = around_native != around_before;
          own_code_outside = ReadMargins(margins) != around_native;
        }
      } else {
        original(reference, base);
        first.assign(target, target + size);
        std::memcpy(slot_first, slot, 8);
        const std::vector<uint8_t> around_own = ReadMargins(margins);
        put_back();
        Native(function, call);
        if (!tries) {
          own_code_outside = around_own != around_before;
          native_outside = ReadMargins(margins) != around_own;
        }
      }
      same_bytes = std::memcmp(target, first.data(), size) == 0;
      same_slot = !with_slot || std::memcmp(slot, slot_first, 8) == 0;
      same_r3 = ReturnsValue(function) ? call.result == reference.r3.u64 : ctx.r3.u32 == reference.r3.u32;
      if (ReturnsDouble(function)) same_r3 = same_r3 && call.f1_result == reference.f1.u64;
      if (same_bytes && same_slot && same_r3) break;
    }
    if (tries && tries < kTries) g_settled[f].fetch_add(1, std::memory_order_relaxed);
    g_verified_by[f].fetch_add(1, std::memory_order_relaxed);
    const uint64_t verified = g_verified.fetch_add(1, std::memory_order_relaxed) + 1;
    if (verified == 1000 || verified % 1000000 == 0) {
      REXLOG_INFO("Any360 xdk: {} calls verified against the functions' own code ({}), {} different; bytes next to a "
                  "destination changed during its own code: {}; during the replacement: {}; left to its own code: {}; "
                  "different at first and the same when run again (another thread wrote meanwhile): {}",
                  verified, CountsByFunction(g_verified_by), g_different.load(), CountsByFunction(g_around_own),
                  CountsByFunction(g_around_native), CountsByFunction(g_declined_by), CountsByFunction(g_settled));
    }
    if (own_code_outside || native_outside) {
      if (own_code_outside) g_around_own[f].fetch_add(1, std::memory_order_relaxed);
      if (native_outside) g_around_native[f].fetch_add(1, std::memory_order_relaxed);
      if (g_around_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_WARN("Any360 xdk: {} r3 {:016X} source/fill {:08X} size {}: bytes next to the destination changed during {}",
                    kNames[f], dest_register, second, size, own_code_outside ? "the function's own code" : "the replacement");
      }
    }
    if (ReturnsDouble(function)) ctx.f1.u64 = reference.f1.u64;  // the function's own double, whatever follows
    if (same_bytes && same_slot && same_r3) {
      if (ReturnsValue(function)) ctx.r3.u64 = reference.r3.u64;
      return;
    }
    if (g_different.fetch_add(1, std::memory_order_relaxed) < 20) {
      uint64_t slot_now, slot_other;
      std::memcpy(&slot_now, slot, 8);
      std::memcpy(&slot_other, slot_first, 8);
      REXLOG_ERROR("Any360 xdk: {} differs from the function's own code: r3 {:016X} source/fill {:08X} size {}: {}{}{}"
                   "(stack slot {:016X} and {:016X})",
                   kNames[f], dest_register, second, size, same_bytes ? "" : "bytes written ",
                   same_slot ? "" : "stack slot ", same_r3 ? "" : (ReturnsDouble(function) ? "f1 " : "r3 "),
                   any360::xdk::detail::ByteSwap64(slot_now), any360::xdk::detail::ByteSwap64(slot_other));
    }
    // The function's own result stands: it ran last in place; otherwise what it wrote is put there again.
    if (!in_place) {
      std::memcpy(target, first.data(), size);
      std::memcpy(slot, slot_first, 8);
    }
    ctx.r3.u64 = reference.r3.u64;
    return;
  }

  // Bench: both sides timed on the same input, in alternating order (the second finds the memory in the cache).
  const uint64_t timed_call = g_timed_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t original_ticks, native_ticks;
  PPCContext copy = ctx;
  if (g_order[f].fetch_add(1, std::memory_order_relaxed) & 1) {
    const uint64_t a = Ticks();
    original(copy, base);
    const uint64_t b = Ticks();
    put_back();
    const uint64_t c = Ticks();
    Native(function, call);
    const uint64_t d = Ticks();
    original_ticks = b - a, native_ticks = d - c;
  } else {
    const uint64_t a = Ticks();
    Native(function, call);
    const uint64_t b = Ticks();
    put_back();
    const uint64_t c = Ticks();
    original(copy, base);
    const uint64_t d = Ticks();
    native_ticks = b - a, original_ticks = d - c;
  }
  if (ReturnsValue(function)) ctx.r3.u64 = call.result;
  if (ReturnsDouble(function)) ctx.f1.u64 = call.f1_result;
  Timed& timed = g_timed[f][std::bit_width(size)];
  timed.calls.fetch_add(1, std::memory_order_relaxed);
  timed.original.fetch_add(original_ticks, std::memory_order_relaxed);
  timed.native.fetch_add(native_ticks, std::memory_order_relaxed);
  if (size < kExactSizes) g_exact[f][size].fetch_add(1, std::memory_order_relaxed);
  if (timed_call == 200000 || timed_call % 2000000 == 0) ReportTimes();
}

Counters GetCounters() { return {g_verified.load(), g_different.load()}; }

const D3dWaitSettings& D3dWait() {
  static const D3dWaitSettings settings = [] {
    D3dWaitSettings s;
    s.sleeps = REXCVAR_GET(any360_d3d_wait_sleep);
    s.longest_us = uint32_t(std::clamp(REXCVAR_GET(any360_d3d_wait_longest_us), 50, 100000));
    REXLOG_INFO("Any360 d3d: the GPU wait step {}", s.sleeps ? "sleeps until the GPU moves (at most " +
                                                                    std::to_string(s.longest_us) + " us)"
                                                              : "spins (any360_d3d_wait_sleep off)");
    return s;
  }();
  return settings;
}

namespace {
std::atomic<uint64_t> g_wait_steps{0}, g_wait_slept{0}, g_wait_timed_out{0};
std::atomic<int64_t> g_wait_next_report{0};
}  // namespace

void CountD3dWait(bool slept, bool moved) {
  const uint64_t steps = g_wait_steps.fetch_add(1, std::memory_order_relaxed) + 1;
  // A step that spins looks at the clock once every 4096 (millions of steps a second: the clock would be most of them).
  if (!slept && (steps & 4095)) return;
  if (slept) g_wait_slept.fetch_add(1, std::memory_order_relaxed);
  if (slept && !moved) g_wait_timed_out.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = g_wait_next_report.load(std::memory_order_relaxed);
  if (now < next || !g_wait_next_report.compare_exchange_strong(next, now + 10)) return;
  if (next == 0) return;  // the first step: the report starts in 10 s
  const uint64_t counted = g_wait_steps.exchange(0), sleeps = g_wait_slept.exchange(0), timed_out = g_wait_timed_out.exchange(0);
  REXLOG_INFO("Any360 d3d: GPU wait steps in 10 s: {} ({} slept, {} of them to the time-out)", counted, sleeps, timed_out);
}

}  // namespace any360::host::rexglue::xdk
