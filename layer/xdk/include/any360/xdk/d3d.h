// any360::xdk: the XDK's D3D runtime's waits for the GPU, made to sleep instead of spinning. The rule this follows:
// docs/NATIVE_FUNCTIONS.md.
#pragma once

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "crt.h"
#include "d3d_records.h"

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

inline bool Disjoint(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
  return uint64_t(a) + a_size <= b || uint64_t(b) + b_size <= a;
}

// A call planned without walking its runs. The write pointer only grows, so each run's room test is stricter than
// the one before: the last run's decides them all. The last run's start comes from the mask: its set bits, its runs
// (a run starts at a set bit whose next higher bit is clear) and the length of its lowest run.
struct UploadPlan {
  uint32_t cursor = 0, end = 0, final_cursor = 0, source_at = 0, source_size = 0;
  uint64_t index_after = 0;
};
struct MaskShape {
  uint32_t ones, runs, last_run, first, past_last;  // first: the first set bit's place (bit 63 = 0); past_last: after the last
};
inline MaskShape ShapeOf(uint64_t mask) {
  const uint32_t low = uint32_t(std::countr_zero(mask));
  return {uint32_t(std::popcount(mask)), uint32_t(std::popcount(mask & ~(mask >> 1))),
          uint32_t(std::countr_zero(~(mask >> low))), uint32_t(std::countl_zero(mask)), 64 - low};
}
// The device's pointer and end, and what any plan declines: a mask of 0, a pointer not on 4 bytes.
inline bool LoadCursor(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask, UploadPlan& plan) {
  if (!mask || !IsPlainMemory(device + cursor_offset, 8)) return false;
  plan.cursor = std::byteswap(Load32(memory.Host(device + cursor_offset)));
  plan.end = std::byteswap(Load32(memory.Host(device + cursor_offset + 4)));
  return (plan.cursor & 3) == 0;
}
// The buffer's words written, the source read and the device's two words: plain and apart.
inline bool Apart(const UploadPlan& plan, uint32_t device, uint32_t cursor_offset) {
  const uint32_t written_at = plan.cursor + 4, written = plan.final_cursor - plan.cursor;
  return IsPlainMemory(written_at, written) && IsPlainMemory(plan.source_at, plan.source_size) &&
         Disjoint(written_at, written, plan.source_at, plan.source_size) &&
         Disjoint(written_at, written, device + cursor_offset, 8);
}

// 64 bytes a bit: the first run's header placed so that its constants start on 16 bytes (pads from the pointer's
// alignment), every later one after 3 pads (a run ends 12 bytes past 16). A run's room test: the pointer plus
// (words + 3) * 4 below the end.
inline bool PlanD3dConstants(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                             uint64_t index, uint32_t source, UploadPlan& plan) {
  if (!LoadCursor(memory, device, cursor_offset, mask, plan)) return false;
  const MaskShape s = ShapeOf(mask);
  const uint32_t misalign = plan.cursor & 15;
  const uint64_t first_pads = misalign == 4 ? 1 : misalign == 0 ? 2 : misalign == 12 ? 3 : 0;
  const uint64_t last_pads = s.runs == 1 ? first_pads : 3;
  const uint64_t final_cursor = uint64_t(plan.cursor) + 4 * first_pads + 4 + 64ull * s.ones + 16ull * (s.runs - 1);
  const uint64_t last_start = final_cursor - 64ull * s.last_run - 4 * last_pads - 4;
  if (final_cursor >= 0x100000000ull || last_start + 64ull * s.last_run + 12 >= plan.end) return false;
  plan.final_cursor = uint32_t(final_cursor);
  plan.index_after = index + 16ull * s.past_last;
  plan.source_at = (source & ~0xFu) + 64 * s.first;
  plan.source_size = 64 * (s.past_last - s.first);
  return Apart(plan, device, cursor_offset);
}

// 4 bytes a bit: a header and the run's words, no pads. A run's room test: the pointer plus the run's bytes (header
// not counted) below the end.
inline bool PlanD3dWords(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask, uint64_t index,
                         uint32_t source, UploadPlan& plan) {
  if (!LoadCursor(memory, device, cursor_offset, mask, plan)) return false;
  const MaskShape s = ShapeOf(mask);
  const uint64_t final_cursor = uint64_t(plan.cursor) + 4ull * (s.ones + s.runs);
  if (final_cursor >= 0x100000000ull || final_cursor - 4 >= plan.end) return false;  // the last run's start + its words
  plan.final_cursor = uint32_t(final_cursor);
  plan.index_after = index + s.past_last;
  plan.source_at = source + 4 * s.first;
  plan.source_size = 4 * (s.past_last - s.first);
  return Apart(plan, device, cursor_offset);
}

