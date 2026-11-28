// The bench's GPU side, as a title's host sets it up: any360::gfx's command processor as any360::sys's GpuBackend, in
// front of the Vulkan renderer (through the asynchronous renderer by default, as titles run), presenting to a window
// or headless. It times every swap and writes the periodic report lines tools/frame_lows.py reads.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace any360::sys {
class GpuBackend;
}

namespace any360::bench {

struct GpuOptions {
  std::string gpu;  // a substring of the device name; empty: the renderer's choice
  bool window = false;
  bool async_renderer = true;
  bool async_pipelines = true;
  bool validation = false;
  uint32_t width = 1280, height = 720;
  double report_seconds = 5.0;
  std::function<void(const std::string&)> log;
};

// What a whole run measured, from its first swap after `warmup` frames to its last.
struct RunSummary {
  uint64_t frames = 0;
  double seconds = 0, fps = 0, median_ms = 0, p10_fps = 0, p1_fps = 0, slowest_ms = 0;
  uint32_t over_16ms = 0;
  double cpu_pct = 0;          // process CPU time over wall time, of one core (100 = one core busy)
  double cpu_allowed_pct = 0;  // the same over the processors the process may run on
  uint32_t cores = 0;
  uint64_t mask = 0;
  double gpu_pct = 0;          // the system's count of the busiest engine's time over wall time (else the renderer's)
  double gpu_own_pct = 0;      // the renderer's batches' time over wall time
  std::string usage;           // any360/host/usage.h's text
  double private_mb = 0, peak_private_mb = 0;
  // Per frame (ms): the renderer's draws (textures, shaders, pipelines, guest memory, submit), resolves, swaps; the
  // command processor busy; the GPU's time.
  double draws_ms = 0, textures_ms = 0, shaders_ms = 0, pipelines_ms = 0, memory_ms = 0, submit_ms = 0, resolves_ms = 0,
         swaps_ms = 0, cp_ms = 0, gpu_ms = 0, renderer_thread_ms = 0;
  double draws = 0, resolves = 0, texture_uploads = 0, pipelines = 0, failed = 0;
  // Host heap allocations a frame, every thread of the process (any360/sys/alloc_meter.h): calls and bytes.
  double allocations = 0, allocated_bytes = 0;
  uint64_t total_failed = 0;
  std::string device;
};

class BenchGpu {
 public:
  BenchGpu();
  ~BenchGpu();

  // `physical`: the guest's 512 MB of physical memory (a view the CPU's write watch never protects). `watch(begin,
  // end)` arms the host's one-shot write watch over physical [begin, end).
  bool Start(const GpuOptions& options, uint8_t* physical, std::function<void(uint32_t, uint32_t)> watch, std::string& error);
  sys::GpuBackend* backend();
  // The CPU wrote physical [begin, end) under the watch (any thread).
  void MemoryWritten(uint32_t begin, uint32_t end);
  // The window's events (the thread that started it); false once it is closed.
  bool PumpWindow();
  // Every swap so far.
  uint64_t frames() const;
  // From now on the summary counts (after the warm-up frames).
  void BeginMeasure();
  RunSummary Summary();
  // The last frame's pixels (RGBA8), read back.
  bool ReadFrame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);
  void Stop();

  struct State;

 private:
  std::unique_ptr<State> s_;
};

// Restricts the process to the processors of `mask` ("0x3", "3", "0-1") and keeps it there (any360/host/usage.h);
// false when the text names none or the system refuses.
bool KeepAffinity(const std::string& mask);

// Writes RGBA8 pixels (red in the low byte) as a PNG.
bool SavePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint32_t>& pixels);

// The process's private bytes and its CPU time (user + kernel, seconds).
uint64_t PrivateBytes();
double ProcessCpuSeconds();

}  // namespace any360::bench
