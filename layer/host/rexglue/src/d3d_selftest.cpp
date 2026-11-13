#include "any360/host/d3d_selftest.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "any360/host/d3d_cases.h"
#include "any360/xdk/crt.h"

REXCVAR_DEFINE_BOOL(any360_d3d_selftest, false, "GPU",
                    "Any360: at the first D3D call, run the title's D3D functions against their native replacements "
                    "on generated inputs (compared, then timed), write d3d_selftest.txt and end the process");
REXCVAR_DEFINE_INT32(any360_d3d_selftest_slice_ms, 3, "GPU",
                     "Any360: the D3D self-test's work a frame, in milliseconds (the title keeps running)");
REXCVAR_DEFINE_STRING(any360_d3d_selftest_only, "", "GPU",
                      "Any360: the D3D self-test runs only the cases whose name contains one of these texts, separated by commas "
                      "(empty: all)");
REXCVAR_DEFINE_INT32(any360_d3d_selftest_after, 2000, "GPU",
                     "Any360: the D3D self-test starts at this call of the D3D function that triggers it (the "
                     "title draws by then)");

namespace any360::host::rexglue::d3d {

namespace {

std::vector<Case>& Cases() {
  static std::vector<Case> cases;
  return cases;
}

uint64_t Ticks() {
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double NsPerTick() {
  static const double rate = [] {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = Ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t c1 = Ticks();
    const auto t1 = std::chrono::steady_clock::now();
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(std::max<uint64_t>(c1 - c0, 1));
  }();
  return rate;
}

any360::xdk::Memory& g_mem = g_selftest_memory;

double Median(std::vector<uint32_t>& v) {
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return double(v[v.size() / 2]);
}
// What two reads of the clock in a row cost, in ticks (the median of many).
double ClockTicks() {
  static const double ticks = [] {
    std::vector<uint32_t> v(4001);
    for (uint32_t& t : v) {
      const uint64_t a = Ticks();
      t = uint32_t(Ticks() - a);
    }
    return Median(v);
  }();
  return ticks;
}

// A guest range's bytes.
struct Region {
  uint32_t at, bytes;
};
std::vector<uint8_t> Read(const std::vector<Region>& regions) {
  size_t total = 0;
  for (const Region& r : regions) total += r.bytes;
  std::vector<uint8_t> out(total);
  size_t at = 0;
  for (const Region& r : regions) std::memcpy(out.data() + at, g_mem.Host(r.at), r.bytes), at += r.bytes;
  return out;
}
void Write(const std::vector<Region>& regions, const std::vector<uint8_t>& bytes) {
  size_t at = 0;
  for (const Region& r : regions) std::memcpy(g_mem.Host(r.at), bytes.data() + at, r.bytes), at += r.bytes;
}
// The first byte that differs: its region and offset, or false.
bool FirstDifference(const std::vector<Region>& regions, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                     uint32_t& address) {
  size_t at = 0;
  for (const Region& r : regions) {
    for (uint32_t i = 0; i < r.bytes; ++i) {
      if (a[at + i] != b[at + i]) {
        address = r.at + i;
        return true;
      }
    }
    at += r.bytes;
  }
  return false;
}

const char* R3Differs(const PPCContext& a, const PPCContext& b) { return a.r3.u64 != b.r3.u64 ? "r3" : nullptr; }

struct Result {
  std::string name;
  uint64_t calls = 0, declined = 0, different = 0;
  double original_ns = 0, native_ns = 0;
  std::string first_difference;
};

// The self-test runs in slices of a few milliseconds, one a frame (at the trigger's calls), so that the title's other
// threads, which keep running, never see the D3D thread stop for seconds.
struct Runner {
  bool started = false;
  uint8_t* base = nullptr;
  uint32_t title_device = 0, device = 0, scratch = 0;
  PPCContext seed{};
  std::string report;
  uint64_t last_slice_end = 0;
  // The case under way.
  size_t index = 0;
  bool in_case = false;
  int phase = 0;  // 0 compared, 1 timed
  uint32_t i = 0;
  std::mt19937_64 rng;
  CallSetup s;
  std::vector<Region> regions;   // compared
  std::vector<Region> restored;  // put back between the two sides: the compared ones and the stack below the call's
  std::vector<uint8_t> at_start;
  Result r;
  std::vector<uint32_t> own_ticks, mine_ticks;  // a timed call each
};
Runner g_run;

void Randomize(Runner& run) {
  uint8_t* p = g_mem.Host(run.scratch);
  for (uint32_t i = 0; i < kScratchBytes; i += 8) {
    const uint64_t v = run.rng();
    std::memcpy(p + i, &v, 8);
  }
}

void StartCase(Runner& run, const Case& c) {
  run.in_case = true, run.phase = 0, run.i = 0;
  run.r = Result{};
  run.r.name = c.name;
  run.own_ticks.clear(), run.mine_ticks.clear();
  run.rng.seed(0x5EED0000u ^ uint32_t(std::hash<std::string>{}(c.name)));
  run.s = CallSetup{};
  run.s.base = run.base, run.s.device = run.device, run.s.scratch = run.scratch, run.s.rng = &run.rng;
  // The call's stack: in the self-test's own memory past the scratch (seed.r1). The 4 KB from its pointer up (the
  // caller's frames) are compared; below it is the callee's own, which the original fills and the replacement not.
  run.s.stack = (uint32_t(run.seed.r1.u32) - 0x1000u) & ~0xFu;
  run.regions = {{run.device, kDeviceBytes}, {run.scratch, kScratchBytes}, {run.s.stack, 0x1000}};
  // Below the stack pointer: the callee's own, not compared, but each side finds it as the other did (a function
  // may read what an earlier call left there).
  run.restored = run.regions;
  run.restored.push_back({run.s.stack - 0x2000, 0x2000});
  run.at_start = Read(run.restored);
  REXLOG_INFO("Any360 D3D self-test: {}", c.name);
}

// The calls timed in a row, and the registers a call takes (put back before each).
constexpr int kRepeat = 8;
inline void RestoreArguments(PPCContext& ctx, const PPCContext& call) {
  ctx.r1 = call.r1, ctx.r3 = call.r3, ctx.r4 = call.r4, ctx.r5 = call.r5, ctx.r6 = call.r6;
  ctx.r7 = call.r7, ctx.r8 = call.r8, ctx.r9 = call.r9, ctx.r10 = call.r10;
}
// What putting the registers back kRepeat times costs, with the clock read twice, in ticks (the median of many).
double RepeatTicks(const PPCContext& call) {
  std::vector<uint32_t> v(2001);
  PPCContext ctx = call;
  for (uint32_t& t : v) {
    const uint64_t a = Ticks();
    for (int k = 0; k < kRepeat; ++k) {
      RestoreArguments(ctx, call);
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    t = uint32_t(Ticks() - a);
  }
  return Median(v);
}

// One call of the case: compared (phase 0) or timed (phase 1).
void OneCall(Runner& run, const Case& c) {
  uint8_t* base = run.base;
  CallSetup& s = run.s;
  const std::vector<Region>& regions = run.regions;
  const std::vector<Region>& restored = run.restored;
  Randomize(run);
  PPCContext call = run.seed;
  c.setup(s, call);
  call.r1.u64 = s.stack;
  const std::vector<uint8_t> before = Read(restored);
  if (run.phase == 0) {
    Result& r = run.r;
    PPCContext own = call;
    c.original(own, base);
    if (c.normalize) c.normalize(s, own);
    const std::vector<uint8_t> own_bytes = Read(regions);
    Write(restored, before);
    PPCContext mine = call;
    ++r.calls;
    if (!c.native(mine, base)) {
      ++r.declined;
      Write(restored, before);
      return;
    }
    if (c.normalize) c.normalize(s, mine);
    const std::vector<uint8_t> mine_bytes = Read(regions);
    Write(restored, before);
    uint32_t where = 0;
    const bool bytes_differ = FirstDifference(regions, own_bytes, mine_bytes, where);
    const char* reg = (c.differs ? c.differs : &R3Differs)(mine, own);
    if ((bytes_differ || reg) && !r.different++) {
      char text[200];
      std::snprintf(text, sizeof text, "call %u: %s%s%s at %08X (r3 %08X r4 %016llX r5 %08X r6 %08X r7 %016llX)", run.i,
                    bytes_differ ? "memory" : "", bytes_differ && reg ? " and " : "", reg ? reg : "",
                    bytes_differ ? where : 0, call.r3.u32, static_cast<unsigned long long>(call.r4.u64), call.r5.u32,
                    call.r6.u32, static_cast<unsigned long long>(call.r7.u64));
      r.first_difference = text;
    }
    return;
  }
  // Timed, in alternating order (the second side finds the memory in the cache), kRepeat calls in a row on the
  // memory each leaves (the clock's tick is coarse next to a call), the argument registers put back before each;
  // a sample where the replacement declined a call is not kept.
  PPCContext own = call, mine = call;
  uint64_t own_ticks = 0, mine_ticks = 0;
  bool taken = true;
  for (int side = 0; side < 2; ++side) {
    const bool original_now = (side == 0) == bool(run.i & 1);
    PPCContext& ctx = original_now ? own : mine;
    const uint64_t a = Ticks();
    for (int k = 0; k < kRepeat; ++k) {
      RestoreArguments(ctx, call);
      if (original_now) c.original(ctx, base);
      else taken = c.native(ctx, base) && taken;
    }
    (original_now ? own_ticks : mine_ticks) = Ticks() - a;
    Write(restored, before);
  }
  if (taken) run.own_ticks.push_back(uint32_t(own_ticks)), run.mine_ticks.push_back(uint32_t(mine_ticks));
}

void EndCase(Runner& run) {
  Write(run.restored, run.at_start);
  Result& r = run.r;
  // The median sample of each side (an interrupt in a call of 20 ns would weigh on a mean), less what reading the
  // clock and putting the registers back cost, a call.
  if (!run.own_ticks.empty()) {
    const double overhead = RepeatTicks(run.seed);
    r.original_ns = std::max(Median(run.own_ticks) - overhead, 0.0) * NsPerTick() / kRepeat;
    r.native_ns = std::max(Median(run.mine_ticks) - overhead, 0.0) * NsPerTick() / kRepeat;
  }
  char line[512];
  std::snprintf(line, sizeof line,
                "%-28s %6llu calls, %llu declined, %llu different; median original %.1f ns, native %.1f ns (%.2fx faster)%s%s\n",
                r.name.c_str(), static_cast<unsigned long long>(r.calls), static_cast<unsigned long long>(r.declined),
                static_cast<unsigned long long>(r.different), r.original_ns, r.native_ns,
                r.native_ns > 0 ? r.original_ns / r.native_ns : 0.0, r.first_difference.empty() ? "" : "; first: ",
                r.first_difference.c_str());
  run.report += line;
  run.in_case = false;
  ++run.index;
}

[[noreturn]] void Finish(const std::string& report) {
  for (size_t at = 0; at < report.size();) {
    const size_t end = report.find('\n', at);
    REXLOG_INFO("{}", report.substr(at, end - at));
    at = end + 1;
  }
  if (FILE* f = std::fopen("d3d_selftest.txt", "w")) {
    std::fputs(report.c_str(), f);
    std::fclose(f);
  }
  std::fflush(nullptr);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

bool Start(Runner& run, const PPCContext& ctx, uint8_t* base, uint32_t device) {
  g_mem.base = base;
  g_mem.physical_offset = uint32_t(rex::memory::GuestPtr<uint8_t*>(base, 0xE0000000u) - (base + 0xE0000000u));
  rex::system::KernelState* kernel = rex::system::kernel_state();
  // The self-test's own memory: a copy of the device (the title's other threads keep using the real one, which the
  // cases must not write or put back), the scratch, then 64 KB for the calls' stack.
  const uint32_t own = kernel ? kernel->memory()->SystemHeapAlloc(kDeviceBytes + kScratchBytes + 0x10000, 4096) : 0;
  char head[160];
  std::snprintf(head, sizeof head, "Any360 D3D self-test: %zu cases on a copy of device %08X\n", Cases().size(),
                device);
  run.report = head;
  if (!own) {
    run.report += "no memory for the self-test\n";
    return false;
  }
  std::memcpy(g_mem.Host(own), g_mem.Host(device), kDeviceBytes);
  run.started = true;
  run.base = base;
  run.title_device = device;
  run.device = own;
  run.scratch = own + kDeviceBytes;
  run.seed = ctx;
  run.seed.r1.u64 = run.scratch + kScratchBytes + 0x10000 - 0x100;
  return true;
}

}  // namespace

uint8_t* CallSetup::Host(uint32_t address) const { return g_mem.Host(address); }

bool Register(const Case& c) {
  Cases().push_back(c);
  return true;
}

void FirstDevice(const PPCContext& ctx, uint8_t* base, uint32_t device) {
  // Several of the title's threads make D3D calls: one at a time works on the self-test, the others go on.
  static std::mutex one_thread;
  std::unique_lock<std::mutex> lock(one_thread, std::try_to_lock);
  if (!lock.owns_lock()) return;
  Runner& run = g_run;
  if (!run.started) {
    if (!REXCVAR_GET(any360_d3d_selftest)) {
      g_device_calls_left = -1;  // off: never called again
      return;
    }
    // The first call only counts the calls to wait for.
    static bool counted = false;
    if (!counted) {
      counted = true;
      const int32_t after = REXCVAR_GET(any360_d3d_selftest_after);
      if (after > 1) {
        g_device_calls_left = after - 1;
        return;
      }
    }
    if (!Start(run, ctx, base, device)) Finish(run.report);
    // From here every call is a slice (g_device_calls_left stays 0).
  }
  // A slice of at most any360_d3d_selftest_slice_ms, then the frame's time for the title.
  const double ns = NsPerTick();
  const uint64_t now = Ticks();
  if (run.last_slice_end && double(now - run.last_slice_end) * ns < 16e6) return;
  const uint64_t budget = uint64_t(double(std::max(REXCVAR_GET(any360_d3d_selftest_slice_ms), 1)) * 1e6 / ns);
  while (Ticks() - now < budget) {
    const std::string only = REXCVAR_GET(any360_d3d_selftest_only);
    const auto wanted = [&](const std::string& name) {
      // Several texts, separated by commas: a case runs when its name contains one.
      for (size_t at = 0; at <= only.size();) {
        const size_t comma = std::min(only.find(',', at), only.size());
        if (comma > at && name.find(only.substr(at, comma - at)) != std::string::npos) return true;
        at = comma + 1;
      }
      return false;
    };
    while (run.index < Cases().size() && !run.in_case && !only.empty() && !wanted(Cases()[run.index].name)) ++run.index;
    if (run.index >= Cases().size()) Finish(run.report);
    const Case& c = Cases()[run.index];
    if (!run.in_case) StartCase(run, c);
    OneCall(run, c);
    if (++run.i >= c.calls) {
      run.i = 0;
      if (++run.phase == 2) EndCase(run);
    }
  }
  run.last_slice_end = Ticks();
}

}  // namespace any360::host::rexglue::d3d
