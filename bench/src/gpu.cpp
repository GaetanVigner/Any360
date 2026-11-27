#include "gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "any360/gfx/async_renderer.h"
#include "any360/gfx/command_processor.h"
#include "any360/gfx/frame_counter.h"
#include "any360/gfx/png.h"
#include "any360/gfx/vulkan/presenter.h"
#include "any360/gfx/vulkan/renderer.h"
#include "any360/host/usage.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#if ANY360_BENCH_WINDOW
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace any360::bench {

namespace vk = gfx::vk;
using Clock = std::chrono::steady_clock;

uint64_t PrivateBytes() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 0;
  return counters.PrivateUsage;
}

bool KeepAffinity(const std::string& mask) {
  const uint64_t parsed = host::ParseAffinityMask(mask);
  return parsed && host::KeepProcessAffinity(parsed);
}

bool SavePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint32_t>& pixels) {
  return pixels.size() >= size_t(width) * height && gfx::WritePng(path, width, height, pixels.data());
}

double ProcessCpuSeconds() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  auto seconds = [](const FILETIME& t) { return double(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 1e-7; };
  return seconds(kernel) + seconds(user);
}

class BenchRenderer;

struct BenchGpu::State {
  GpuOptions o;
  vk::Device device;
  std::unique_ptr<BenchRenderer> renderer;
  std::unique_ptr<gfx::CommandProcessor> cp;
  std::unique_ptr<gfx::AsyncRenderer> async;
  std::unique_ptr<vk::Presenter> presenter;
#if ANY360_BENCH_WINDOW
  SDL_Window* window = nullptr;
#endif
  std::atomic<uint32_t> window_width{0}, window_height{0};
  std::atomic<bool> resize{false};
  std::atomic<uint64_t> frames{0};

  // On the renderer's thread: swap times, the period's and the run's.
  Clock::time_point last_swap{}, period_start{};
  std::vector<float> period_ms, run_ms;
  float period_worst = 0;
  uint32_t period_slow = 0;
  vk::RendererStats period_stats{};
  uint64_t period_cp_busy = 0;
  host::UsageMeter period_usage;
  std::unique_ptr<host::UsageMeter> run_usage;
  std::atomic<bool> measure_requested{false};
  bool measuring = false;
  Clock::time_point run_start{};
  vk::RendererStats run_stats{};
  uint64_t run_cp_busy = 0, run_async_busy = 0, run_frames = 0;
  float run_worst = 0;
  uint32_t run_slow = 0;
  double peak_private = 0;

  void OnSwap(vk::VulkanRenderer& r);
  void Report(const vk::RendererStats& now, Clock::time_point t);
};

class BenchRenderer : public vk::VulkanRenderer {
 public:
  BenchRenderer(vk::Device& device, uint8_t* physical, BenchGpu::State& owner)
      : VulkanRenderer(device, physical, 0x20000000), owner_(owner) {}
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* registers) override {
    VulkanRenderer::Swap(frontbuffer, width, height, registers);
    owner_.OnSwap(*this);
  }

 private:
  BenchGpu::State& owner_;
};

void BenchGpu::State::OnSwap(vk::VulkanRenderer& r) {
  if (presenter) {
    if (resize.exchange(false)) presenter->Resize(window_width, window_height);
    const vk::VulkanRenderer::Frame& frame = r.frame();
    const bool shown = frame.on_gpu ? presenter->Present([&](VkCommandBuffer cb, VkImage target, VkOffset3D a, VkOffset3D b) {
                                        r.RecordFrameBlit(cb, target, a, b);
                                      }, frame.width, frame.height)
                                    : presenter->Present(frame.pixels.data(), frame.width, frame.height);
    if (!shown && presenter->error().find("out of date") != std::string::npos) resize = true;
  }
  const Clock::time_point now = Clock::now();
  const uint64_t n = frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1) {
    last_swap = period_start = now;
    period_stats = r.stats();
    period_cp_busy = cp->ns_busy();
    period_usage.Take();
    return;
  }
  const float ms = std::chrono::duration<float, std::milli>(now - last_swap).count();
  last_swap = now;
  period_ms.push_back(ms);
  period_worst = std::max(period_worst, ms);
  period_slow += ms > 16.7f;
  if (measure_requested.exchange(false)) {
    measuring = true;
    run_start = now;
    run_usage = std::make_unique<host::UsageMeter>();
    run_stats = r.stats();
    run_cp_busy = cp->ns_busy();
    run_async_busy = async ? async->busy_ns() : 0;
    run_ms.clear();
    run_ms.reserve(1 << 16);
  } else if (measuring) {
    run_ms.push_back(ms);
    run_worst = std::max(run_worst, ms);
    run_slow += ms > 16.7f;
    ++run_frames;
  }
  if (std::chrono::duration<double>(now - period_start).count() >= o.report_seconds) Report(r.stats(), now);
}

