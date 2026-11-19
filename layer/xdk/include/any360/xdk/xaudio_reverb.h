// XAudio's I3DL2 reverb, native: the effect's whole processing of one frame (256 samples of two input channels into
// four output channels), sub_82A4DCF0 in Crackdown's build with its ten stages (delay lines read across a crossfade,
// early reflections, all-pass and comb filters, the late reverb's mix; scalar single precision and a few vector
// multiply-adds).
//
// First version: the function's own statements (any360/xdk/xaudio_reverb_native.h, made by tools/ppc_model.py
// --native from the recompiled code) compiled as one leaf, every stage inlined and the registers local to the call,
// so the same operations in the same order give the same bits; what it saves is the recompiled code's traffic
// through the shared register context. Its model is any360/xdk/xaudio_reverb_reference.h (the same tool, unchanged
// statements); the title's own code is checked against it in game.
//
// Its hot loop (loc_82A4E774: 256 samples through the filters) keeps its stack slots and its memory at invariant
// addresses (the state's coefficients and filter words) in locals behind a check, the plain loop beside it
// (--promote loc_82A4E774 --promote-memory loc_82A4E774; its filters' double-buffer words picked by a selector):
// about 77 -> 48 us a frame; with the whole context in host registers (no helper left out of line, no vector lane
// reached by a variable index) 27 us. Regenerated with
//   python -I tools/ppc_model.py --native --promote loc_82A4E774 --promote-memory loc_82A4E774
//       <the title's generated dir> xaudio_reverb layer/xdk/include/any360/xdk/xaudio_reverb_native.h 82A4DCF0
//
// The state (r3) holds everything the effect keeps between frames, in line: delay buffers, positions, coefficients,
// and last a 256-sample buffer one frame leaves for the next (0x4B9D0 bytes in all). r4 points to the two input
// channels' buffers, r5 to the output channels' (it writes the first, second, fourth and fifth); the frame uses about
// 18 KB of stack below r1 and one word of the caller's at r1 + 20 (the state's address).
// Constants come from the build's image (a few floats; 0x82000A8C-0x82BA2F8F in Crackdown's). Declines (false,
// nothing written) memory that is not plain. Allocates nothing.
#pragma once

#include <cstdint>

#include "any360/xdk/crt.h"
#include "any360/xdk/xaudio_reverb_native.h"

namespace any360::xdk {

inline constexpr uint32_t kXAudioReverbStateSize = 0x4B9D0;
inline constexpr uint32_t kXAudioReverbStackBelow = 0x4800;  // what the frame and its stages use below r1
inline constexpr uint32_t kXAudioReverbFrameBytes = 256 * 4;

// What one call reads and writes, from its registers and the pointers they lead to.
struct XAudioReverbPlan {
  uint32_t inputs[2] = {}, outputs[4] = {};
  // What it leaves that its caller may read: the state, the four outputs, the caller's word at r1 + 20. Its frame
  // below r1 is not one: the native version saves its own registers there, not the caller's (nothing reads them after
  // the return, and the caller's registers come back the same either way).
  Range writes[6] = {};
};

inline bool XAudioReverbPlanned(const Memory& memory, uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5,
                                XAudioReverbPlan& plan) {
  if (!IsPlainMemory(r3, kXAudioReverbStateSize) || !IsPlainMemory(r1 - kXAudioReverbStackBelow,
                                                                   kXAudioReverbStackBelow + 32) ||
      !IsPlainMemory(r4, 8) || !IsPlainMemory(r5, 20)) {
    return false;
  }
  const auto word = [&](uint32_t at) {
    uint32_t v;
    std::memcpy(&v, memory.Host(at), 4);
    return std::byteswap(v);
  };
  // Which input feeds which channel: the state's word at +100 (1: the first for both, 2: the second for both).
  if (!IsPlainMemory(r3 + 100, 4)) return false;
  const uint32_t mode = word(r3 + 100);
  plan.inputs[0] = mode == 2 ? word(r4 + 4) : word(r4);
  plan.inputs[1] = mode == 1 ? word(r4) : word(r4 + 4);
  plan.outputs[0] = word(r5), plan.outputs[1] = word(r5 + 4), plan.outputs[2] = word(r5 + 12),
  plan.outputs[3] = word(r5 + 16);
  for (uint32_t at : plan.inputs) {
    if (!IsPlainMemory(at, kXAudioReverbFrameBytes)) return false;
  }
  for (uint32_t at : plan.outputs) {
    if (!IsPlainMemory(at, kXAudioReverbFrameBytes)) return false;
  }
  plan.writes[0] = {r3, kXAudioReverbStateSize};
  for (int i = 0; i < 4; ++i) plan.writes[1 + i] = {plan.outputs[i], kXAudioReverbFrameBytes};
  plan.writes[5] = {r1 + 20, 4};
  return true;
}

namespace detail {
// The leaf compiled twice: with the processor's fused multiply-add when it has one (FMA3), and without. Its fmadd and
// fmsub are std::fma, which the title's build (and the model) compute by the C library's function: rounded once,
// the same bits as the instruction, at a fraction of the cost (164 of them in the effect's code).
// The registers and the memory's description are copied into locals of the leaf: a context reached through a
// reference is memory the leaf's guest stores (through byte pointers, which may alias anything) could change, so every
// register would be read again from it after every store; a local whose address never leaves the leaf stays in host
// registers.
__attribute__((target("fma"))) inline void XAudioReverbLeafFma(ppc_model::Ctx& ctx, const Memory& memory) {
  ppc_model::Ctx local = ctx;
  const Memory m = memory;
  xaudio_reverb_native::sub_82A4DCF0(local, m);
  ctx = local;
}
inline void XAudioReverbLeaf(ppc_model::Ctx& ctx, const Memory& memory) {
  ppc_model::Ctx local = ctx;
  const Memory m = memory;
  xaudio_reverb_native::sub_82A4DCF0(local, m);
  ctx = local;
}
}  // namespace detail

// One frame. `csr` the host's floating-point control word as the caller keeps it (its flush bit is the guest's
// mode); it comes back as the function leaves it.
inline void XAudioReverb(const Memory& memory, uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t& csr) {
  ppc_model::Ctx ctx;
  ctx.r1.u64 = r1;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.fpscr.csr = csr;
  if (ppc_model::detail::kHostHasFma) {
    detail::XAudioReverbLeafFma(ctx, memory);
  } else {
    detail::XAudioReverbLeaf(ctx, memory);
  }
  csr = ctx.fpscr.csr;
}

}  // namespace any360::xdk
