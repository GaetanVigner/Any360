// Diagnostics the command processor and the renderer keep: whether the timers run (a binding's report turns them off
// every other period, to measure what they cost), and the bytes swapped between the guest's byte order and the
// host's (what a title converted ahead of time would no longer pay).
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace any360::gfx {

inline std::atomic<bool> g_diagnostics{true};
inline bool Diagnosing() { return g_diagnostics.load(std::memory_order_relaxed); }

// Bytes swapped, by what: registers from the command stream, index buffers, texture uploads, resolves written back
// into guest memory, shader microcode.
// A cache line each: the command processor's and the renderer's threads count at every packet and draw, and one
// shared line would bounce between them.
struct SwappedBytes {
  alignas(64) std::atomic<uint64_t> registers{0};
  alignas(64) std::atomic<uint64_t> indices{0};
  alignas(64) std::atomic<uint64_t> textures{0};
  alignas(64) std::atomic<uint64_t> write_backs{0};
  alignas(64) std::atomic<uint64_t> shaders{0};
  uint64_t total() const { return registers + indices + textures + write_backs + shaders; }
};
inline SwappedBytes g_swapped;
static_assert(sizeof(SwappedBytes) == 5 * 64);
// Counted per thread and added to the shared counters by 256 KB: an atomic add at every packet or draw stalls the
// thread on its pending stores (what is left uncounted is too little to show).
inline void CountSwapped(std::atomic<uint64_t>& counter, uint64_t bytes) {
  thread_local uint64_t pending[5] = {};
  uint64_t& mine = pending[(reinterpret_cast<const char*>(&counter) - reinterpret_cast<const char*>(&g_swapped)) / 64];
  mine += bytes;
  if (mine >= (256u << 10)) counter.fetch_add(std::exchange(mine, 0), std::memory_order_relaxed);
}

}  // namespace any360::gfx