void BenchGpu::State::Report(const vk::RendererStats& s, Clock::time_point t) {
  const double seconds = std::chrono::duration<double>(t - period_start).count();
  const double n = double(std::max<size_t>(1, period_ms.size())), ms = n * 1e6;
  const vk::RendererStats& l = period_stats;
  const double cp_busy = double(cp->ns_busy() - period_cp_busy);
  const double fps = n / seconds;
  const std::string usage = host::FormatUsage(period_usage.Take(int64_t(s.ns_gpu - l.ns_gpu)));
  const double private_mb = double(PrivateBytes()) / 1e6;
  peak_private = std::max(peak_private, private_mb);
  char line[1024];
  std::snprintf(line, sizeof(line),
                "Any360: %.1f FPS; per frame: %.0f draws, %.0f skipped, %.0f failed, %.0f resolves, %.1f texture uploads, "
                "%.1f pipelines; ms: draws %.2f (textures %.2f, shaders %.2f, pipelines %.2f, guest memory %.2f, submit %.2f), "
                "resolves %.2f, swaps %.2f; command processor %.2f, GPU %.2f; %s",
                fps, (s.draws - l.draws) / n, (s.skipped - l.skipped) / n, (s.failed - l.failed) / n, (s.resolves - l.resolves) / n,
                (s.texture_uploads - l.texture_uploads) / n, (s.pipelines - l.pipelines) / n, (s.ns_draws - l.ns_draws) / ms,
                (s.ns_textures - l.ns_textures) / ms, (s.ns_shaders - l.ns_shaders) / ms, (s.ns_pipelines - l.ns_pipelines) / ms,
                (s.ns_memory - l.ns_memory) / ms, (s.ns_submit - l.ns_submit) / ms, (s.ns_resolves - l.ns_resolves) / ms,
                (s.ns_swaps - l.ns_swaps) / ms, cp_busy / ms, double(s.ns_gpu - l.ns_gpu) / ms, usage.c_str());
  if (o.log) o.log(line);
  const gfx::FrameTimeSummary f = gfx::SummarizeFrameTimes(period_ms);
  std::snprintf(line, sizeof(line),
                "Any360: %.1f FPS with the diagnostics off (bench); frame times: median %.2f ms (%.1f FPS), 10%% low %.1f FPS, "
                "1%% low %.1f FPS; slowest frame %.1f ms, %u over 16.7 ms; %.0f MB private; %s",
                fps, f.median_ms, f.median_ms > 0 ? 1e3 / f.median_ms : 0, f.p90_ms > 0 ? 1e3 / f.p90_ms : 0,
                f.p99_ms > 0 ? 1e3 / f.p99_ms : 0, period_worst, period_slow, private_mb, usage.c_str());
  if (o.log) o.log(line);
  period_ms.clear();
  period_worst = 0, period_slow = 0;
  period_start = t;
  period_stats = s;
  period_cp_busy = cp->ns_busy();
}

BenchGpu::BenchGpu() : s_(std::make_unique<State>()) {}
BenchGpu::~BenchGpu() { Stop(); }

