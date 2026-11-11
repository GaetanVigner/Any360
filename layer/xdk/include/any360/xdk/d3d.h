// any360::xdk: the XDK's D3D runtime's waits for the GPU, made to sleep instead of spinning. The rule this follows:
// docs/NATIVE_FUNCTIONS.md.
#pragma once

#include <chrono>
#include <cstdint>

namespace any360::xdk {

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
