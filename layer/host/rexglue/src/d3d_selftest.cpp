#include "any360/host/d3d_selftest.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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

any360::xdk::Memory g_mem;

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
  std::vector<Region> regions;
  std::vector<uint8_t> at_start;
  Result r;
  uint64_t own_ticks = 0, mine_ticks = 0, timed = 0;
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
  run.own_ticks = run.mine_ticks = run.timed = 0;
  run.rng.seed(0x5EED0000u ^ uint32_t(std::hash<std::string>{}(c.name)));
  run.s = CallSetup{};
  run.s.base = run.base, run.s.device = run.device, run.s.scratch = run.scratch, run.s.rng = &run.rng;
  // The call's stack: in the self-test's own memory past the scratch (seed.r1). The 4 KB from its pointer up (the
  // caller's frames) are compared; below it is the callee's own, which the original fills and the replacement not.
  run.s.stack = (uint32_t(run.seed.r1.u32) - 0x1000u) & ~0xFu;
  run.regions = {{run.device, kDeviceBytes}, {run.scratch, kScratchBytes}, {run.s.stack, 0x1000}};
  run.at_start = Read(run.regions);
  REXLOG_INFO("Any360 D3D self-test: {}", c.name);
}

// One call of the case: compared (phase 0) or timed (phase 1).
void OneCall(Runner& run, const Case& c) {
  uint8_t* base = run.base;
  CallSetup& s = run.s;
  const std::vector<Region>& regions = run.regions;
  Randomize(run);
  PPCContext call = run.seed;
  c.setup(s, call);
  call.r1.u64 = s.stack;
  const std::vector<uint8_t> before = Read(regions);
  if (run.phase == 0) {
    Result& r = run.r;
    PPCContext own = call;
    c.original(own, base);
    const std::vector<uint8_t> own_bytes = Read(regions);
    Write(regions, before);
    PPCContext mine = call;
    ++r.calls;
    if (!c.native(mine, base)) {
      ++r.declined;
      Write(regions, before);
      return;
    }
    const std::vector<uint8_t> mine_bytes = Read(regions);
    Write(regions, before);
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
  // Timed, in alternating order (the second side finds the memory in the cache); a declined call is not timed.
  PPCContext own = call, mine = call;
  uint64_t own_one = 0, mine_one = 0;
  bool taken = false;
  for (int side = 0; side < 2; ++side) {
    const bool original_now = (side == 0) == bool(run.i & 1);
    const uint64_t a = Ticks();
    if (original_now) {
      c.original(own, base);
      own_one = Ticks() - a;
    } else {
      taken = c.native(mine, base);
      mine_one = Ticks() - a;
    }
    Write(regions, before);
  }
  if (taken) run.own_ticks += own_one, run.mine_ticks += mine_one, ++run.timed;
}

void EndCase(Runner& run) {
  Write(run.regions, run.at_start);
  Result& r = run.r;
  if (run.timed) {
    r.original_ns = double(run.own_ticks) * NsPerTick() / double(run.timed);
    r.native_ns = double(run.mine_ticks) * NsPerTick() / double(run.timed);
  }
  char line[512];
  std::snprintf(line, sizeof line,
                "%-28s %6llu calls, %llu declined, %llu different; original %.1f ns, native %.1f ns (%.2fx)%s%s\n",
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
const any360::xdk::Memory& SelfTestMemory() { return g_mem; }

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
