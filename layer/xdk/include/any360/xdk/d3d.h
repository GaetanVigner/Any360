// any360::xdk: the XDK's D3D runtime's waits for the GPU, made to sleep instead of spinning. The rule this follows:
// docs/NATIVE_FUNCTIONS.md.
#pragma once

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "crt.h"

namespace any360::xdk {

// The D3D runtime's upload of the shader constants a 64-bit mask marks as changed (named after what it does): r3
// the device, r4 the mask (bit 63 first), r5 the register index of the mask's first bit, r6 the constants (64 bytes,
// four vectors, a bit). For each run of set bits it writes into the command buffer a header
// ((words - 1) << 16 | index) and the run's constants, the header placed so that the constants start on 16 bytes
// (type-2 packets, 0x80000000, before it as needed); the buffer's write pointer (the last word written) and its end
// are the device's words at `cursor_offset` and `cursor_offset + 4` (48 in Crackdown's runtime, 40 in Test Drive
// Unlimited's). The constants are copied as they are, their source rounded down to 16 bytes as lvx128 does. A run
// with no room before the buffer's end goes to a function of the runtime that makes room: such a call, and a mask
// of 0 (the function would loop for ever: its callers never pass one), are declined, and so is memory that is not
// plain. Leaves r4 the write pointer and r5 the index after the last run.
namespace detail {

// The runs as the function's steps find them, `visit(header_at, pads, source, words, index)` for each; false when one
// has no room before the buffer's end. `cursor` comes in as the buffer's write pointer and goes out as the last
// word written; `index` comes in as r5 and goes out as the function leaves it.
template <class Visit>
bool D3dConstantRuns(uint64_t mask, uint32_t source, uint32_t end, uint32_t& cursor, uint64_t& index, Visit&& visit) {
  uint64_t bits = mask;
  uint32_t from = source & ~0xFu;
  while (bits) {
    const uint32_t skip = uint32_t(std::countl_zero(bits));
    from += skip * 64;
    index += uint64_t(skip) * 16;
    bits <<= skip;
    const uint32_t run = uint32_t(std::countl_zero(~bits));  // 64 when every bit left is set
    const uint32_t words = run * 16;
    const uint64_t need = uint64_t(cursor) + (words + 3) * 4;
    if (need >= 0x100000000ull || uint32_t(need) >= end) return false;
    const uint32_t misalign = cursor & 15;
    const uint32_t pads = misalign == 4 ? 1 : misalign == 0 ? 2 : misalign == 12 ? 3 : 0;
    const uint32_t header_at = cursor + 4 * pads + 4;
    visit(header_at, pads, from, words, uint32_t(index));
    cursor = header_at + run * 64;
    from += run * 64;
    index += words;
    bits = run == 64 ? 0 : bits << run;
  }
  return true;
}

inline bool Disjoint(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
  return uint64_t(a) + a_size <= b || uint64_t(b) + b_size <= a;
}

// What a call does, found without writing: false when it is declined. Every run with room; the buffer's words
// written, the constants read (from the first run's to the last's end) and the device's two words plain and apart
// from each other (the function copies 64 bytes at a time and reads its pointer once).
struct ConstantsPlan {
  uint32_t cursor = 0, end = 0, final_cursor = 0;
  uint64_t index_after = 0;
};
inline bool PlanD3dConstants(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                             uint64_t index, uint32_t source, ConstantsPlan& plan) {
  if (!mask || !IsPlainMemory(device + cursor_offset, 8)) return false;
  plan.cursor = std::byteswap(Load32(memory.Host(device + cursor_offset)));
  plan.end = std::byteswap(Load32(memory.Host(device + cursor_offset + 4)));
  if (plan.cursor & 3) return false;
  uint32_t cursor = plan.cursor, first_source = 0, last_source_end = 0;
  bool first = true;
  if (!D3dConstantRuns(mask, source, plan.end, cursor, index, [&](uint32_t, uint32_t, uint32_t from, uint32_t words, uint32_t) {
        if (first) first_source = from, first = false;
        last_source_end = from + words * 4;
      })) {
    return false;
  }
  plan.final_cursor = cursor;
  plan.index_after = index;
  const uint32_t written_at = plan.cursor + 4, written = cursor - plan.cursor;
  const uint32_t read = last_source_end - first_source;
  return IsPlainMemory(written_at, written) && IsPlainMemory(first_source, read) &&
         Disjoint(written_at, written, first_source, read) && Disjoint(written_at, written, device + cursor_offset, 8);
}

}  // namespace detail

inline bool D3dDirtyConstantsAccepts(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                                     uint64_t index, uint32_t source, Range& write) {
  detail::ConstantsPlan plan;
  if (!detail::PlanD3dConstants(memory, device, cursor_offset, mask, index, source, plan)) return false;
  write = {plan.cursor + 4, plan.final_cursor - plan.cursor};  // the buffer's words written; the device's pointer apart
  return true;
}

// r4 and r5 as the function leaves them in `cursor_register` and `index` (r5 coming in).
inline bool D3dDirtyConstants(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                              uint64_t& index, uint32_t source, uint64_t& cursor_register) {
  detail::ConstantsPlan plan;
  // Planned first: a call is declined before anything is written.
  if (!detail::PlanD3dConstants(memory, device, cursor_offset, mask, index, source, plan)) return false;
  uint32_t cursor = plan.cursor;
  detail::D3dConstantRuns(mask, source, plan.end, cursor, index,
                          [&](uint32_t header_at, uint32_t pads, uint32_t from, uint32_t words, uint32_t at_index) {
                            uint8_t* to = memory.Host(header_at - 4 * pads);
                            for (uint32_t p = 0; p < pads; ++p, to += 4) detail::Store32(to, 0x00000080u);  // 0x80000000
                            detail::Store32(to, std::byteswap(((words - 1) << 16) | at_index));
                            std::memcpy(to + 4, memory.Host(from), words * 4);
                          });
  detail::Store32(memory.Host(device + cursor_offset), std::byteswap(cursor));
  cursor_register = cursor;
  return true;
}

// Every wait of the D3D runtime for the GPU (the ring's read pointer passing a position: room in the ring, the end
// of a frame; a fence; a query's result) is a loop in the caller around one function, the wait's step: r3 is the
// wait's state, whose first word is the device, and it returns nonzero while the wait may go on (zero: the device
// is stopping, or the GPU has not moved for seconds and the step has handled the hang). The caller looks at the word
// it waits for between two steps. On the console the step pauses the hardware thread (db16cyc) before it reads the
// time; recompiled, the pause is nothing and the loop takes a whole processor for as long as the GPU needs.
//
// The step keeps its own code. When it says the wait goes on, the thread sleeps until the GPU's progress count moves
// (a count the GPU side moves after every write the guest can see: the read pointer written back, fences, memory
// writes, interrupts) or `longest` passes, so the step's own checks (the device stopping, the hang) still run.
//
// No wake is lost. The count a thread sleeps on is the one it read at the end of its previous step, before the
// caller looked at its word again: whatever the GPU wrote after that look has moved the count, and the sleep
// returns at once. The first step of a wait finds the count of an earlier wait the same way: it sleeps only if the
// GPU has written nothing since, which the caller's look came after.
struct D3dGpuWait {
  uint64_t seen = ~uint64_t{0};

  // Call after the step's own code returned nonzero. `gpu` has `uint64_t Progress()` and
  // `void WaitProgress(uint64_t seen, std::chrono::microseconds longest)`, which returns as soon as Progress() is not
  // `seen`. Returns whether the count had moved when the sleep ended (false: `longest` passed).
  template <class Gpu>
  bool Sleep(Gpu& gpu, std::chrono::microseconds longest) {
    gpu.WaitProgress(seen, longest);
    const uint64_t now = gpu.Progress();
    const bool moved = now != seen;
    seen = now;
    return moved;
  }
};

}  // namespace any360::xdk