// The runs, from the first set bit: `visit(skip, run)` with the clear bits before each run and the run's length.
template <class Visit>
void ForEachRun(uint64_t mask, Visit&& visit) {
  uint64_t bits = mask;
  while (bits) {
    const uint32_t skip = uint32_t(std::countl_zero(bits));
    bits <<= skip;
    const uint32_t run = uint32_t(std::countl_zero(~bits));  // 64 when every bit left is set
    visit(skip, run);
    bits = run == 64 ? 0 : bits << run;
  }
}

}  // namespace detail

inline bool D3dDirtyConstantsAccepts(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                                     uint64_t index, uint32_t source, Range& write) {
  detail::UploadPlan plan;
  if (!detail::PlanD3dConstants(memory, device, cursor_offset, mask, index, source, plan)) return false;
  write = {plan.cursor + 4, plan.final_cursor - plan.cursor};  // the buffer's words written; the device's pointer apart
  return true;
}

// r4 and r5 as the function leaves them in `cursor_register` and `index` (r5 coming in).
inline bool D3dDirtyConstants(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                              uint64_t& index, uint32_t source, uint64_t& cursor_register) {
  detail::UploadPlan plan;
  // Planned first: a call is declined before anything is written.
  if (!detail::PlanD3dConstants(memory, device, cursor_offset, mask, index, source, plan)) return false;
  uint32_t cursor = plan.cursor, from = source & ~0xFu, at = uint32_t(index);
  detail::ForEachRun(mask, [&](uint32_t skip, uint32_t run) {
    from += skip * 64, at += skip * 16;
    const uint32_t misalign = cursor & 15;
    const uint32_t pads = misalign == 4 ? 1 : misalign == 0 ? 2 : misalign == 12 ? 3 : 0;
    uint8_t* to = memory.Host(cursor + 4);
    for (uint32_t p = 0; p < pads; ++p, to += 4) detail::Store32(to, 0x00000080u);  // 0x80000000
    detail::Store32(to, std::byteswap(((run * 16 - 1) << 16) | at));
    std::memcpy(to + 4, memory.Host(from), run * 64);
    cursor += 4 * pads + 4 + run * 64, from += run * 64, at += run * 16;
  });
  detail::Store32(memory.Host(device + cursor_offset), std::byteswap(plan.final_cursor));
  cursor_register = plan.final_cursor;
  index = plan.index_after;
  return true;
}

// The same upload for the constants of 4 bytes a bit (named after what it does; Crackdown's sub_826C5B00, Test Drive
// Unlimited's sub_8268DB50, Call of Duty: Black Ops' sub_824B20F0, the same steps; The Simpsons Game's sub_8245E8C8
// too, patched by hand in that port): r3 the device, r4 the mask, r5 the index of its first bit, r6 the words. For
// each run of set bits a header ((words - 1) << 16 | index) and the run's words, no pads; the room test is the
// function's own (the write pointer plus the run's bytes, header not counted, below the end). The same declines as
// above. Leaves r4 the write pointer and r5 the index after the last run.
inline bool D3dDirtyWordsAccepts(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                                 uint64_t index, uint32_t source, Range& write) {
  detail::UploadPlan plan;
  if (!detail::PlanD3dWords(memory, device, cursor_offset, mask, index, source, plan)) return false;
  write = {plan.cursor + 4, plan.final_cursor - plan.cursor};
  return true;
}

inline bool D3dDirtyWords(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask, uint64_t& index,
                          uint32_t source, uint64_t& cursor_register) {
  detail::UploadPlan plan;
  if (!detail::PlanD3dWords(memory, device, cursor_offset, mask, index, source, plan)) return false;
  uint8_t* to = memory.Host(plan.cursor + 4);
  const uint8_t* from = memory.Host(source);
  uint32_t at = uint32_t(index);
  detail::ForEachRun(mask, [&](uint32_t skip, uint32_t run) {
    from += skip * 4, at += skip;
    detail::Store32(to, std::byteswap(((run - 1) << 16) | at));
    to += 4;
    if (run <= 4) {  // most runs: a few words, not worth a call to memcpy
      for (uint32_t w = 0; w < run; ++w) detail::Store32(to + 4 * w, detail::Load32(from + 4 * w));
    } else {
      std::memcpy(to, from, run * 4);
    }
    to += run * 4, from += run * 4, at += run;
  });
  detail::Store32(memory.Host(device + cursor_offset), std::byteswap(plan.final_cursor));
  cursor_register = plan.final_cursor;
  index = plan.index_after;
  return true;
}

