// The bench title: code in the recompiled form (functions of a PowerPC context and the guest memory base, at guest
// addresses the scheduler's dispatcher runs) calling any360::sys through its exports, as a title's import thunks do.
// RunTitle sets any360::sys up the way a title's host does, starts the title's main thread and waits for it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "city.h"

namespace any360::bench {

class BenchGpu;

struct TitleOptions {
  CityOptions city;
  uint32_t frames = 1200;
  uint32_t warmup = 120;  // frames before the measure starts
  uint32_t workers = 2;
  double vblank_hz = 60.0;
  double timeout_seconds = 0;  // > 0: the run ends with a report of where each title thread waits
  std::function<void()> on_measure;
  std::function<void(const std::string&)> log;
};

struct TitleResult {
  uint32_t frames = 0;
  uint64_t pm4_hash = 0;       // every frame's command buffer, in order
  uint64_t pm4_dwords = 0;
  uint64_t audio_hash = 0;     // the first kAudioHashFrames audio frames the sink got
  uint64_t audio_frames = 0;
  uint64_t vblanks = 0;
  uint64_t write_faults = 0;   // CPU writes the watch reported
  double main_ms = 0;          // the main thread's work a frame (animation jobs, streaming, allocations), waits excluded
  double render_ms = 0;        // the render thread's work a frame (the command buffer, the ring), waits excluded
  double kernel_calls = 0;     // system calls a frame
};
constexpr uint32_t kAudioHashFrames = 64;

bool RunTitle(const TitleOptions& options, BenchGpu& gpu, TitleResult& result, std::string& error);

}  // namespace any360::bench
