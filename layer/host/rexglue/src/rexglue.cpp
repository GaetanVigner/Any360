// any360::host::rexglue: see the header. The graphics binding inside a ReXGlue title: any360::sys's GraphicsKernel and
// any360::gfx behind the title's Vd* calls and GPU MMIO, interrupts run on a runtime host thread, frames presented
// into a window of the binding's own (a plain Win32 window for now: the runtime has its own SDL inside its library,
// and a second copy in the title would be a second SDL in the process; its messages are pumped by the runtime's UI
// thread, which creates it).
#include "any360/host/rexglue.h"

#include "any360/gfx/async_renderer.h"
#include "any360/gfx/capture.h"
#include "any360/gfx/registers.h"

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <intrin.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/presenter.h"
#include "any360/gfx/vulkan/renderer.h"
#include "any360/sys/graphics.h"
#include "any360/sys/objects.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vulkan/vulkan_win32.h>
#else
#include <dlfcn.h>
#endif

#ifdef _WIN32
using Window = HWND;
#else
using Window = void*;
#endif

namespace any360::host::rexglue {
namespace {

constexpr uint32_t kPhysicalBytes = 0x20000000;  // 512 MB
constexpr uint32_t kGpuMmio = 0x7FC80000;

// What a byte swap costs over a plain copy on this machine, in ns a byte: vectorized (indices, register runs) and one
// word at a time (texels and resolved elements, swapped between other work). Measured once, best of a few runs.
struct SwapCost {
  double vectorized = 0, scalar = 0;
};
// Samples every thread of the process (the title's, the runtime's, ours) every 2 ms while the diagnostics are off:
// where each is, as offsets in the executable, or, in another module (waits in ntdll, the driver), the first caller in
// the executable (bit 63 set), with the leaf's module counted apart.
class Sampler {
 public:
  // `always`: sampled whether the diagnostics are on or not (no binding alternates them).
  explicit Sampler(std::string path, bool always = false) : path_(std::move(path)), always_(always) {
    worker_ = std::thread([this] { Run(); });
  }
  ~Sampler() {
    done_ = true;
    if (worker_.joinable()) worker_.join();
    Write();
  }
  // Lines "thread 0xoffset [0xcaller...]" (0: nowhere in the executable; then up to 15 callers in it, return
  // addresses minus one), then "# thread module name count" (the leaf's module when outside the executable) and
  // "# thread name name".
  void Write() {
    std::lock_guard lock(mutex_);
    if (FILE* f = std::fopen(path_.c_str(), "w")) {
      for (const auto& [tid, s] : samples_) {
        std::fprintf(f, "%lu", tid);
        for (uint64_t frame : s) {
          if (frame) std::fprintf(f, " 0x%llX", (unsigned long long)frame);
        }
        std::fprintf(f, "%c", 10);
      }
      for (const auto& [key, count] : outside_) {
        std::fprintf(f, "# %lu module %s %llu%c", key.first, key.second.c_str(), (unsigned long long)count, 10);
      }
      for (const auto& [tid, name] : names_) std::fprintf(f, "# %lu name %s%c", tid, name.c_str(), 10);
      std::fclose(f);
    }
  }

 private:
  struct Target {
    DWORD tid;
    HANDLE handle;
  };
  // Every thread of the process but this one (threads come and go: refreshed now and then), and the loaded modules.
  void Refresh(std::vector<Target>& targets) {
    images_.clear();
    HMODULE modules[1024];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
      for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
        MODULEINFO info{};
        if (K32GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info))) {
          images_.push_back({uint64_t(info.lpBaseOfDll), uint64_t(info.lpBaseOfDll) + info.SizeOfImage});
        }
      }
      std::sort(images_.begin(), images_.end());
    }
    for (Target& t : targets) CloseHandle(t.handle);
    targets.clear();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
      const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                  entry.th32ThreadID);
      if (!h) continue;
      targets.push_back({entry.th32ThreadID, h});
      std::lock_guard lock(mutex_);
      if (names_.count(entry.th32ThreadID)) continue;
      PWSTR wide = nullptr;
      std::string name;
      if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
        for (const wchar_t* c = wide; *c; ++c) name += *c > 32 && *c < 127 ? char(*c) : '_';
        LocalFree(wide);
      }
      names_[entry.th32ThreadID] = name.empty() ? "-" : name;
    }
    CloseHandle(snapshot);
  }
  void Run() {
    const auto base = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uint64_t size = nt->OptionalHeader.SizeOfImage;
    auto inside = [&](uint64_t rip) { return rip >= base && rip < base + size; };
    std::vector<Target> targets;
    // Per thread: the sample (with callers in the executable) and the leaf's address. Nothing is allocated or locked
    // while a thread is suspended (it may hold the heap's lock).
    std::vector<std::pair<Frames, uint64_t>> taken;
    auto refreshed = std::chrono::steady_clock::time_point{};
    auto written = std::chrono::steady_clock::now();
    while (!done_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if (std::chrono::steady_clock::now() - written > std::chrono::seconds(5)) {
        Write();  // the samples so far: a title may end its process without destructors
        written = std::chrono::steady_clock::now();
      }
      if (!always_ && gfx::Diagnosing()) continue;
      if (std::chrono::steady_clock::now() - refreshed > std::chrono::milliseconds(500)) {
        Refresh(targets);
        refreshed = std::chrono::steady_clock::now();
      }
      taken.assign(targets.size(), {Frames{}, 0});
      for (size_t i = 0; i < targets.size(); ++i) {
        if (SuspendThread(targets[i].handle) == DWORD(-1)) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(targets[i].handle, &context)) {
          taken[i].second = context.Rip;
          // The leaf (or the first caller in the executable, bit 63), then up to fifteen more callers in it.
          CONTEXT walk = context;
          uint32_t found = 0;
          for (int depth = 0; depth < 64 && walk.Rip && found < kFrames; ++depth) {
            if (inside(walk.Rip)) {
              const bool leaf = depth == 0;
              taken[i].first[found++] = leaf ? walk.Rip - base : (walk.Rip - base - 1) | (found ? 0 : 1ull << 63);
            }
            // Only through loaded images: looking up code generated at run time (a driver's) takes the dynamic
            // function tables' lock, which the suspended thread may hold.
            if (!InImage(walk.Rip)) break;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
            if (!entry) break;  // no unwind data: stop rather than guess
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, entry, &walk, &handler_data, &frame, nullptr);
          }
        }
        ResumeThread(targets[i].handle);
      }
      std::lock_guard lock(mutex_);
      for (size_t i = 0; i < targets.size(); ++i) {
        if (!taken[i].second) continue;
        samples_.push_back({targets[i].tid, taken[i].first});
        if (inside(taken[i].second)) continue;
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(taken[i].second), &module)) {
          GetModuleFileNameA(module, name, MAX_PATH);
        }
        const char* slash = std::strrchr(name, 92);
        ++outside_[{targets[i].tid, slash ? slash + 1 : name}];
      }
    }
    for (Target& t : targets) CloseHandle(t.handle);
  }
  bool InImage(uint64_t rip) const {
    auto it = std::upper_bound(images_.begin(), images_.end(), std::pair<uint64_t, uint64_t>{rip, UINT64_MAX});
    return it != images_.begin() && rip < std::prev(it)->second;
  }
  std::vector<std::pair<uint64_t, uint64_t>> images_;  // loaded modules [begin, end), sorted
  std::string path_;
  bool always_ = false;
  std::thread worker_;
  std::atomic<bool> done_{false};
  std::mutex mutex_;
  static constexpr uint32_t kFrames = 16;
  using Frames = std::array<uint64_t, kFrames>;
  std::vector<std::pair<DWORD, Frames>> samples_;
  std::map<std::pair<DWORD, std::string>, uint64_t> outside_;
  std::map<DWORD, std::string> names_;
};