// The same uploads written as Any360's register records (d3d_records.h) instead of type-0 packets: a record a run of
// set bits, its registers' values in the host's byte order (swapped from the title's big-endian constants), no pads.
// Declined (false, nothing written) where the PM4 upload is, and when the records would reach the buffer's end (a
// record a run is a word longer than its packet): the caller then writes the PM4 form.
namespace detail {
template <uint32_t kWordsABit>
inline bool UploadAsRecords(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask, uint64_t& index,
                            uint32_t source, uint64_t& cursor_register, const UploadPlan& plan) {
  const MaskShape s = ShapeOf(mask);
  const uint64_t bytes = 4ull * (2ull * s.runs + uint64_t(kWordsABit) * s.ones);
  const uint64_t final_cursor = uint64_t(plan.cursor) + bytes;
  if (final_cursor >= plan.end || !IsPlainMemory(plan.cursor + 4, uint32_t(bytes)) ||
      !Disjoint(plan.cursor + 4, uint32_t(bytes), plan.source_at, plan.source_size) ||
      !Disjoint(plan.cursor + 4, uint32_t(bytes), device + cursor_offset, 8))
    return false;
  uint8_t* to = memory.Host(plan.cursor + 4);
  const uint8_t* from = memory.Host(kWordsABit == 16 ? (source & ~0xFu) : source);
  uint32_t at = uint32_t(index);
  ForEachRun(mask, [&](uint32_t skip, uint32_t run) {
    from += skip * 4 * kWordsABit, at += skip * kWordsABit;
    const uint32_t words = run * kWordsABit;
    Store32(to, std::byteswap(record::Header(record::RegistersWords(words))));
    Store32(to + 4, record::KindWord(record::kRegisters, at));
    to += 8;
    for (uint32_t w = 0; w < words; ++w) Store32(to + 4 * w, std::byteswap(Load32(from + 4 * w)));
    to += 4 * words, from += 4 * words, at += words;
  });
  Store32(memory.Host(device + cursor_offset), std::byteswap(uint32_t(final_cursor)));
  cursor_register = final_cursor;
  index = plan.index_after;
  return true;
}
}  // namespace detail
inline bool D3dDirtyConstantsRecords(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                                     uint64_t& index, uint32_t source, uint64_t& cursor_register) {
  detail::UploadPlan plan;
  if (!detail::PlanD3dConstants(memory, device, cursor_offset, mask, index, source, plan)) return false;
  return detail::UploadAsRecords<16>(memory, device, cursor_offset, mask, index, source, cursor_register, plan);
}
inline bool D3dDirtyWordsRecords(const Memory& memory, uint32_t device, uint32_t cursor_offset, uint64_t mask,
                                 uint64_t& index, uint32_t source, uint64_t& cursor_register) {
  detail::UploadPlan plan;
  if (!detail::PlanD3dWords(memory, device, cursor_offset, mask, index, source, plan)) return false;
  return detail::UploadAsRecords<1>(memory, device, cursor_offset, mask, index, source, cursor_register, plan);
}

