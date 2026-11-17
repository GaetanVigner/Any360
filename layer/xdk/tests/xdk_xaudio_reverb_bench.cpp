// Timings of XAudio's I3DL2 reverb (any360/xdk/xaudio_reverb.h) against its model: microseconds a frame (256
// samples, two channels in, four out) on a state made as in xdk_xaudio_reverb_tests, and what it allocates. The first
// version is the generated leaf itself (tools/ppc_model.py --native); hand-written stages, if any come, are timed
// against it here. Not a test: the figures that decide a binding are the title's own (--any360_xdk_bench).
// Run: xdk_xaudio_reverb_bench [repeats]
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "xdk_bench.h"
#include "xdk_xaudio_reverb_state.h"

#include "any360/xdk/xaudio_reverb.h"
#include "any360/xdk/xaudio_reverb_reference.h"

int main(int argc, char** argv) {
  namespace xdk = any360::xdk;
  using namespace xaudio_reverb_test;
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  Side side;
  std::mt19937 rng(5);
  NewState(side, side, rng, 0, true);
  NewInputs(side, side, rng);
  const std::vector<uint8_t> start(side.At(kState), side.At(kState) + xdk::kXAudioReverbStateSize);
  const int frames = 2000;
  const auto run = [&](auto&& frame) {
    std::memcpy(side.At(kState), start.data(), start.size());
    const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) frame();
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / frames;
    const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
    xdk_bench::Allocated& a = xdk_bench::g_allocated;
    a.calls += frames, a.bytes += spent.bytes, a.count += spent.count;
    a.max_bytes_a_call = std::max(a.max_bytes_a_call, double(spent.bytes) / frames);
    return us;
  };
  for (int r = 0; r < repeats; ++r) {
    const double native = run([&] {
      uint32_t csr = _mm_getcsr();
      xdk::XAudioReverb(side.m, kStack, kState, kPointers, kPointers + 0x40, csr);
      _mm_setcsr(csr);
    });
    const double model = run([&] {
      xdk::reference::xaudio_reverb::Ctx ctx;
      ctx.r1.u64 = kStack;
      ctx.r3.u64 = kState;
      ctx.r4.u64 = kPointers;
      ctx.r5.u64 = kPointers + 0x40;
      xdk::reference::xaudio_reverb::sub_82A4DCF0(ctx, side.m);
      _mm_setcsr(ctx.fpscr.csr);
    });
    std::printf("reverb %.2f us a frame, model %.2f (%.2fx)\n", native, model, model / native);
  }
  xdk_bench::PrintAllocated("xdk_xaudio_reverb_bench");
  return 0;
}