bool BenchGpu::Start(const GpuOptions& options, uint8_t* physical, std::function<void(uint32_t, uint32_t)> watch,
                     std::string& error) {
  State& s = *s_;
  s.o = options;
  vk::DeviceOptions device_options;
  device_options.prefer = options.gpu;
  device_options.validation = options.validation;
#if ANY360_BENCH_WINDOW
  if (options.window) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      error = std::string("SDL_Init: ") + SDL_GetError();
      return false;
    }
    s.window = SDL_CreateWindow("Any360 bench", int(options.width), int(options.height), SDL_WINDOW_RESIZABLE);
    if (!s.window) {
      error = std::string("window: ") + SDL_GetError();
      return false;
    }
    device_options.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    SDL_Window* window = s.window;
    device_options.make_surface = [window](VkInstance instance) {
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetInstanceProcAddr")));
      auto create = get ? reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get(instance, "vkCreateWin32SurfaceKHR")) : nullptr;
      VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
      info.hinstance = GetModuleHandleA(nullptr);
      info.hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
      if (!create || !info.hwnd || create(instance, &info, nullptr, &surface) != VK_SUCCESS) return VkSurfaceKHR(VK_NULL_HANDLE);
      return surface;
    };
  }
#else
  if (options.window) {
    error = "this build has no window (no SDL3)";
    return false;
  }
#endif
  if (!s.device.Create(device_options)) {
    error = "device: " + s.device.error();
    return false;
  }
  s.renderer = std::make_unique<BenchRenderer>(s.device, physical, s);
  s.renderer->SetAsyncPipelines(options.async_pipelines);
  if (!s.renderer->Initialize()) {
    error = "renderer: " + s.renderer->last_error();
    return false;
  }
  s.renderer->SetRecordThreads(vk::VulkanRenderer::AutoRecordThreads());
  if (vk::TextureCache* textures = s.renderer->texture_cache()) textures->SetMemoryWatch(watch);
  s.renderer->SetMemoryWatch(watch);
#if ANY360_BENCH_WINDOW
  if (s.window) {
    s.presenter = std::make_unique<vk::Presenter>(s.device);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(s.window, &pw, &ph);
    s.window_width = uint32_t(pw), s.window_height = uint32_t(ph);
    if (!s.presenter->Resize(uint32_t(pw), uint32_t(ph))) {
      error = "swapchain: " + s.presenter->error();
      return false;
    }
  }
#endif
  s.cp = std::make_unique<gfx::CommandProcessor>(physical);
  if (options.async_renderer) {
    s.async = std::make_unique<gfx::AsyncRenderer>(*s.cp, *s.renderer);
    s.cp->SetRenderer(s.async.get());
  } else {
    s.cp->SetRenderer(s.renderer.get());
  }
  s.cp->SetDisplaySize(options.width, options.height);
  s.cp->SetMemoryWrittenHook([renderer = s.renderer.get()](uint32_t begin, uint32_t end) { renderer->GuestMemoryWritten(begin, end); });
  s.cp->Start();
  if (options.log) {
    options.log(std::string("Any360 bench: ") + s.device.properties().deviceName + ", " +
                (options.async_renderer ? "asynchronous renderer" : "renderer on the command processor's thread") + ", " +
                std::to_string(std::max(1u, s.renderer->record_threads())) + " recording thread(s), " +
                (options.window ? "windowed" : "headless"));
  }
  return true;
}

sys::GpuBackend* BenchGpu::backend() { return s_->cp.get(); }

void BenchGpu::MemoryWritten(uint32_t begin, uint32_t end) {
  vk::VulkanRenderer* r = s_->renderer.get();
  if (!r) return;
  if (vk::TextureCache* textures = r->texture_cache()) textures->QueueMemoryChanged(begin, end);
  r->GuestMemoryWritten(begin, end);
  r->CpuWrote(begin, end);
}

bool BenchGpu::PumpWindow() {
#if ANY360_BENCH_WINDOW
  if (!s_->window) return true;
  SDL_Event event;
  bool open = true;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) open = false;
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
      int pw = 0, ph = 0;
      SDL_GetWindowSizeInPixels(s_->window, &pw, &ph);
      s_->window_width = uint32_t(pw), s_->window_height = uint32_t(ph);
      s_->resize = true;
    }
  }
  return open;
#else
  return true;
#endif
}

