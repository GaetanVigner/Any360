// Diagnostics the command processor and the renderer keep: whether the timers run (a binding's report turns them off
// every other period, to measure what they cost), and the bytes swapped between the guest's byte order and the
// host's (what a title converted ahead of time would no longer pay).
#pragma once

#include <atomic>
#include <cstdint>

namespace any360::gfx {

inline std::atomic<bool> g_diagnostics{true};
inline bool Diagnosing() { return g_diagnostics.load(std::memory_order_relaxed); }

// Bytes swapped, by what: registers from the command stream, index buffers, texture uploads, resolves written back
// into guest memory, shader microcode.
struct SwappedBytes {
  std::atomic<uint64_t> registers{0}, indices{0}, textures{0}, write_backs{0}, shaders{0};
  uint64_t total() const { return registers + indices + textures + write_backs + shaders; }
};
inline SwappedBytes g_swapped;
inline void CountSwapped(std::atomic<uint64_t>& counter, uint64_t bytes) { counter.fetch_add(bytes, std::memory_order_relaxed); }

}  // namespace any360::gfx