SwapCost MeasureSwapCost() {
  constexpr size_t kWords = size_t(4) << 20;  // 16 MB
  std::vector<uint32_t> from(kWords), to(kWords);
  for (size_t i = 0; i < kWords; ++i) from[i] = uint32_t(i * 2654435761u);
  auto best = [&](auto&& body) {
    double ns = 1e30;
    for (int run = 0; run < 5; ++run) {
      const auto start = std::chrono::steady_clock::now();
      body();
      ns = std::min(ns, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count());
    }
    return ns;
  };
  const double copy = best([&] { std::memcpy(to.data(), from.data(), kWords * 4); });
  const double vectorized = best([&] {
    for (size_t i = 0; i < kWords; ++i) to[i] = std::byteswap(from[i]);
  });
  const double scalar = best([&] {
#pragma clang loop vectorize(disable) unroll(disable)
    for (size_t i = 0; i < kWords; ++i) to[i] = std::byteswap(from[i]);
  });
  volatile uint32_t keep = to[kWords / 2];
  (void)keep;
  const double bytes = double(kWords) * 4;
  return {std::max(0.0, vectorized - copy) / bytes, std::max(0.0, scalar - copy) / bytes};
}

// CPU writes that hit a memory watch (a fault each), counted by MemoryWritten.
std::atomic<uint64_t> g_watch_writes{0}, g_watch_bytes{0};

// The renderer that presents each frame it swaps, on the command processor's thread (the thread that renders).
class PresentingRenderer : public gfx::vk::VulkanRenderer {
 public:
  // `parent`: the title's window `window` is a child of, kept covering its client area (null: none).
  PresentingRenderer(gfx::vk::Device& device, uint8_t* physical, Window window, Window parent,
                     std::function<void(const std::string&)> log, double stats_seconds, uint32_t resolution_scale)
      : VulkanRenderer(device, physical, kPhysicalBytes, resolution_scale), log_(std::move(log)), stats_seconds_(stats_seconds),
        presenter_(device), window_(window), parent_(parent) {}

