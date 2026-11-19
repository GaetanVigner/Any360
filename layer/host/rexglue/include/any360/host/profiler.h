// The function profiler: where a title's threads spend their processor time over a stretch of play, function by
// function. Every 2 ms each thread of the process that ran since the last look is sampled (its stack captured as the
// hitch tracer does, any360/host/thread_stacks.h); a thread caught running counts the time since the previous round
// on that stack (the chance of catching a thread running is its share of the time: the rounds' times summed over its
// samples estimate the time it ran there).
// The report, symbolized at the end:
//  - per title function (a symbol with sub_XXXXXXXX in its name: the recompiled function or the binding that replaces
//    it), its own time (the samples where it is the innermost title function: itself and the host code it calls,
//    Any360's natives and the system's included), the part of it in its own code (no executable frame inside it: what
//    making it native works on) and its time with what it calls (anywhere on the stack's first kDepth frames of the
//    executable);
//  - per host function a title function calls (the outermost executable frame inside the innermost title one: a
//    binding's handler, a native, a kernel export; a binding that only forwards leaves no frame of its own, so its
//    handler's time shows here, under the title function that called it), its time and that title function;
//  - per host function, the time it was the innermost frame (where host time goes: memcpy, the driver).
// A thread caught in a system call (ntdll's Zw* / Nt*: a wait, nearly always) is not running: counted apart.
// Times are milliseconds of one core. Off unless a title starts it (any360_profile_seconds); the samples themselves
// cost a few percent.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "any360/host/thread_stacks.h"

namespace any360::host::diagnostics {

// The sampling and the report, apart from the timing (tests drive it directly).
class ProfileSampler {
 public:
  static constexpr size_t kDepth = 24;  // the executable's frames kept a sample (the innermost frame apart)
  // Opens the process's threads but the caller's (allocates; keeps the cycles seen of those it had).
  void Refresh();
  // One look at every thread: those that used at least `min_cycles` since the last look (the others are surely
  // waiting) are captured and tallied, `ms` each (the time since the previous round).
  void Sample(uint64_t min_cycles, double ms);
  // The report on everything tallied: `seconds` the stretch sampled, `cycles_per_ms` the clock (reported). Writes
  // the text report to `text` and one line a function to `csv` ("title,<hex address>,<symbol>,<own ms>,<total ms>,
  // <self ms>", "called,<caller's hex address>,<symbol>,<ms>,," and "host,,<symbol>,<own ms>,,").
  void Report(double seconds, double cycles_per_ms, std::string& text, std::string& csv) const;
  uint64_t samples() const { return samples_; }

 private:
  struct Key {
    std::array<uint64_t, kDepth + 1> frames{};  // the innermost function (any module), then the executable's
    bool operator==(const Key& o) const { return frames == o.frames; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = 1469598103934665603ull;
      for (uint64_t f : k.frames) h = (h ^ f) * 1099511628211ull;
      return size_t(h);
    }
  };
  struct Thread {
    ThreadHandle handle;
    uint64_t last_cycles = 0;
    explicit Thread(uint32_t id) : handle(id) {}
  };
  std::vector<Thread> threads_;
  ThreadStack stack_;
  std::unordered_map<Key, double, KeyHash> tally_;  // milliseconds
  uint64_t samples_ = 0;
};

class Profiler {
 public:
  // Samples from `after_seconds` after construction for `seconds`, then writes `folder`/profile.txt and profile.csv
  // and announces them through `log`.
  Profiler(double after_seconds, double seconds, std::string folder, std::function<void(const std::string&)> log);
  ~Profiler();

 private:
  void Run();
  const double after_seconds_, seconds_;
  const std::string folder_;
  std::function<void(const std::string&)> log_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

// The cycles a millisecond of the calling thread (a busy loop measured on its own cycle counter).
double MeasureCyclesPerMs();

}  // namespace any360::host::diagnostics