// The D3D runtime's copy of a title's shader constants into the device (named after what it does; Crackdown's
// sub_826B2F68, Test Drive Unlimited's sub_8266B808, the same steps in another order): r3 the device, r4 the first
// constant register, r5 the constants (any alignment: lvlx and lvrx), r6 how many (16 bytes each), r7 the dirty bits
// to add. The vectors go to the device + (r4 + 112) * 16 (rounded down to 16 bytes, as stvx does), four at a time
// then one at a time; then the device's first doubleword gets r7 or-ed in. It saves r7 at r1 + 48 (the caller's
// parameter area). Declined: memory that is not plain, a source over the destination or the device's first
// doubleword. r3 is left as it came; the other volatile registers are the function's own.
// `vector_base` is where the registers start in the device (in 16-byte registers: 112 the vertex shader's, 368
// the pixel shader's in Crackdown's runtime), `dirty_offset` where their dirty bits are.
inline bool D3dSetShaderConstantsAccepts(uint64_t device, uint64_t first, uint32_t source, uint32_t count,
                                         uint32_t stack_pointer, Range& write, uint32_t vector_base = 112,
                                         uint32_t dirty_offset = 0) {
  const uint32_t dest = uint32_t(device + ((uint32_t(first + vector_base) << 4) & 0xFFFFFFF0u)) & ~0xFu;
  const uint64_t bytes = uint64_t(count) * 16;
  if (bytes >= 0x100000000ull) return false;
  write = {dest, uint32_t(bytes)};
  const uint32_t d = uint32_t(device + dirty_offset);
  return IsPlainMemory(dest, write.size) && IsPlainMemory(source, write.size) && IsPlainMemory(d, 8) &&
         IsPlainMemory(stack_pointer + 48, 8) && detail::Disjoint(dest, write.size, source, write.size) &&
         detail::Disjoint(dest, write.size, d, 8) && detail::Disjoint(dest, write.size, stack_pointer + 48, 8) &&
         detail::Disjoint(d, 8, stack_pointer + 48, 8);
}
inline bool D3dSetShaderConstants(const Memory& memory, uint64_t device, uint64_t first, uint32_t source, uint32_t count,
                                  uint64_t dirty, uint32_t stack_pointer, uint32_t vector_base = 112,
                                  uint32_t dirty_offset = 0) {
  Range write;
  if (!D3dSetShaderConstantsAccepts(device, first, source, count, stack_pointer, write, vector_base, dirty_offset))
    return false;
  detail::Store64(memory.Host(stack_pointer + 48), detail::ByteSwap64(dirty));
  std::memcpy(memory.Host(write.at), memory.Host(source), write.size);
  uint8_t* bits = memory.Host(uint32_t(device + dirty_offset));
  detail::Store64(bits, detail::ByteSwap64(detail::ByteSwap64(detail::Load64(bits)) | dirty));
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

// D3DResource_AddRef and D3DResource_Release, the XDK's reference counting of D3D resources (Crackdown sub_826ACC90,
// sub_826ACD08). A resource: +0 its common word (bits 0-3 its type; bit 30 on a type-4 resource: it holds its
// parent), +4 its reference count, +24 the parent. The count changes atomically, as the runtime's lwarx/stwcx. loop
// does (other threads change it at the same time).
//  AddRef: count + 1; when it becomes 1 on a type-4 resource with bit 30, the parent referenced the same way.
//  Release: count - 1; when it becomes 0, the parent released first the same way (a type-4 resource with bit 30),
//  then `destroy(resource, depth)` (the runtime's sub_826AC6C8; depth 0 the resource asked for, 1 its parent...),
//  and 0 back.
// `count` is the runtime's r3 back: the 32-bit count + or - 1 in 64 bits (0 - 1 is -1, 0xFFFFFFFF + 1 is 2^32).
// Declined (false, nothing changed): a resource of the chain the call may reach not 4-byte aligned plain memory, or a
// chain longer than 8 (read before the first change; the runtime's code would follow it into any memory).
namespace detail {
// A big-endian guest word.
inline uint32_t ResourceWord(const Memory& m, uint32_t address) { return std::byteswap(Load32(m.Host(address))); }
inline bool ResourceChainPlain(const Memory& m, uint32_t resource) {
  for (int depth = 0; depth < 8; ++depth) {
    if ((resource & 3) || !IsPlainMemory(resource, 28)) return false;
    const uint32_t word = ResourceWord(m, resource);
    if ((word & 0xF) != 4 || !(word & 0x40000000u)) return true;
    resource = ResourceWord(m, resource + 24);
  }
  return false;
}
// The count + delta, atomically on the big-endian word; the 32-bit count before.
inline uint32_t AddToCount(const Memory& m, uint32_t resource, uint32_t delta) {
  std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(m.Host(resource + 4)));
  uint32_t old = word.load(std::memory_order_relaxed);
  while (!word.compare_exchange_weak(old, std::byteswap(std::byteswap(old) + delta), std::memory_order_acq_rel)) {
  }
  return std::byteswap(old);
}
inline bool HoldsParent(const Memory& m, uint32_t resource) {
  const uint32_t word = ResourceWord(m, resource);
  return (word & 0xF) == 4 && (word & 0x40000000u);
}
}  // namespace detail

inline bool D3dResourceAddRef(const Memory& m, uint32_t resource, uint64_t& count) {
  if (!detail::ResourceChainPlain(m, resource)) return false;
  count = uint64_t(detail::AddToCount(m, resource, 1)) + 1;
  for (uint32_t r = resource, now = uint32_t(count); now == 1 && detail::HoldsParent(m, r);) {
    r = detail::ResourceWord(m, r + 24);
    now = detail::AddToCount(m, r, 1) + 1;
  }
  return true;
}

namespace detail {
template <class Destroy>
inline uint64_t ReleaseOne(const Memory& m, uint32_t resource, Destroy& destroy, uint32_t depth) {
  const uint64_t now = uint64_t(AddToCount(m, resource, 0xFFFFFFFFu)) - 1;
  if (uint32_t(now) != 0) return now;
  if (HoldsParent(m, resource)) ReleaseOne(m, ResourceWord(m, resource + 24), destroy, depth + 1);
  destroy(resource, depth);
  return 0;
}
}  // namespace detail

template <class Destroy>
inline bool D3dResourceRelease(const Memory& m, uint32_t resource, Destroy&& destroy, uint64_t& count) {
  if (!detail::ResourceChainPlain(m, resource)) return false;
  count = detail::ReleaseOne(m, resource, destroy, 0);
  return true;
}

}  // namespace any360::xdk