  bool Resize() {
    const auto [w, h] = Size();
    if (!w || !h) return true;  // minimised: keep the swapchain
    width_ = w, height_ = h;
    return presenter_.Resize(width_, height_);
  }

  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* registers) override {
    VulkanRenderer::Swap(frontbuffer, width, height, registers);
    const auto [w, h] = Size();
    if ((w != width_ || h != height_) && !Resize()) return;
    const Frame& f = frame();
    if (!f.width || !f.height) return;
    WaitSubmitted();  // the frame's batches on the queue before the present's
    const bool shown = f.on_gpu ? presenter_.Present([this](VkCommandBuffer cb, VkImage target, VkOffset3D a,
                                                            VkOffset3D b) { RecordFrameBlit(cb, target, a, b); },
                                                     f.width, f.height)
                                : presenter_.Present(f.pixels.data(), f.width, f.height);
    if (!shown) {
      Resize();  // out of date: the next frame goes to a new swapchain
    } else if (parent_ && !visible_) {
      // Shown over the title's window from its first frame on: until then the runtime draws there (its launcher).
#ifdef _WIN32
      ShowWindowAsync(window_, SW_SHOWNA);
#endif
      visible_ = true;
    }
    Report();
  }

 private:
  std::pair<uint32_t, uint32_t> Size() const {
#ifdef _WIN32
    RECT r{};
    if (parent_) {
      // The child follows the title's window (asynchronously: the window belongs to the UI thread).
      if (!IsWindow(parent_) || !GetClientRect(parent_, &r)) return {0, 0};
      RECT own{};
      GetClientRect(window_, &own);
      if (own.right != r.right || own.bottom != r.bottom) {
        SetWindowPos(window_, nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      }
      return {uint32_t(r.right), uint32_t(r.bottom)};
    }
    if (!IsWindow(window_) || !GetClientRect(window_, &r)) return {0, 0};
    return {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
#else
    return {0, 0};
#endif
  }

  // Frames per second and the renderer's cost per frame since the last report.
  void Report() {
    ++frames_;
    const auto now = std::chrono::steady_clock::now();
    // The period's slowest frames (the averages hide drops).
    if (frames_ > 1) {
      const double interval = std::chrono::duration<double, std::milli>(now - last_frame_).count();
      worst_ms_ = std::max(worst_ms_, interval);
      slow_frames_ += interval > 1000.0 / 60.0;
      very_slow_frames_ += interval > 1000.0 / 20.0;
    }
    last_frame_ = now;
    if (show_fps_) Overlay(now);
    if (!log_) return;
    if (frames_ == 1) {
      since_ = now;
      last_ = stats();
      return;
    }
    const double seconds = std::chrono::duration<double>(now - since_).count();
    if (seconds < stats_seconds_) return;
    const gfx::vk::RendererStats& s = stats();
    const double n = double(frames_ - 1), ms = 1e6 * n;
    // Every other period runs without the diagnostics (timers, per-packet counts, wait sites, logs of uploads and
    // resolves): only frames counted, to show what they cost, and what remains without byte swapping either.
    const gfx::SwappedBytes& sw = gfx::g_swapped;
    const uint64_t swapped[5] = {sw.registers, sw.indices, sw.textures, sw.write_backs, sw.shaders};
    double swap_bytes = 0, vector_bytes = 0, per_kind[5];
    for (int i = 0; i < 5; ++i) per_kind[i] = double(swapped[i] - last_swapped_[i]) / n, swap_bytes += per_kind[i];
    vector_bytes = double(swapped[0] - last_swapped_[0] + swapped[1] - last_swapped_[1]) / n;  // registers, indices
    const double swap_ms = (vector_bytes * swap_cost_.vectorized + (swap_bytes - vector_bytes) * swap_cost_.scalar) / 1e6;
    for (int i = 0; i < 5; ++i) last_swapped_[i] = swapped[i];
    if (!gfx::Diagnosing()) {
      const double fps = n / seconds, frame_ms = 1e3 / fps;
      const uint64_t watch_writes = g_watch_writes.load(), watch_bytes = g_watch_bytes.load();
      char off[768];
      std::snprintf(off, sizeof(off),
                    "Any360: %.1f FPS with the diagnostics off (%.1f with them, the period before: %+.0f%%); byte swapping "
                    "%.2f MB a frame, ~%.2f ms over plain copies: without it ~%.0f FPS (the command processor's thread bound); "
                    "slowest frame %.1f ms, %u over 16.7 ms, %u over 50 ms; %.1f watched writes a frame (%.2f MB "
                    "invalidated); %.0f MB private, %zu textures, %zu targets, %zu resolves kept, %zu pipelines",
                    fps, last_on_fps_, last_on_fps_ > 0 ? 100.0 * (fps / last_on_fps_ - 1.0) : 0.0, swap_bytes / 1e6, swap_ms,
                    1e3 / std::max(0.1, frame_ms - swap_ms), worst_ms_, slow_frames_, very_slow_frames_,
                    double(watch_writes - last_watch_writes_) / n, double(watch_bytes - last_watch_bytes_) / n / 1e6,
                    double(PrivateBytes()) / 1e6, texture_cache() ? texture_cache()->texture_count() : size_t(0),
                    surface_count(), resolved_count(), pipeline_count());
      log_(off);
      last_watch_writes_ = watch_writes, last_watch_bytes_ = watch_bytes;
      // What accumulated meanwhile is not reported: the next period starts clean.
      if (cp_) {
        last_busy_ = cp_->ns_busy(), last_waiting_ = cp_->ns_waiting(), last_kicks_ = cp_->kicks();
        cp_->TakeWaitSites();
        cp_->TakeWaitTraces();
        for (uint32_t op = 0; op < 129; ++op) last_opcode_ticks_[op] = cp_->opcode_ticks(op);
        last_tsc_ = __rdtsc();
      }
      if (gfx::vk::TextureCache* textures = texture_cache()) {
        textures->upload_log().clear();
        textures->source_misses().clear();
      }
      resolve_paths().clear();
      frames_ = 1;
      since_ = now;
      last_ = s;
      worst_ms_ = 0, slow_frames_ = very_slow_frames_ = 0;
      if (sampler_) sampler_->Write();  // the samples so far (the title may end without StopGraphics)
      gfx::g_diagnostics = true;
      return;
    }
    char line[768];
    std::snprintf(line, sizeof(line),
                  "Any360: %.1f FPS; per frame: %.0f draws, %.0f skipped, %.0f failed, %.0f resolves, %.1f texture uploads, "
                  "%.1f texture copies from resolves, %.1f translations, %.1f pipelines (%.1f draws waiting), %.1f flushes; ms: draws %.2f (textures %.2f, shaders %.2f, "
                  "pipelines %.2f, guest memory %.2f, submit %.2f), resolves %.2f, swaps %.2f; guest memory %s",
                  n / seconds, (s.draws - last_.draws) / n, (s.skipped - last_.skipped) / n, (s.failed - last_.failed) / n,
                  (s.resolves - last_.resolves) / n, (s.texture_uploads - last_.texture_uploads) / n,
                  (s.texture_gpu_copies - last_.texture_gpu_copies) / n,
                  (s.translations - last_.translations) / n, (s.pipelines - last_.pipelines) / n,
                  (s.pipeline_waits - last_.pipeline_waits) / n,
                  (s.flushes - last_.flushes) / n, (s.ns_draws - last_.ns_draws) / ms,
                  (s.ns_textures - last_.ns_textures) / ms, (s.ns_shaders - last_.ns_shaders) / ms,
                  (s.ns_pipelines - last_.ns_pipelines) / ms, (s.ns_memory - last_.ns_memory) / ms,
                  (s.ns_submit - last_.ns_submit) / ms, (s.ns_resolves - last_.ns_resolves) / ms,
                  (s.ns_swaps - last_.ns_swaps) / ms, guest_memory_in_place() ? "in place" : "copied");
    log_(line);
    {
      const double batches = double(s.gpu_batches - last_.gpu_batches);
      char gpu[256];
      std::snprintf(gpu, sizeof(gpu),
                    "Any360: GPU per frame: %.2f ms running the batches (%.1f batches), %.2f ms from a batch's submission "
                    "to its completion on average; %.2f MB of guest memory copied into video memory",
                    (s.ns_gpu - last_.ns_gpu) / ms, batches / n, batches ? (s.ns_gpu_latency - last_.ns_gpu_latency) / 1e6 / batches : 0.0,
                    (s.guest_upload_bytes - last_.guest_upload_bytes) / 1e6 / n);
      log_(gpu);
    }
    if (cp_) {
      const uint64_t busy = cp_->ns_busy(), waiting = cp_->ns_waiting(), kicks = cp_->kicks();
      char line_cp[256];
      std::snprintf(line_cp, sizeof(line_cp),
                    "Any360: command processor per frame: %.2f ms running the ring (%.2f ms of it in WAIT_REG_MEM), "
                    "%.1f kicks; %.1f%% of the time with nothing to run",
                    (busy - last_busy_) / ms, (waiting - last_waiting_) / ms, (kicks - last_kicks_) / n,
                    100.0 * (1.0 - (busy - last_busy_) / 1e9 / seconds));
      log_(line_cp);
      last_busy_ = busy, last_waiting_ = waiting, last_kicks_ = kicks;
      auto sites = cp_->TakeWaitSites();
      std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.ns > b.ns; });
      std::string waits = "Any360: WAIT_REG_MEM per frame by site:";
      for (size_t i = 0; i < sites.size() && i < 6; ++i) {
        char line[200];
        std::snprintf(line, sizeof(line), " %s %08X func %u ref %08X: %.1f waits %.2f ms (%.1f queued ahead);",
                      (sites[i].wait_info & 0x10) ? "memory" : "register", sites[i].poll, sites[i].wait_info & 7,
                      sites[i].reference, sites[i].count / double(n), sites[i].ns / ms, sites[i].ahead / double(n));
        waits += line;
      }
      log_(waits);
      for (const std::string& trace : cp_->TakeWaitTraces()) log_("Any360: " + trace);
      // The ring's time by packet type (time-stamp counter ticks, scaled by the ticks this report spans).
      const uint64_t tsc = __rdtsc();
      if (last_tsc_) {
        const double ticks_per_ms = double(tsc - last_tsc_) / (seconds * 1e3);
        std::vector<std::pair<uint64_t, uint32_t>> by_opcode;
        for (uint32_t op = 0; op < 129; ++op) {
          const uint64_t ticks = cp_->opcode_ticks(op);
          if (ticks > last_opcode_ticks_[op]) by_opcode.push_back({ticks - last_opcode_ticks_[op], op});
          last_opcode_ticks_[op] = ticks;
        }
        std::sort(by_opcode.begin(), by_opcode.end(), std::greater<>());
        std::string ops = "Any360: ring ms per frame by packet:";
        for (size_t i = 0; i < by_opcode.size() && i < 10; ++i) {
          char item[64];
          if (by_opcode[i].second == 128) {
            std::snprintf(item, sizeof(item), " registers %.2f;", by_opcode[i].first / ticks_per_ms / n);
          } else {
            std::snprintf(item, sizeof(item), " %02X %.2f;", by_opcode[i].second, by_opcode[i].first / ticks_per_ms / n);
          }
          ops += item;
        }
        log_(ops);
      } else {
        for (uint32_t op = 0; op < 129; ++op) last_opcode_ticks_[op] = cp_->opcode_ticks(op);
      }
      last_tsc_ = tsc;
    }
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "Any360: per frame: resolves %.1f on the GPU, %.1f read back, %.1f depth, %.1f written back later; "
                  "textures ms: hashing %.2f, "
                  "loading for uploads %.2f",
                  (s.resolves_on_gpu - last_.resolves_on_gpu) / n, (s.resolves_read_back - last_.resolves_read_back) / n,
                  (s.resolves_depth - last_.resolves_depth) / n, (s.resolve_write_backs - last_.resolve_write_backs) / n,
                  (s.ns_texture_hash - last_.ns_texture_hash) / ms,
                  (s.ns_texture_load - last_.ns_texture_load) / ms);
    log_(detail);
    if (gfx::vk::TextureCache* textures = texture_cache()) {
      // The textures uploaded most since the last report: base, format, size, and what last resolved there.
      std::vector<std::pair<uint32_t, std::array<uint32_t, 4>>> top;
      for (const auto& [key, count] : textures->upload_log()) top.emplace_back(count, key);
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string uploads = "Any360: uploaded most:";
      for (size_t i = 0; i < top.size() && i < 10; ++i) {
        const auto& [count, key] = top[i];
        const uint32_t resolved = ResolveFormatAt(key[0]);
        char item[160];
        std::snprintf(item, sizeof(item), " %08X fmt %u %ux%u x%u%s;", key[0], key[1], key[2], key[3], count,
                      resolved == UINT32_MAX ? "" : (resolved & 0x100 ? " (depth resolve dest)" : " (resolve dest)"));
        uploads += item;
      }
      log_(uploads);
      textures->upload_log().clear();
      if (!textures->source_misses().empty()) {
        std::string misses = "Any360: textures over a GPU resolve not using it:";
        for (const auto& [why, count] : textures->source_misses()) misses += " " + why + " x" + std::to_string(count) + ";";
        log_(misses);
        textures->source_misses().clear();
      }
    }
    std::string paths = "Any360: resolves by path:";
    for (const auto& [path, count] : resolve_paths()) paths += " " + path + " x" + std::to_string(count) + ";";
    log_(paths);
    resolve_paths().clear();
    if (s.failed != last_.failed) {
      std::string reasons = "Any360: draws not drawn so far:";
      for (const auto& [reason, count] : failures()) reasons += " " + reason + " (" + std::to_string(count) + ");";
      log_(reasons);
    }
    {
      char swaps[320];
      std::snprintf(swaps, sizeof(swaps),
                    "Any360: byte swapping per frame: %.2f MB (registers %.2f, indices %.2f, textures %.2f, write-backs "
                    "%.2f, shaders %.2f), ~%.2f ms over plain copies (%.3f / %.3f ns a byte vectorized / not)",
                    swap_bytes / 1e6, per_kind[0] / 1e6, per_kind[1] / 1e6, per_kind[2] / 1e6, per_kind[3] / 1e6,
                    per_kind[4] / 1e6, swap_ms, swap_cost_.vectorized, swap_cost_.scalar);
      log_(swaps);
    }
    last_on_fps_ = n / seconds;
    gfx::g_diagnostics = false;  // the next period without them
    frames_ = 1;
    since_ = now;
    last_ = s;
    worst_ms_ = 0, slow_frames_ = very_slow_frames_ = 0;
  }

  // The counter's text, every half second: frames per second, average / slowest frame (ms), the GPU's time per frame
  // (the batches' timestamps) and the command processor's idle share.
  void Overlay(std::chrono::steady_clock::time_point now) {
    if (!overlay_frames_++) {
      overlay_since_ = now;
      overlay_stats_ = stats();
      overlay_busy_ = cp_ ? cp_->ns_busy() : 0;
      overlay_worst_ = 0;
      return;
    }
    if (overlay_frames_ > 2) {
      overlay_worst_ = std::max(overlay_worst_, std::chrono::duration<double, std::milli>(now - overlay_last_).count());
    }
    overlay_last_ = now;
    const double seconds = std::chrono::duration<double>(now - overlay_since_).count();
    if (seconds < 0.5) return;
    const double n = double(overlay_frames_ - 1);
    const gfx::vk::RendererStats& s = stats();
    const uint64_t busy = cp_ ? cp_->ns_busy() : 0;
    char text[128];
    std::snprintf(text, sizeof(text), "%.0f FPS %.1f ms (max %.1f) GPU %.1f ms CP idle %.0f%%", n / seconds,
                  1e3 * seconds / n, overlay_worst_, (s.ns_gpu - overlay_stats_.ns_gpu) / 1e6 / n,
                  cp_ ? 100.0 * std::max(0.0, 1.0 - double(busy - overlay_busy_) / 1e9 / seconds) : 0.0);
    presenter_.SetOverlay(text);
    overlay_frames_ = 1;
    overlay_since_ = now;
    overlay_stats_ = s;
    overlay_busy_ = busy;
    overlay_worst_ = 0;
  }

  std::function<void(const std::string&)> log_;
  double stats_seconds_;
 public:
  bool show_fps_ = false;
 private:
  uint64_t overlay_frames_ = 0, overlay_busy_ = 0;
  double overlay_worst_ = 0;
  std::chrono::steady_clock::time_point overlay_since_, overlay_last_;
  gfx::vk::RendererStats overlay_stats_;
 public:
  gfx::CommandProcessor* cp_ = nullptr;  // for the report: where the command processor's time goes
 private:
  uint64_t last_busy_ = 0, last_waiting_ = 0, last_kicks_ = 0;
  uint64_t last_tsc_ = 0;
  uint64_t last_swapped_[5] = {};
  double last_on_fps_ = 0;
 public:
  SwapCost swap_cost_;
  Sampler* sampler_ = nullptr;
 private:
  std::array<uint64_t, 129> last_opcode_ticks_{};
  uint64_t frames_ = 0;
  std::chrono::steady_clock::time_point since_;
  std::chrono::steady_clock::time_point last_frame_;
  uint64_t last_watch_writes_ = 0, last_watch_bytes_ = 0;
  static uint64_t PrivateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
      return counters.PrivateUsage;
    }