uint64_t BenchGpu::frames() const { return s_->frames.load(std::memory_order_relaxed); }

void BenchGpu::BeginMeasure() { s_->measure_requested = true; }

RunSummary BenchGpu::Summary() {
  State& s = *s_;
  if (s.async) s.async->Drain();
  RunSummary r;
  r.device = s.device.properties().deviceName;
  if (!s.measuring || s.run_ms.empty()) return r;
  const vk::RendererStats& a = s.run_stats;
  const vk::RendererStats& b = s.renderer->stats();
  r.frames = s.run_frames;
  r.seconds = std::chrono::duration<double>(s.last_swap - s.run_start).count();
  r.fps = r.seconds > 0 ? double(r.frames) / r.seconds : 0;
  const gfx::FrameTimeSummary f = gfx::SummarizeFrameTimes(s.run_ms);
  r.median_ms = f.median_ms;
  r.p10_fps = f.p90_ms > 0 ? 1e3 / f.p90_ms : 0;
  r.p1_fps = f.p99_ms > 0 ? 1e3 / f.p99_ms : 0;
  r.slowest_ms = s.run_worst;
  r.over_16ms = s.run_slow;
  const host::UsageShares usage = s.run_usage->Take(int64_t(b.ns_gpu - a.ns_gpu));
  r.cpu_pct = std::max(0.0, usage.cpu_core_pct);
  r.cpu_allowed_pct = std::max(0.0, usage.cpu_allowed_pct);
  r.cores = usage.cores, r.mask = usage.mask;
  r.gpu_pct = usage.gpu_pct >= 0 ? usage.gpu_pct : std::max(0.0, usage.gpu_own_pct);
  r.gpu_own_pct = std::max(0.0, usage.gpu_own_pct);
  r.usage = host::FormatUsage(usage);
  r.private_mb = double(PrivateBytes()) / 1e6;
  r.peak_private_mb = std::max(s.peak_private, r.private_mb);
  const double n = double(std::max<uint64_t>(1, r.frames)), ms = n * 1e6;
  r.draws_ms = (b.ns_draws - a.ns_draws) / ms, r.textures_ms = (b.ns_textures - a.ns_textures) / ms;
  r.shaders_ms = (b.ns_shaders - a.ns_shaders) / ms, r.pipelines_ms = (b.ns_pipelines - a.ns_pipelines) / ms;
  r.memory_ms = (b.ns_memory - a.ns_memory) / ms, r.submit_ms = (b.ns_submit - a.ns_submit) / ms;
  r.resolves_ms = (b.ns_resolves - a.ns_resolves) / ms, r.swaps_ms = (b.ns_swaps - a.ns_swaps) / ms;
  r.cp_ms = double(s.cp->ns_busy() - s.run_cp_busy) / ms, r.gpu_ms = double(b.ns_gpu - a.ns_gpu) / ms;
  r.renderer_thread_ms = s.async ? double(s.async->busy_ns() - s.run_async_busy) / ms : 0;
  r.draws = (b.draws - a.draws) / n, r.resolves = (b.resolves - a.resolves) / n;
  r.texture_uploads = (b.texture_uploads - a.texture_uploads) / n, r.pipelines = (b.pipelines - a.pipelines) / n;
  r.failed = (b.failed - a.failed) / n;
  r.total_failed = b.failed;
  return r;
}

bool BenchGpu::ReadFrame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
  State& s = *s_;
  if (s.async) s.async->Drain();
  if (!s.renderer || !s.renderer->ReadFrame(pixels)) return false;
  width = s.renderer->frame().width, height = s.renderer->frame().height;
  return true;
}

void BenchGpu::Stop() {
  State& s = *s_;
  if (s.cp) s.cp->Stop();
  s.async.reset();
  if (s.renderer) s.renderer->WaitIdle();
  s.presenter.reset();
  s.cp.reset();
  s.renderer.reset();
#if ANY360_BENCH_WINDOW
  if (s.window) {
    SDL_DestroyWindow(s.window);
    SDL_Quit();
    s.window = nullptr;
  }
#endif
}

}  // namespace any360::bench
