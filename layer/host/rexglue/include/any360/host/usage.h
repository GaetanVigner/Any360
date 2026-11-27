// What a process uses of the machine over a period, for the frame reports of every runtime (efficiency: frames per
// CPU% and per GPU%, tools/frame_lows.py): its CPU time over the wall time, as a share of one logical processor and
// of the processors it may run on (its affinity), and its GPU time over the wall time. The GPU time is the system's
// count of the process's time on its busiest GPU engine (Windows: the kernel graphics statistics, the figure Task
// Manager shows), the same for any graphics API; a renderer can also pass the busy time its own timestamps measured.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace any360::host {

// A period's shares, in percent; < 0: not known.
struct UsageShares {
  double cpu_core_pct = -1;     // CPU time / wall time (100: one logical processor busy)
  double cpu_allowed_pct = -1;  // the same over the logical processors the process may run on
  uint32_t cores = 0;           // those processors
  uint64_t mask = 0;            // their mask (0: not known)
  uint32_t mask_resets = 0;     // the times the kept mask (KeepProcessAffinity) was set again over the period
  double gpu_pct = -1;          // the system's count: the busiest engine's time / wall time
  double gpu_own_pct = -1;      // the caller's own GPU busy time / wall time
};

// The shares from a period's figures (nanoseconds; a negative GPU time: not known). Pure: unit-tested.
UsageShares ComputeUsage(uint64_t wall_ns, uint64_t cpu_ns, uint32_t cores, int64_t gpu_ns, int64_t gpu_own_ns);

// "CPU 245% of a core, 31% of the 8 allowed (mask 0xFF); GPU 62% (system), 60% (batches)": the parts not known left
// out; "(mask 0xFF, set again 1 time)" when the kept mask had been changed.
std::string FormatUsage(const UsageShares& shares);

// The processors the process may run on, as a mask (bit n: logical processor n; 0 where the system does not say).
uint64_t ProcessAffinity();
// Restricts the process (every thread, those to come too) to `mask`; false when the system refuses it.
bool SetProcessAffinity(uint64_t mask);
// A mask to keep (0: none): set now, and set again by every UsageMeter::Take that finds it changed (a runtime opens the
// process to every processor while it starts). Returns SetProcessAffinity's result.
bool KeepProcessAffinity(uint64_t mask);
uint64_t KeptProcessAffinity();
// A mask from "0xFF" (hexadecimal), "255" (decimal) or "0-3,8" (logical processors and ranges); 0 when the text is
// none of these or names no processor below 64.
uint64_t ParseAffinityMask(std::string_view text);

// The process's figures, sampled at its construction and at each Take, which returns the shares since the last one.
class UsageMeter {
 public:
  UsageMeter();
  // `gpu_own_ns`: the caller's own GPU busy time over the period (a renderer's batch timestamps), < 0 when none.
  UsageShares Take(int64_t gpu_own_ns = -1);

  // The process's CPU time (user and kernel, every thread), ns; its allowed logical processors; its GPU time on each
  // engine of each adapter (ns, the system's count; empty where the system does not say).
  static uint64_t ProcessCpuNs();
  static uint32_t AllowedProcessors();
  std::vector<uint64_t> GpuEngineNs() const;

 private:
  struct Engine {
    uint64_t luid = 0;
    uint32_t node = 0;
  };
  std::vector<Engine> engines_;
  uint64_t wall_ns_ = 0, cpu_ns_ = 0;
  std::vector<uint64_t> gpu_ns_;
};

}  // namespace any360::host