#endif
    return 0;
  }
  double worst_ms_ = 0;
  uint32_t slow_frames_ = 0, very_slow_frames_ = 0;
  gfx::vk::RendererStats last_;

  gfx::vk::Presenter presenter_;
  Window window_, parent_;
  uint32_t width_ = 0, height_ = 0;
  bool visible_ = false;
};

#ifdef _WIN32
// The child window lets the mouse through to the title's window under it.
LRESULT CALLBACK ChildWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcA(hwnd, message, wparam, lparam);
}

// The title's window: the largest visible top-level window of the calling thread.
HWND FindTitleWindow() {
  struct Best {
    HWND hwnd = nullptr;
    LONG area = 0;
  } best;
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND hwnd, LPARAM user) -> BOOL {
        auto& b = *reinterpret_cast<Best*>(user);
        RECT r{};
        if (IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) && GetClientRect(hwnd, &r) &&
            r.right * r.bottom > b.area) {
          b = {hwnd, r.right * r.bottom};
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&best));
  return best.hwnd;
}
#endif

// The title's interrupt callback, run on a runtime host thread (the runtime's kernel needs its own thread state),
// synchronously for the thread that raises it (the command processor's or the vertical blank's), as the GPU does.
class InterruptThread {
 public:
  void Start(rex::system::KernelState* kernel) {
    kernel_ = kernel;
    thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel, 128 * 1024, 0, [this] { return Run(); }));
    thread_->Create();
  }
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
  }
  // Called on any thread; returns once the callback has run.
  void Raise(uint32_t callback, uint32_t source, uint32_t user_data) {
    std::unique_lock lock(mutex_);
    if (stopping_) return;
    const uint64_t ticket = ++queued_;
    requests_.push_back({callback, source, user_data});
    wake_.notify_all();
    done_.wait(lock, [&] { return stopping_ || served_ >= ticket; });
  }

 private:
  struct Request {
    uint32_t callback, source, user_data;
  };
  int Run() {
    auto* thread = rex::system::XThread::GetCurrentThread();
    std::unique_lock lock(mutex_);
    while (true) {
      wake_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
      if (stopping_) break;
      const Request r = requests_.front();
      requests_.pop_front();
      lock.unlock();
      thread->SetActiveCpu(2);  // the reference's choice for GPU interrupts
      uint64_t args[] = {r.source, r.user_data};
      kernel_->function_dispatcher()->ExecuteInterrupt(thread->thread_state(), r.callback, args, 2);
      lock.lock();
      ++served_;
      done_.notify_all();
    }
    done_.notify_all();
    return 0;
  }

  rex::system::KernelState* kernel_ = nullptr;
  rex::system::object_ref<rex::system::XHostThread> thread_;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  std::deque<Request> requests_;
  uint64_t queued_ = 0, served_ = 0;
  bool stopping_ = false;
};

