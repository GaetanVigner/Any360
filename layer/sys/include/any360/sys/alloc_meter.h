// Host heap allocations, counted per thread (BenchmarkDotNet's "Allocated" column for Any360's benches, tests and
// in-game self-test): the replacement of the global operator new / delete in alloc_meter.cpp counts the bytes and
// the calls of each thread, and forwards to the C runtime's heap (malloc / _aligned_malloc), so code that allocates
// in one module and frees in another still pairs. Linked into every executable that links any360_sys or any360_xdk.
//
//   const auto before = any360::sys::ThreadAllocations();
//   work();
//   const auto spent = any360::sys::ThreadAllocations() - before;  // spent.bytes, spent.count
#pragma once

#include <cstdint>

namespace any360::sys {

struct AllocCounts {
  uint64_t bytes = 0, count = 0;  // requested bytes (not the heap's own overhead), operator new calls
  AllocCounts operator-(const AllocCounts& o) const { return {bytes - o.bytes, count - o.count}; }
};

// The calling thread's allocations since it started.
AllocCounts ThreadAllocations();
// Every thread's allocations since the process started, the threads that have ended included (any thread). Each
// thread counts into its own block, which this sums: no shared counter on the allocation path.
AllocCounts ProcessAllocations();

}  // namespace any360::sys
