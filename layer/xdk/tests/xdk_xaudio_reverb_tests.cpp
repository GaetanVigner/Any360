// XAudio's I3DL2 reverb (any360/xdk/xaudio_reverb.h) against its model (any360/xdk/xaudio_reverb_reference.h): one
// frame at a time on states made here (no title's data: random samples and coefficients, random delay taps, the
// crossfades' targets moved, positions advanced by running the effect), every byte of the state, the four outputs,
// the stack below r1 and the caller's word compared, over sequences of frames (the state carried on) in the three
// input modes; and reverbs dying out in silence (products in the denormal range), entered with the host's flush mode
// off and on. The guest memory is a sparse reservation of the 32-bit space, so that the few constants the effect
// reads from its build's image sit at their addresses (made-up values of the same kind: 1, 2, 10000 ...).
// Declined: memory that is not plain.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "any360/xdk/xaudio_reverb.h"
#include "any360/xdk/xaudio_reverb_reference.h"
#include "xdk_test.h"
#include "xdk_xaudio_reverb_state.h"

namespace {

namespace xdk = any360::xdk;
using namespace xaudio_reverb_test;

uint64_t g_frames = 0;

// One frame on both sides, each entered with the host's floating-point control word `entry`; true when every byte
// the frame may write is the same.
bool Frame(Side& native, Side& model, uint32_t entry) {
  // What the call may leave changed, from its plan (the in-game check compares those ranges only), and its frame.
  xdk::XAudioReverbPlan plan;
  CHECK(xdk::XAudioReverbPlanned(native.m, kStack, kState, kPointers, kPointers + 0x40, plan));
  const std::vector<uint8_t> work_before(native.At(kWorkBegin), native.At(kWorkEnd));
  _mm_setcsr(entry);
  uint32_t csr = entry;
  xdk::XAudioReverb(native.m, kStack, kState, kPointers, kPointers + 0x40, csr);
  _mm_setcsr(entry);
  for (uint32_t at = kWorkBegin; at < kWorkEnd; ++at) {
    bool planned = at >= kStack - xdk::kXAudioReverbStackBelow && at < kStack;
    for (const xdk::Range& w : plan.writes) planned = planned || (at >= w.at && at < w.at + w.size);
    if (!planned && *native.At(at) != work_before[at - kWorkBegin]) {
      if (xdk_test::g_failures < 3) std::printf("  written outside its plan: %08X\n", at);
      CHECK(false);
      break;
    }
  }
  xdk::reference::xaudio_reverb::Ctx ctx;  // its control word read from the host: `entry`
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = kState;
  ctx.r4.u64 = kPointers;
  ctx.r5.u64 = kPointers + 0x40;
  xdk::reference::xaudio_reverb::sub_82A4DCF0(ctx, model.m);
  _mm_setcsr(ctx.fpscr.csr);
  ++g_frames;
  bool same = csr == ctx.fpscr.csr && ctx.r1.u32 == kStack;
  if (!same && xdk_test::g_failures < 3) std::printf("  csr %08X / %08X, r1 %08X\n", csr, ctx.fpscr.csr, ctx.r1.u32);
  for (uint32_t at = kWorkBegin; at < kWorkEnd; at += 4) {
    if (std::memcmp(native.At(at), model.At(at), 4) != 0) {
      if (xdk_test::g_failures < 3) {
        uint32_t a, b;
        std::memcpy(&a, native.At(at), 4);
        std::memcpy(&b, model.At(at), 4);
        std::printf("  first difference at %08X: native %08X, model %08X\n", at, std::byteswap(a), std::byteswap(b));
      }
      same = false;
      break;
    }
  }
  return same;
}

}  // namespace

int main() {
  Side native, model;
  std::mt19937 rng(0x7E5E);
  uint32_t sequences = 0;
  const uint32_t plain = _mm_getcsr() & ~xdk::ppc_model::Fpscr::kFlushMask;
  for (uint32_t mode = 0; mode < 3; ++mode) {
    for (int s = 0; s < 6; ++s) {
      NewState(native, model, rng, mode, s % 2 == 1);
      ++sequences;
      for (int f = 0; f < 12; ++f) {
        NewInputs(native, model, rng);
        const bool same = Frame(native, model, plain);
        CHECK(same);
        if (!same) {
          std::printf("  mode %u, sequence %d, frame %d: native and model differ\n", mode, s, f);
          break;
        }
      }
    }
  }
  // Dying out: tiny delay lines and coefficients, a few frames of tiny samples then silence, entered with the flush
  // mode off and on (the title's code flushes in its vector steps only, so it is entered both ways in game).
  uint64_t denormals = 0;
  for (const uint32_t entry : {plain, plain | xdk::ppc_model::Fpscr::kFlushMask}) {
    for (int s = 0; s < 4; ++s) {
      NewState(native, model, rng, s % 3, s % 2 == 1, 1e-36f);  // about 1% of its floats denormal
      ++sequences;
      for (int f = 0; f < 12; ++f) {
        NewInputs(native, model, rng, f < 3 ? 1e-37f : 0.0f);
        const bool same = Frame(native, model, entry);
        CHECK(same);
        if (!same) {
          std::printf("  dying out, entry %08X, sequence %d, frame %d: native and model differ\n", entry, s, f);
          break;
        }
        for (uint32_t at = kState; at < kState + xdk::kXAudioReverbStateSize; at += 4) {
          uint32_t v;
          std::memcpy(&v, native.At(at), 4);
          v = std::byteswap(v);
          denormals += (v & 0x7F800000) == 0 && (v & 0x007FFFFF) != 0;
        }
      }
    }
  }
  _mm_setcsr(plain);
  // The effect reads them from its delay lines and coefficients (the outputs, written by vector steps, are flushed).
  CHECK(denormals > 0);
  // Planned: what one call writes, from its pointers; declined: memory that is not plain.
  {
    xdk::XAudioReverbPlan plan;
    CHECK(xdk::XAudioReverbPlanned(native.m, kStack, kState, kPointers, kPointers + 0x40, plan));
    CHECK(plan.outputs[2] == kOutputs + 0x3000 && plan.writes[0].at == kState);
    CHECK(!xdk::XAudioReverbPlanned(native.m, kStack, 0x7EFF0000, kPointers, kPointers + 0x40, plan));
    CHECK(!xdk::XAudioReverbPlanned(native.m, 0x7F002000, kState, kPointers, kPointers + 0x40, plan));
    Put32(native.m, kPointers + 0x4C, 0x7F000000);  // the fourth output in the GPU's register window
    CHECK(!xdk::XAudioReverbPlanned(native.m, kStack, kState, kPointers, kPointers + 0x40, plan));
  }
  std::printf("xdk_xaudio_reverb_tests: %llu frames compared with the model (state, outputs, stack) in %u sequences; "
              "%llu denormal words left in the dying-out states\n",
              (unsigned long long)g_frames, sequences, (unsigned long long)denormals);
  return xdk_test::Finish("xdk_xaudio_reverb_tests");
}