struct Binding {
  Window window = nullptr, parent = nullptr;
  gfx::vk::Device device;
  std::unique_ptr<PresentingRenderer> renderer;
  std::unique_ptr<gfx::CommandProcessor> cp;
  std::unique_ptr<gfx::AsyncRenderer> async;
  InterruptThread interrupts;
  std::unique_ptr<Sampler> sampler;
  // A frame capture: requested by a trigger file, opened and closed on the title's thread at a kick while the
  // command processor is idle (it records each kick from its own thread), so the writer never changes under it.
  uint8_t* physical = nullptr;
  std::string capture_path;
  uint32_t capture_kicks = 0, kicks_left = 0;
  std::unique_ptr<gfx::capture::Writer> writer;
  std::chrono::steady_clock::time_point next_trigger_check{};
  std::function<void(const std::string&)> log;
};

std::atomic<bool> g_bound{false};
Binding* g_binding = nullptr;

// A CPU write to watched guest memory (the runtime's physical memory access callbacks): the textures over it are
// checked again before their next use. The watch is one-shot; the texture cache arms it again when it reads the
// memory. Only the range invalidated here may be unwatched: the runtime would otherwise drop the watch of up to 4 MB
// around the write without telling us, and what we hold of those pages would go stale.
std::pair<uint32_t, uint32_t> MemoryWritten(void* context, uint32_t start, uint32_t length, bool) {
  g_watch_writes.fetch_add(1, std::memory_order_relaxed);
  g_watch_bytes.fetch_add(length, std::memory_order_relaxed);
  auto* renderer = static_cast<gfx::vk::VulkanRenderer*>(context);
  if (auto* textures = renderer->texture_cache()) textures->QueueMemoryChanged(start, start + length);
  renderer->GuestMemoryWritten(start, start + length);  // the vertices copied into video memory
  return {start, length};
}

