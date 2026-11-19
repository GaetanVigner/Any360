#include "any360/host/xdk_natives.h"

#include "any360/xdk/xaudio_reverb.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <functional>
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
                      "_set_exp, _add_exp, XAudio mix ramp, D3D dirty constants, D3D dirty words, D3D set shader constants, XAudio fill)");
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
                                            "_get_exp", "_set_exp", "_add_exp", "XAudio mix ramp", "D3D dirty constants", "D3D dirty words", "D3D set shader constants", "XAudio fill",
                                            "D3D blend", "D3D bool constants", "D3D int constants",
                                            "D3D pixel shader constants", "D3D SetTexture", "D3D SetStreamSource",
                                            "D3D sampler filter", "D3D SetVertexDeclaration", "D3D SetShader",
                                            "D3D CPU range flush", "zlib inflate_fast", "png_read_filter_row", "XMemSet", "sin", "cos", "asin",
                                            "D3D render state", "atan2", "pow", "D3D surfaces", "XAudio reverb", "D3D viewport",
                                            "D3D query", "D3D resource type", "D3D format block",
                                            "D3D DrawVerticesUP"};
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
// Which side runs first, call by call of each function (the second finds the memory in the cache, and the first
// pays a fault on a page the renderer watches). By function: with one count for all, two functions a title always
// calls one after the other could each keep the same place (GetLastError after SetLastError). Not plain
// alternation either: a title that calls a function twice a frame (D3D's constants: the vertex shader's, then
// the pixel shader's) put the second call on the same side every time, and only the replacement ever paid the
// fault of the frame's first write to a watched page of the command buffer (0.1x where it was 1.4x). The count
// times the golden ratio's fraction: half and half, with no period.
std::atomic<uint32_t> g_order[kFunctions] = {};
bool OriginalFirst(uint32_t f) {
  const uint64_t n = g_order[f].fetch_add(1, std::memory_order_relaxed);
  return (n * 0x9E3779B97F4A7C15ull) >> 63;
}
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
    case Function::kXMemSet: return any360::xdk::XMemSetAccepts(c.dest, c.size, c.size < 16 || VectorStepIs16(c.value));
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
    case Function::kXMemSet:
      return any360::xdk::XMemSet(g_memory, c.dest, c.second, c.size, c.size < 16 || VectorStepIs16(c.value));
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
    case Function::kXMemSet:
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
// destination there; the others leave it alone). XMemCpy and XMemSet build a frame there, which no caller can see.
bool ComparesStackSlot(Function function) { return function != Function::kXMemCpy && function != Function::kXMemSet; }

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
    if (OriginalFirst(f)) {
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
  if (OriginalFirst(f)) {
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

namespace {

// The registers XAudio's mix writes, named, for the comparison of its two sides: the first that differs, or null.
const char* FirstDifferentRegister(const PPCContext& a, const PPCContext& b, bool groups) {
  const struct {
    const char* name;
    const PPCRegister& x;
    const PPCRegister& y;
  } gprs[] = {{"r3", a.r3, b.r3}, {"r4", a.r4, b.r4}, {"r5", a.r5, b.r5}, {"r6", a.r6, b.r6}, {"r7", a.r7, b.r7},
              {"r8", a.r8, b.r8}, {"r9", a.r9, b.r9}, {"r10", a.r10, b.r10}, {"r11", a.r11, b.r11}, {"r31", a.r31, b.r31}};
  for (const auto& g : gprs) {
    if (g.x.u64 != g.y.u64) return g.name;
  }
  using V = decltype(PPCContext::v0);
  const struct {
    const char* name;
    const V& x;
    const V& y;
    bool always;
  } vrs[] = {{"v0", a.v0, b.v0, true},     {"v1", a.v1, b.v1, false},    {"v2", a.v2, b.v2, false},
             {"v3", a.v3, b.v3, false},    {"v4", a.v4, b.v4, false},    {"v5", a.v5, b.v5, false},
             {"v6", a.v6, b.v6, false},    {"v7", a.v7, b.v7, false},    {"v8", a.v8, b.v8, false},
             {"v9", a.v9, b.v9, false},    {"v10", a.v10, b.v10, true},  {"v11", a.v11, b.v11, true},
             {"v12", a.v12, b.v12, true},  {"v13", a.v13, b.v13, true},  {"v29", a.v29, b.v29, false},
             {"v30", a.v30, b.v30, false}, {"v31", a.v31, b.v31, false}};
  for (const auto& v : vrs) {
    if ((v.always || groups) && std::memcmp(v.x.u32, v.y.u32, 16) != 0) return v.name;
  }
  return nullptr;
}

// The replacement on a copy of the title's registers, as the plain native path gives them back.
bool MixNative(PPCContext& ctx, any360::xdk::VmxRegisters x, bool groups) {
  if (!any360::xdk::XAudioMixRamp(g_memory, x)) return false;
  FromVmx(x, ctx);
  if (groups) GroupVectorsFromVmx(x, ctx);
  return true;
}

}  // namespace

namespace {

// A function whose replacement takes and gives back registers of its own (not the r3-r5 calls of RunSlow): what
// both sides write (put back between them: these functions read what they write), the replacement on a copy of
// the registers, and the first register the two sides leave different.
struct Sides {
  uint32_t f;
  std::vector<any360::xdk::Range> writes;
  uint32_t work;  // for the timings by size
  std::function<void(PPCContext&)> native;
  std::function<const char*(const PPCContext&, const PPCContext&)> differs;
  std::string call;  // the call's arguments, for a difference logged
};

void RunSides(Mode mode, const Sides& s, PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t f = s.f;
  // The host's floating-point control word as the call found it: each side starts with it, the one the context
  // says. The recompiled code and the replacements only write it when the guest's mode changes, so a side started
  // with the other's leftover flush mode would compute denormals another way than its context says.
  const uint32_t entry_csr = ctx.fpscr.csr;
  const auto at_entry = [entry_csr] { _mm_setcsr(entry_csr); };
  std::vector<std::vector<uint8_t>> before;
  for (const any360::xdk::Range& w : s.writes) before.emplace_back(g_memory.Host(w.at), g_memory.Host(w.at) + w.size);
  const auto put_back = [&] {
    for (size_t i = 0; i < s.writes.size(); ++i) std::memcpy(g_memory.Host(s.writes[i].at), before[i].data(), s.writes[i].size);
  };
  const auto same_as = [&](const std::vector<std::vector<uint8_t>>& bytes, size_t& range, uint32_t& at) {
    for (range = 0; range < s.writes.size(); ++range) {
      const uint8_t* now = g_memory.Host(s.writes[range].at);
      for (at = 0; at < s.writes[range].size; ++at) {
        if (now[at] != bytes[range][at]) return false;
      }
    }
    return true;
  };

  if (mode == Mode::kVerify) {
    PPCContext reference = ctx, mine = ctx;
    std::vector<std::vector<uint8_t>> first(s.writes.size());
    bool same_bytes = false;
    const char* register_differs = nullptr;
    size_t range = 0;
    uint32_t at = 0;
    int tries = 0;
    for (; tries < kTries; ++tries) {
      if (tries) put_back();
      reference = ctx;
      at_entry();
      original(reference, base);
      for (size_t i = 0; i < s.writes.size(); ++i) {
        first[i].assign(g_memory.Host(s.writes[i].at), g_memory.Host(s.writes[i].at) + s.writes[i].size);
      }
      put_back();
      mine = ctx;
      at_entry();
      s.native(mine);
      same_bytes = same_as(first, range, at);
      register_differs = s.differs(mine, reference);
      if (same_bytes && !register_differs) break;
      // The first few calls that differ at first: where, for the cases that agree when run again.
      static std::atomic<uint32_t> first_logged{0};
      if (!tries && first_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        const uint32_t word_at = s.writes[std::min(range, s.writes.size() - 1)].at + (at & ~3u);
        uint32_t replacement = 0, own = 0;
        if (!same_bytes) {
          std::memcpy(&replacement, g_memory.Host(word_at), 4);
          std::memcpy(&own, first[range].data() + (at & ~3u), 4);
        }
        // The function's own code once more on the same input: does it give itself the same bytes?
        std::vector<std::vector<uint8_t>> native_bytes(s.writes.size());
        for (size_t i = 0; i < s.writes.size(); ++i) {
          native_bytes[i].assign(g_memory.Host(s.writes[i].at), g_memory.Host(s.writes[i].at) + s.writes[i].size);
        }
        put_back();
        PPCContext own_again = ctx;
        at_entry();
        original(own_again, base);
        size_t own_range = 0;
        uint32_t own_at = 0;
        const bool own_repeats = same_as(first, own_range, own_at);
        uint32_t own_second = 0;
        std::memcpy(&own_second, g_memory.Host(word_at), 4);
        REXLOG_WARN("Any360 xdk: {}: its own code again on the same input: {} (range {} at +{:X}; the word {:08X})",
                    kNames[f], own_repeats ? "the same bytes" : "other bytes", own_range, own_at,
                    std::byteswap(own_second));
        for (size_t i = 0; i < s.writes.size(); ++i) {
          std::memcpy(g_memory.Host(s.writes[i].at), native_bytes[i].data(), s.writes[i].size);
        }
        _mm_setcsr(mine.fpscr.csr);
        REXLOG_WARN("Any360 xdk: {} differs at first: {}: {}{} (range {} at +{:X}, word {:08X}: replacement {:08X}, own "
                    "code {:08X})",
                    kNames[f], s.call, same_bytes ? "" : "bytes written ", register_differs ? register_differs : "",
                    range, at, word_at, std::byteswap(replacement),
                    std::byteswap(own));
      }
    }
    if (tries && tries < kTries) g_settled[f].fetch_add(1, std::memory_order_relaxed);
    g_verified_by[f].fetch_add(1, std::memory_order_relaxed);
    const uint64_t verified = g_verified.fetch_add(1, std::memory_order_relaxed) + 1;
    if (verified == 1000 || verified % 1000000 == 0) {
      REXLOG_INFO("Any360 xdk: {} calls verified against the functions' own code ({}), {} different; left to its own "
                  "code: {}; different at first and the same when run again: {}",
                  verified, CountsByFunction(g_verified_by), g_different.load(), CountsByFunction(g_declined_by),
                  CountsByFunction(g_settled));
    }
    if (!(same_bytes && !register_differs) && g_different.fetch_add(1, std::memory_order_relaxed) < 20) {
      REXLOG_ERROR("Any360 xdk: {} differs from the function's own code: {}: {}{}{} (first byte different: range {} "
                   "at +{})",
                   kNames[f], s.call, same_bytes ? "" : "bytes written ", register_differs ? "register " : "",
                   register_differs ? register_differs : "", range, at);
    }
    // The function's own result stands.
    for (size_t i = 0; i < s.writes.size(); ++i) std::memcpy(g_memory.Host(s.writes[i].at), first[i].data(), s.writes[i].size);
    ctx = reference;
    _mm_setcsr(ctx.fpscr.csr);
    return;
  }

  // Bench: both sides timed on the same input, in alternating order; the side that ran last stands.
  const uint64_t timed_call = g_timed_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t original_ticks, native_ticks;
  PPCContext own = ctx, mine = ctx;
  if (OriginalFirst(f)) {
    const uint64_t a = Ticks();
    original(own, base);
    const uint64_t b = Ticks();
    put_back();
    at_entry();
    const uint64_t c = Ticks();
    s.native(mine);
    const uint64_t d = Ticks();
    original_ticks = b - a, native_ticks = d - c;
    ctx = mine;
  } else {
    const uint64_t a = Ticks();
    s.native(mine);
    const uint64_t b = Ticks();
    put_back();
    at_entry();
    const uint64_t c = Ticks();
    original(own, base);
    const uint64_t d = Ticks();
    native_ticks = b - a, original_ticks = d - c;
    ctx = own;
  }
  // A replacement call ten times slower than the function's own code is a fault or a wait, not its work: the first
  // few are logged with their arguments (a page the renderer watches, memory that is not where it seems).
  static std::atomic<uint32_t> slow_logged{0};
  if (native_ticks > 10 * original_ticks + 2000 && slow_logged.fetch_add(1, std::memory_order_relaxed) < 6) {
    // Once more on the same input, right away: a second run as slow is the replacement's own work; a fast one, the
    // first touch of the memory (a page fault, a watched page).
    put_back();
    PPCContext again = ctx;
    at_entry();
    const uint64_t a = Ticks();
    s.native(again);
    const uint64_t again_ticks = Ticks() - a;
    _mm_setcsr(ctx.fpscr.csr);
    REXLOG_WARN("Any360 xdk bench: {} took {} ticks, its own code {}, again {}: {}", kNames[f], native_ticks,
                original_ticks, again_ticks, s.call);
  }
  Timed& timed = g_timed[f][std::bit_width(s.work)];
  timed.calls.fetch_add(1, std::memory_order_relaxed);
  timed.original.fetch_add(original_ticks, std::memory_order_relaxed);
  timed.native.fetch_add(native_ticks, std::memory_order_relaxed);
  if (s.work < kExactSizes) g_exact[f][s.work].fetch_add(1, std::memory_order_relaxed);
  if (timed_call == 200000 || timed_call % 2000000 == 0) ReportTimes();
}

// The mode, resolved; kSelect turned into the native or the original one for function `f`.
Mode SlowMode(uint32_t f, uint8_t* base) {
  Mode mode = g_mode;
  if (mode == Mode::kUnresolved) mode = Resolve(base);
  if (mode == Mode::kSelect) mode = g_skipped[f] ? Mode::kOriginal : Mode::kNative;
  return mode;
}

}  // namespace

void RunMixSlow(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t f = uint32_t(Function::kMixRamp);
  const Mode mode = SlowMode(f, base);
  any360::xdk::VmxRegisters x;
  ToVmx(ctx, x);
  const uint32_t samples = uint32_t(x.gpr[7]);
  const bool groups = any360::xdk::detail::MixRampGroups(samples) != 0;
  ctx.fpscr.enableFlushMode();  // as the function's first vector instruction does: both sides start so
  if (mode == Mode::kNative) {
    if (!MixNative(ctx, x, groups)) original(ctx, base);
    return;
  }
  if (mode == Mode::kOriginal || !any360::xdk::XAudioMixRampAccepts(x)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The buffers are the mixing thread's own.
  Sides s;
  s.f = f;
  s.writes = {any360::xdk::XAudioMixRampWrite(x), {uint32_t(x.gpr[1]) - 8, 8}};
  s.work = samples;
  s.native = [&](PPCContext& c) { MixNative(c, x, groups); };
  s.differs = [&](const PPCContext& a, const PPCContext& b) { return FirstDifferentRegister(a, b, groups); };
  s.call = fmt::format("r3 {:016X} r4 {:016X} r5 {:08X} r6 {:08X} samples {}", x.gpr[3], x.gpr[4], uint32_t(x.gpr[5]),
                       uint32_t(x.gpr[6]), samples);
  RunSides(mode, s, ctx, base, original);
}

void RunConstantsSlow(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t cursor_offset, bool words) {
  const uint32_t f = uint32_t(words ? Function::kDirtyWords : Function::kDirtyConstants);
  const Mode mode = SlowMode(f, base);
  const uint32_t device = ctx.r3.u32, source = ctx.r6.u32;
  const uint64_t mask = ctx.r4.u64, index = ctx.r5.u64;
  const auto native = [=](PPCContext& c) {
    uint64_t i = index, cursor = 0;
    if (!(words ? any360::xdk::D3dDirtyWords(g_memory, device, cursor_offset, mask, i, source, cursor)
                : any360::xdk::D3dDirtyConstants(g_memory, device, cursor_offset, mask, i, source, cursor))) {
      return false;
    }
    c.r4.u64 = cursor;
    c.r5.u64 = i;
    return true;
  };
  if (mode == Mode::kNative) {
    if (!native(ctx)) original(ctx, base);
    return;
  }
  any360::xdk::Range buffer;
  if (mode == Mode::kOriginal ||
      !(words ? any360::xdk::D3dDirtyWordsAccepts(g_memory, device, cursor_offset, mask, index, source, buffer)
              : any360::xdk::D3dDirtyConstantsAccepts(g_memory, device, cursor_offset, mask, index, source, buffer))) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The command buffer is the rendering thread's own until it is kicked.
  Sides s;
  s.f = f;
  s.writes = {buffer, {device + cursor_offset, 4}};
  s.work = uint32_t(std::popcount(mask)) * (words ? 4 : 64);
  s.native = [&](PPCContext& c) { native(c); };
  s.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    if (a.r3.u64 != b.r3.u64) return "r3";
    if (a.r4.u64 != b.r4.u64) return "r4";
    if (a.r5.u64 != b.r5.u64) return "r5";
    return nullptr;
  };
  s.call = fmt::format("device {:08X} mask {:016X} index {:X} constants {:08X} buffer {:08X}", device, mask, index,
                       source, buffer.at);
  RunSides(mode, s, ctx, base, original);
}

void RunSetConstantsSlow(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t f = uint32_t(Function::kSetConstants);
  const Mode mode = SlowMode(f, base);
  const uint64_t device = ctx.r3.u64, first = ctx.r4.u64, dirty = ctx.r7.u64;
  const uint32_t source = ctx.r5.u32, count = ctx.r6.u32, stack_pointer = ctx.r1.u32;
  const auto native = [=](PPCContext&) {
    return any360::xdk::D3dSetShaderConstants(g_memory, device, first, source, count, dirty, stack_pointer);
  };
  if (mode == Mode::kNative) {
    if (!native(ctx)) original(ctx, base);
    return;
  }
  any360::xdk::Range constants;
  if (mode == Mode::kOriginal ||
      !any360::xdk::D3dSetShaderConstantsAccepts(device, first, source, count, stack_pointer, constants)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The device is the rendering thread's own.
  Sides s;
  s.f = f;
  s.writes = {constants, {uint32_t(device), 8}, {stack_pointer + 48, 8}};
  s.work = count * 16;
  s.native = [&](PPCContext& c) { native(c); };
  s.differs = [](const PPCContext& a, const PPCContext& b) -> const char* { return a.r3.u64 != b.r3.u64 ? "r3" : nullptr; };
  s.call = fmt::format("device {:016X} first {} constants {:08X} count {} dirty {:016X}", device, first, source, count, dirty);
  RunSides(mode, s, ctx, base, original);
}

void RunD3dStateSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original, D3dStateNative native,
                     D3dStateWrites writes, bool keeps_r3) {
  const uint32_t f = uint32_t(function);
  const Mode mode = SlowMode(f, base);
  if (mode == Mode::kNative) {
    if (!native(g_memory, ctx)) original(ctx, base);
    return;
  }
  std::vector<any360::xdk::Range> written;
  if (mode == Mode::kOriginal || !writes(ctx, written)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The device is the rendering thread's own.
  Sides s;
  s.f = f;
  s.writes = std::move(written);
  s.work = 0;
  for (const any360::xdk::Range& r : s.writes) s.work += r.size;
  s.native = [native](PPCContext& c) { native(g_memory, c); };
  s.differs = [keeps_r3](const PPCContext& a, const PPCContext& b) -> const char* {
    return keeps_r3 && a.r3.u64 != b.r3.u64 ? "r3" : nullptr;
  };
  s.call = fmt::format("device {:08X} r4 {:X} r5 {:X} r6 {:X} r7 {:X}", ctx.r3.u32, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                       ctx.r7.u64);
  RunSides(mode, s, ctx, base, original);
}

void RunFillSlow(PPCContext& ctx, uint8_t* base, GuestFunction original, uint32_t floats) {
  constexpr uint32_t f = uint32_t(Function::kFill);
  const Mode mode = SlowMode(f, base);
  const uint32_t block = ctx.r3.u32;
  const auto native = [=](PPCContext& c) {
    uint32_t bits;
    if (!any360::xdk::XAudioFill(g_memory, block, floats, bits)) return false;
    c.fpscr.disableFlushMode();
    c.f0.f64 = double(std::bit_cast<float>(bits));
    return true;
  };
  if (mode == Mode::kNative) {
    if (!native(ctx)) original(ctx, base);
    return;
  }
  if (mode == Mode::kOriginal || !any360::xdk::XAudioFillAccepts(block, floats)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The block is the audio thread's own.
  Sides s;
  s.f = f;
  s.writes = {{block, 8 + 4 * floats}};
  s.work = floats * 4;
  s.native = [&](PPCContext& c) { native(c); };
  s.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    if (a.r3.u64 != b.r3.u64) return "r3";
    if (a.f0.u64 != b.f0.u64) return "f0";
    return nullptr;
  };
  s.call = fmt::format("block {:08X} floats {}", block, floats);
  RunSides(mode, s, ctx, base, original);
}

void RunInflateFastSlow(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t f = uint32_t(Function::kInflateFast);
  const Mode mode = SlowMode(f, base);
  const any360::xdk::InflateFastCall call{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  // The native side: the replacement, and the function's own code where it stops.
  const auto native = [=](PPCContext& c) {
    uint32_t result = 0;
    if (any360::xdk::IsPlainMemory(c.r1.u32 - 88, 88) &&
        any360::xdk::InflateFast(g_memory, call, result) == any360::xdk::InflateOutcome::kDone) {
      SaveGprLr22(c);
      c.r3.u64 = result;
      return;
    }
    original(c, base);
  };
  if (mode == Mode::kNative) {
    native(ctx);
    return;
  }
  // What either side may write: the window, the state's bit buffer and write pointer, the stream's input fields and
  // message, the register save. A call whose state cannot be read that far is the function's own.
  const uint32_t s = call.s, z = call.z, r1 = ctx.r1.u32;
  const bool readable = any360::xdk::IsPlainMemory(s, 56) && any360::xdk::IsPlainMemory(z, 28) &&
                        any360::xdk::IsPlainMemory(r1 - 88, 88);
  const uint32_t window = readable ? any360::xdk::detail::GuestLoad32(g_memory, s + 40) : 0;
  const uint32_t end = readable ? any360::xdk::detail::GuestLoad32(g_memory, s + 44) : 0;
  const uint32_t avail_in = readable ? any360::xdk::detail::GuestLoad32(g_memory, z + 4) : 0;
  if (mode == Mode::kOriginal || !readable || window >= end || end - window > kMostChecked ||
      !any360::xdk::IsPlainMemory(window, end - window)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Sides sides;
  sides.f = f;
  sides.writes = {{window, end - window}, {s + 28, 8}, {s + 52, 4}, {z, 12}, {z + 24, 4}, {r1 - 88, 88}};
  sides.work = avail_in;
  sides.native = native;
  sides.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    if (a.r3.u64 != b.r3.u64) return "r3";
    if (a.r12.u32 != b.r12.u32) return "r12";
    return nullptr;
  };
  sides.call = fmt::format("bl {} bd {} tl {:08X} td {:08X} s {:08X} z {:08X} window {:08X}-{:08X} in {}", call.bl,
                           call.bd, call.tl, call.td, s, z, window, end, avail_in);
  RunSides(mode, sides, ctx, base, original);
}

void RunPngFilterRowSlow(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t f = uint32_t(Function::kPngFilterRow);
  const Mode mode = SlowMode(f, base);
  const uint32_t info = ctx.r4.u32, row = ctx.r5.u32, prev = ctx.r6.u32, filter = ctx.r7.u32, r1 = ctx.r1.u32;
  const auto native = [=](PPCContext& c) {
    if (any360::xdk::IsPlainMemory(r1 - 112, 112) && any360::xdk::PngReadFilterRow(g_memory, info, row, prev, filter)) {
      SaveGprLr(c, 29);
      any360::xdk::detail::GuestStore32(g_memory, r1 - 112, r1);
      return;
    }
    original(c, base);
  };
  if (mode == Mode::kNative) {
    native(ctx);
    return;
  }
  // What either side may write: the row (and row[0] on a bad filter), the frame below r1.
  const uint32_t rowbytes = any360::xdk::IsPlainMemory(info, 12) ? any360::xdk::detail::GuestLoad32(g_memory, info + 4) : 0;
  if (mode == Mode::kOriginal || !any360::xdk::IsPlainMemory(info, 12) || rowbytes > kMostChecked ||
      !any360::xdk::IsPlainMemory(row, std::max<uint32_t>(rowbytes, 1)) || !any360::xdk::IsPlainMemory(r1 - 112, 112)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Sides sides;
  sides.f = f;
  sides.writes = {{row, std::max<uint32_t>(rowbytes, 1)}, {r1 - 112, 112}};
  sides.work = rowbytes;
  sides.native = native;
  sides.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    if (a.r12.u32 != b.r12.u32) return "r12";
    return nullptr;
  };
  sides.call = fmt::format("filter {} row {:08X} prev {:08X} rowbytes {} depth {}", filter, row, prev, rowbytes,
                           any360::xdk::IsPlainMemory(info, 12) ? *g_memory.Host(info + 11) : 0);
  RunSides(mode, sides, ctx, base, original);
}

void RunCrtMathSlow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original,
                    const any360::xdk::CrtTrigBuild& trig, const any360::xdk::CrtAsinBuild& asin) {
  const uint32_t f = uint32_t(function);
  const Mode mode = SlowMode(f, base);
  const uint32_t r1 = ctx.r1.u32;
  const auto native = [=](PPCContext& c) {
    double result = 0;
    const bool taken = function == Function::kSin   ? any360::xdk::CrtSin(g_memory, trig, r1, c.f1.f64, result)
                       : function == Function::kCos ? any360::xdk::CrtCos(g_memory, trig, r1, c.f1.f64, result)
                                                    : any360::xdk::CrtAsin(g_memory, asin, c.f1.f64, result);
    if (taken) {
      c.f1.f64 = result;
      return;
    }
    original(c, base);
  };
  if (mode == Mode::kNative) {
    native(ctx);
    return;
  }
  if (mode == Mode::kOriginal || !any360::xdk::IsPlainMemory(r1 - 16, 16)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // What either side may write: the two words below r1 (asin none); what it leaves: f1.
  Sides sides;
  sides.f = f;
  sides.writes = {{r1 - 16, 16}};
  sides.work = 1;
  sides.native = native;
  sides.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    return a.f1.u64 != b.f1.u64 ? "f1" : nullptr;
  };
  sides.call = fmt::format("f1 {:016X} ({})", ctx.f1.u64, ctx.f1.f64);
  RunSides(mode, sides, ctx, base, original);
}

void XAudioReverb(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const auto native = [&](PPCContext& c, const any360::xdk::XAudioReverbPlan* known) {
    any360::xdk::XAudioReverbPlan plan;
    if (known || any360::xdk::XAudioReverbPlanned(g_memory, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, plan)) {
      uint32_t csr = c.fpscr.csr;
      any360::xdk::XAudioReverb(g_memory, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, csr);
      c.fpscr.csr = csr;
      return true;
    }
    original(c, base);
    return false;
  };
  if (g_mode == Mode::kNative) [[likely]] {
    native(ctx, nullptr);
    return;
  }
  const uint32_t f = uint32_t(Function::kXAudioReverb);
  const Mode mode = SlowMode(f, base);
  any360::xdk::XAudioReverbPlan plan;
  if (mode == Mode::kNative) {
    native(ctx, nullptr);
    return;
  }
  if (mode == Mode::kOriginal ||
      !any360::xdk::XAudioReverbPlanned(g_memory, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, plan)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // What either side leaves for its caller: the state, the four outputs, the caller's word; registers: none (the
  // nonvolatile ones come back as they were).
  Sides sides;
  sides.f = f;
  sides.writes.assign(std::begin(plan.writes), std::end(plan.writes));
  sides.work = 1;
  sides.native = [native, plan](PPCContext& c) { native(c, &plan); };
  sides.differs = [](const PPCContext&, const PPCContext&) -> const char* { return nullptr; };
  sides.call = fmt::format("state {:08X}, inputs {:08X} {:08X}", ctx.r3.u32, plan.inputs[0], plan.inputs[1]);
  RunSides(mode, sides, ctx, base, original);
}

void RunCrtMath2Slow(Function function, PPCContext& ctx, uint8_t* base, GuestFunction original,
                     const any360::xdk::CrtAtan2Build& atan2, const any360::xdk::CrtPowBuild& pow) {
  const uint32_t f = uint32_t(function);
  const Mode mode = SlowMode(f, base);
  const uint32_t r1 = ctx.r1.u32;
  const auto native = [=](PPCContext& c) {
    double result = 0;
    const bool taken = function == Function::kAtan2
                           ? any360::xdk::CrtAtan2(g_memory, atan2, r1, c.f1.f64, c.f2.f64, result)
                           : any360::xdk::CrtPow(g_memory, pow, r1, c.f1.f64, c.f2.f64, result);
    if (taken) {
      c.f1.f64 = result;
      return;
    }
    original(c, base);
  };
  if (mode == Mode::kNative) {
    native(ctx);
    return;
  }
  if (mode == Mode::kOriginal || !any360::xdk::IsPlainMemory(r1 + 16, 16)) {
    original(ctx, base);
    if (mode != Mode::kOriginal) g_declined_by[f].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // What either side may write: the caller's two parameter words; what it leaves: f1.
  Sides sides;
  sides.f = f;
  sides.writes = {{r1 + 16, 16}};
  sides.work = 1;
  sides.native = native;
  sides.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    return a.f1.u64 != b.f1.u64 ? "f1" : nullptr;
  };
  sides.call = fmt::format("f1 {:016X} ({}), f2 {:016X} ({})", ctx.f1.u64, ctx.f1.f64, ctx.f2.u64, ctx.f2.f64);
  RunSides(mode, sides, ctx, base, original);
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
