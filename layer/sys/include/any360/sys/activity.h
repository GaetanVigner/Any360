// What any360::sys did since the start, counted cheaply where it happens (relaxed atomics), for diagnostics: the
// host's report reads them at every frame and keeps the difference for the period's slowest frame, to tell a stall in
// the system layer (file reads, sleeping waits, XMA decoding) from one in the title's own code or the renderer.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace any360::sys {

struct Activity {
  std::atomic<uint64_t> file_reads{0}, file_bytes{0}, file_ns{0};  // host file reads for the title
  std::atomic<uint64_t> waits_slept{0};                            // the title's waits that went to sleep
  std::atomic<uint64_t> xma_ns{0};                                 // XMA decoding
  std::atomic<uint64_t> memory_allocs{0}, memory_ns{0};            // the title's physical allocations (MmAllocatePhysicalMemoryEx)
};

inline Activity& SysActivity() {
  static Activity activity;
  return activity;
}

// Adds the time from its construction to its destruction to `counter` (nanoseconds).
class ActivityTimer {
 public:
  explicit ActivityTimer(std::atomic<uint64_t>& counter) : counter_(counter), start_(std::chrono::steady_clock::now()) {}
  ~ActivityTimer() {
    counter_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count()),
                       std::memory_order_relaxed);
  }
  ActivityTimer(const ActivityTimer&) = delete;
  ActivityTimer& operator=(const ActivityTimer&) = delete;

 private:
  std::atomic<uint64_t>& counter_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace any360::sys