uint32_t ReadGpuRegister(void*, void*, uint32_t address) { return sys::Graphics().ReadRegister(sys::GpuRegisterIndex(address)); }

// At a kick (the title's write of CP_RB_WPTR, before the command processor sees it): starts a requested capture, or
// ends one whose kicks are done, while the command processor is idle.
void CaptureAtKick(Binding& b) {
  if (b.capture_path.empty()) return;
  if (b.writer) {
    if (b.kicks_left > 0) --b.kicks_left;
    if (b.kicks_left == 0 && b.cp->Idle()) {
      b.cp->SetCapture(nullptr);
      const uint64_t bytes = b.writer->bytes_written();
      b.writer.reset();
      if (b.log) b.log("Any360: capture written to " + b.capture_path + " (" + std::to_string(bytes >> 20) + " MB)");
    }
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < b.next_trigger_check) return;
  b.next_trigger_check = now + std::chrono::milliseconds(500);
  const std::string trigger = b.capture_path + ".go";
  if (std::FILE* f = std::fopen(trigger.c_str(), "rb")) {
    std::fclose(f);
  } else {
    return;
  }
  if (!b.cp->Idle()) return;  // at a later kick
  std::remove(trigger.c_str());
  auto writer = std::make_unique<gfx::capture::Writer>();
  if (!writer->Open(b.capture_path, b.physical, kPhysicalBytes, b.cp->registers(), gfx::reg::kRegisterCount,
                    "any360::host::rexglue binding")) {
    if (b.log) b.log("Any360: cannot open the capture " + b.capture_path);
    return;
  }
  writer->Shaders(b.cp->shader(0).microcode, b.cp->shader(1).microcode);
  b.kicks_left = b.capture_kicks;
  b.writer = std::move(writer);
  b.cp->SetCapture(b.writer.get());
  if (b.log) b.log("Any360: capturing " + std::to_string(b.capture_kicks) + " kicks into " + b.capture_path);
}

void WriteGpuRegister(void*, void*, uint32_t address, uint32_t value) {
  const uint32_t index = sys::GpuRegisterIndex(address);
  if (index == gfx::reg::CP_RB_WPTR && g_binding) CaptureAtKick(*g_binding);
  sys::Graphics().WriteRegister(index, value);
}

}  // namespace

void StartSampling(const std::string& path) {
  static std::unique_ptr<Sampler> sampler;
  if (!sampler && !path.empty()) sampler = std::make_unique<Sampler>(path, true);
}

