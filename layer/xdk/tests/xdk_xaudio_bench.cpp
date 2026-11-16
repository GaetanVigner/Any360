// Timings of the XAudio replacements (any360/xdk/xaudio.h) against their models: nanoseconds a call by count of
// samples. Not a test. The figures that decide a binding are the title's own, with --any360_xdk_bench (the
// function's recompiled code against the replacement, on the title's calls): docs/NATIVE_FUNCTIONS.md.
// Run: xdk_xaudio_bench [repeats]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "xdk_bench.h"

#include <immintrin.h>

#include "any360/xdk/xaudio.h"
#include "any360/xdk/xaudio_reference.h"

namespace {

namespace xdk = any360::xdk;

template <class F>
double NsPerCall(F&& f, int calls) {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < calls; ++i) f();
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  xdk_bench::Allocated& a = xdk_bench::g_allocated;
  a.calls += uint64_t(calls), a.bytes += spent.bytes, a.count += spent.count;
  a.max_bytes_a_call = std::max(a.max_bytes_a_call, double(spent.bytes) / double(calls));
  return ns;
}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  std::vector<uint8_t> arena(1u << 20, 0);
  for (size_t i = 0; i < arena.size(); i += 4) arena[i] = 0x3C, arena[i + 1] = uint8_t(i);  // small floats
  xdk::Memory memory;
  memory.base = arena.data();
  _mm_setcsr(_mm_getcsr() | 0x8040);
  if (!xdk::detail::HostHasFma()) {
    std::printf("xdk_xaudio_bench: this host has no FMA3: the replacement declines every call\n");
    return 0;
  }
  for (int r = 0; r < repeats; ++r) {
    for (uint32_t samples : {64u, 256u, 512u, 1024u}) {
      xdk::VmxRegisters x;
      const auto call = [&] {
        x.gpr[1] = 0xFFF00, x.gpr[3] = 0x10000, x.gpr[4] = 0x40000, x.gpr[5] = 0x80000, x.gpr[6] = 0x80100;
        x.gpr[7] = samples;
      };
      const int calls = 2000000 / samples;
      const double native = NsPerCall([&] { call(); xdk::XAudioMixRamp(memory, x); }, calls);
      const double model = NsPerCall([&] { call(); xdk::reference::XAudioMixRamp(memory, x); }, calls / 8 + 1);
      std::printf("mix ramp %5u samples: replacement %8.1f ns (%.2f ns a sample), model %9.1f ns (%.1fx)\n", samples, native,
                  native / samples, model, model / native);
    }
  }
  xdk_bench::PrintAllocated("xdk_xaudio_bench");
  return 0;
}
