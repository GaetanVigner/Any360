// The hitch tracer: what the process's threads do during a frame that takes too long. Steady frames take a few
// milliseconds; when one has run past the threshold, every thread's stack is sampled every 2 ms until the next frame,
// and a report says, thread by thread, the processor cycles it used meanwhile and where it was (its innermost frame,
// then the title's own frames calling it): the thread that burned cycles is the culprit, and if all waited, the stall
// is a wait (a file, a lock, the GPU). Armed only after a run of fast frames (play, not loading screens), a few
// reports a run. Off unless a title starts it (any360_hitch_trace_ms).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "any360/host/thread_stacks.h"

namespace any360::host::diagnostics {

// The sampling itself, apart from the watching (tests drive it directly).
class HitchSampler {
 public:
  // Opens every thread of the process but the caller (allocates, and listing the threads takes milliseconds: done
  // between hitches, not when one starts).
  void Refresh();
  // Notes the threads' cycles and empties the tallies (allocation-free).
  void Begin();
  // One capture of every thread, tallied by function (allocation-free: tables sized in Refresh).
  void Sample();
  // The report on what was sampled since Begin (symbolized now), its threads by the cycles they used.
  std::string End(double hitch_ms);

 private:
  static constexpr size_t kPaths = 64, kDepth = 4;
  struct Path {
    uint64_t frames[kDepth] = {};  // the innermost function, then up to 3 of the executable's functions calling it
    uint32_t count = 0;
  };
  struct Thread {
    ThreadHandle handle;
    uint64_t cycles = 0;
    uint32_t samples = 0, lost = 0;
    Path paths[kPaths];
    explicit Thread(uint32_t id) : handle(id) {}
  };
  std::vector<Thread> threads_;
  ThreadStack stack_;
  uint32_t rounds_ = 0;
};

class HitchTracer {
 public:
  // Reports go to `folder`/hitch_<n>.txt, at most `max_reports`, each announced through `log`.
  HitchTracer(double threshold_ms, std::string folder, int max_reports, std::function<void(const std::string&)> log);
  ~HitchTracer();
  // At every frame the title presents.
  void Frame();

 private:
  void Run();
  const double threshold_ms_;
  const std::string folder_;
  const int max_reports_;
  std::function<void(const std::string&)> log_;
  std::atomic<int64_t> last_frame_ns_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint32_t> fast_streak_{0};  // frames in a row under half the threshold
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace any360::host::diagnostics