bool StartGraphics(const GraphicsOptions& options, std::string& error) {
  if (g_bound) return true;
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::KernelState* kernel = runtime ? runtime->kernel_state() : nullptr;
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  rex::runtime::MMIOHandler* mmio = rex::runtime::MMIOHandler::global_handler();
  rex::runtime::MMIORange* range = mmio ? mmio->LookupRange(kGpuMmio) : nullptr;
  if (!kernel || !memory || !range) {
    error = "the runtime is not set up (kernel, memory or the GPU MMIO range missing)";
    return false;
  }
  auto binding = std::make_unique<Binding>();

  // The window and the device that presents to it.
  gfx::vk::DeviceOptions device_options;
  device_options.prefer = options.gpu;
#ifdef _WIN32
  const HINSTANCE instance = GetModuleHandleA(nullptr);
  WNDCLASSEXA window_class{sizeof(WNDCLASSEXA)};
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = ChildWindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
  window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = "Any360Window";
  RegisterClassExA(&window_class);  // fails harmlessly when already registered
  binding->parent = options.embed ? FindTitleWindow() : nullptr;
  if (binding->parent) {
    // Inside the title's window: its own painting must leave the child alone.
    SetWindowLongPtrA(binding->parent, GWL_STYLE, GetWindowLongPtrA(binding->parent, GWL_STYLE) | WS_CLIPCHILDREN);
    RECT client{};
    GetClientRect(binding->parent, &client);
    binding->window = CreateWindowExA(WS_EX_NOPARENTNOTIFY, "Any360Window", "", WS_CHILD | WS_DISABLED,
                                      0, 0, client.right, client.bottom, binding->parent, nullptr, instance, nullptr);
  } else {
    RECT frame{0, 0, LONG(options.width), LONG(options.height)};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    binding->window = CreateWindowExA(0, "Any360Window", "Any360 - any360::gfx", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                                      nullptr, nullptr, instance, nullptr);
  }
  if (!binding->window) {
    error = "window: CreateWindowEx failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  const HWND window = binding->window;
  device_options.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  device_options.make_surface = [window](VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetInstanceProcAddr")));
    auto create = get ? reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get(instance, "vkCreateWin32SurfaceKHR")) : nullptr;
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleA(nullptr);
    info.hwnd = window;
    if (!create || create(instance, &info, nullptr, &surface) != VK_SUCCESS) return VkSurfaceKHR(VK_NULL_HANDLE);
    return surface;
  };
#else
  error = "window surfaces other than Win32 are not wired yet";
  return false;
#endif
  if (!binding->device.Create(device_options)) {
    error = "Vulkan: " + binding->device.error();
    DestroyWindow(binding->window);
    return false;
  }

  // The renderer over the guest's physical memory, the command processor in front of it.
  uint8_t* physical = memory->TranslatePhysical<uint8_t*>(0);
  binding->physical = physical;
  binding->capture_path = options.capture_path;
  binding->capture_kicks = std::max(1u, options.capture_kicks);
  binding->log = options.log;
  binding->renderer = std::make_unique<PresentingRenderer>(binding->device, physical, binding->window, binding->parent,
                                                          options.log, options.stats_seconds, options.resolution_scale);
  binding->renderer->SetPipelineCache(options.pipeline_cache_path);
  binding->renderer->show_fps_ = options.show_fps;
  binding->renderer->SetAsyncPipelines(options.async_pipelines);
  binding->renderer->SetMsaa(options.msaa_samples);
  if (!binding->renderer->Initialize() || !binding->renderer->Resize()) {
    error = "renderer: " + binding->renderer->last_error();
    binding->renderer.reset();
    DestroyWindow(binding->window);
    return false;
  }
  // Textures are checked again only after the CPU writes their memory, as the runtime's own GPU does.
  if (gfx::vk::TextureCache* textures = binding->renderer->texture_cache()) {
    memory->RegisterPhysicalMemoryInvalidationCallback(&MemoryWritten, binding->renderer.get());
    textures->SetMemoryWatch([memory](uint32_t begin, uint32_t end) {
      memory->EnablePhysicalMemoryAccessCallbacks(begin, end - begin, true, false);
    });
    binding->renderer->SetMemoryWatch([memory](uint32_t begin, uint32_t end) {
      memory->EnablePhysicalMemoryAccessCallbacks(begin, end - begin, true, false);
    });
  }
  binding->cp = std::make_unique<gfx::CommandProcessor>(physical);
  binding->renderer->SetRecordThreads(options.record_threads < 0 ? gfx::vk::VulkanRenderer::AutoRecordThreads()
                                                                 : uint32_t(options.record_threads));
  if (options.log) options.log("Any360: draws recorded on " + std::to_string(std::max(1u, binding->renderer->record_threads())) +
                               " thread(s) (" + std::to_string(std::thread::hardware_concurrency()) + " hardware threads)");
  if (options.log) {
    options.log("Any360: render targets at " + std::to_string(binding->renderer->resolution_scale()) + "x the title's resolution, " +
                std::to_string(binding->renderer->msaa()) + " sample(s) a pixel (host MSAA)");
  }
  if (options.async_renderer) {
    binding->async = std::make_unique<gfx::AsyncRenderer>(*binding->cp, *binding->renderer);
    binding->cp->SetRenderer(binding->async.get());
  } else {
    binding->cp->SetRenderer(binding->renderer.get());
  }
  binding->renderer->cp_ = binding->cp.get();
  if (options.log) binding->renderer->swap_cost_ = MeasureSwapCost();  // for the report (~0.1 s)
  binding->cp->SetDisplaySize(options.width, options.height);

  // any360::sys's graphics kernel: the command processor as its backend, interrupts through the runtime's thread.
  binding->interrupts.Start(kernel);
  sys::Objects().SetGuest({memory->virtual_membase()});
  sys::Graphics().Reset();
  sys::Graphics().SetBackend(binding->cp.get());
  sys::Graphics().SetInterruptDispatch(
      [](uint32_t callback, uint32_t source, uint32_t user_data, void* user) {
        static_cast<Binding*>(user)->interrupts.Raise(callback, source, user_data);
      },
      binding.get());

  // The GPU MMIO range the runtime registered now reaches the graphics kernel.
  range->callback_context = binding.get();
  range->read = &ReadGpuRegister;
  range->write = &WriteGpuRegister;

  binding->cp->Start();
  if (!options.profile_path.empty()) {
    binding->sampler = std::make_unique<Sampler>(options.profile_path);
    binding->renderer->sampler_ = binding->sampler.get();
  }
  double vblank_hz = options.vblank_hz;
  const bool vsync = rex::cvar::GetFlagByName("vsync") == "true";
  if (vblank_hz <= 0.0) vblank_hz = vsync ? 60.0 : 1000.0;
  // As the runtime's own command processor: without vsync, waits spin.
  binding->cp->SetSpinWaits(!vsync && options.spin != 0);
  if (binding->async) binding->async->SetSpin(options.spin != 0);
  binding->cp->SetWritesAfterDraws(options.writes_after_draws);
  binding->cp->SetFencesAfterDraws(options.fences_after_draws);
  binding->renderer->SetFencesWhenCaptured(options.fences_when_captured);
  // What the command processor writes into guest memory itself (fences, query counts), the page watch does not see:
  // the vertices copied into video memory over it are stale (textures are not kept over such words).
  binding->cp->SetMemoryWrittenHook([renderer = binding->renderer.get()](uint32_t begin, uint32_t end) {
    renderer->GuestMemoryWritten(begin, end);
  });
  sys::Graphics().StartVblank(vblank_hz);
  g_binding = binding.release();
  g_bound = true;
  return true;
}

void StopGraphics() {
  if (!g_binding) return;
  sys::Graphics().StopVblank();
  g_binding->interrupts.Stop();
  g_binding->sampler.reset();  // its samples written
  g_binding->cp->Stop();
  if (g_binding->writer) {  // a capture still running: what it has is kept
    g_binding->cp->SetCapture(nullptr);
    g_binding->writer.reset();
  }
  if (g_binding->async) g_binding->async->Drain();
  // The window, device and renderer stay until the process ends: the runtime may still be tearing down around them.
}

bool GraphicsBound() { return g_bound; }

uint64_t GpuProgress() { return g_binding ? g_binding->cp->Progress() : 0; }

void WaitGpuProgress(uint64_t seen, uint32_t timeout_us) {
  if (g_binding) g_binding->cp->WaitProgress(seen, std::chrono::microseconds(timeout_us));
}

GuestFunction ReferenceExport(const char* symbol) {
#ifdef _WIN32
  for (const char* module : {"rexruntime.dll", "rexruntimerd.dll", "rexruntimed.dll"}) {
    if (HMODULE handle = GetModuleHandleA(module)) {
      if (FARPROC f = GetProcAddress(handle, symbol)) return reinterpret_cast<GuestFunction>(reinterpret_cast<void*>(f));
    }
  }
  return nullptr;
#else
  return reinterpret_cast<GuestFunction>(dlsym(RTLD_NEXT, symbol));
#endif
}

namespace {
std::function<void(const std::string&)> g_runtime_frames_log;
double g_runtime_frames_seconds = 5.0;

// The runtime's VdSwap calls: frames per second and the slowest frame, every g_runtime_frames_seconds.
void CountRuntimeFrame() {
  static uint64_t frames = 0;
  static double worst_ms = 0;
  static uint32_t slow = 0;
  static std::chrono::steady_clock::time_point since, last;
  const auto now = std::chrono::steady_clock::now();
  if (frames++) {
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    worst_ms = std::max(worst_ms, ms);
    slow += ms > 1000.0 / 55.0;
  } else {
    since = now;
  }
  last = now;
  const double seconds = std::chrono::duration<double>(now - since).count();
  if (frames < 2 || seconds < g_runtime_frames_seconds) return;
  char line[200];
  std::snprintf(line, sizeof(line), "Any360: the runtime's GPU: %.1f FPS (VdSwap calls); slowest frame %.1f ms, %u over 18 ms",
                double(frames - 1) / seconds, worst_ms, slow);
  g_runtime_frames_log(line);
  frames = 1, since = now, worst_ms = 0, slow = 0;
}
}  // namespace

void ReportRuntimeFrames(std::function<void(const std::string&)> log, double seconds) {
  g_runtime_frames_log = std::move(log);
  g_runtime_frames_seconds = std::max(0.5, seconds);
}

void CallGraphicsExport(const char* symbol, GuestFunction any360, PPCContext& ctx, uint8_t* base) {
  if (g_bound) {
    any360(ctx, base);
    return;
  }
  // The runtime's own export: looked up once per name (the overrides are a handful, each with its own call site).
  static std::mutex mutex;
  static std::vector<std::pair<const char*, GuestFunction>> cache;
  GuestFunction reference = nullptr;
  {
    std::lock_guard lock(mutex);
    for (const auto& [name, f] : cache) {
      if (name == symbol) reference = f;
    }
    if (!reference) {
      reference = ReferenceExport(symbol);
      cache.emplace_back(symbol, reference);
    }
  }
  if (!reference) {
    std::fprintf(stderr, "any360: the runtime has no %s\n", symbol);
    std::abort();
  }
  if (g_runtime_frames_log && std::strcmp(symbol, "__imp__VdSwap") == 0) CountRuntimeFrame();
  reference(ctx, base);
}

}  // namespace any360::host::rexglue
