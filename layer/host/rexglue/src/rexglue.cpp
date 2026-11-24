// any360::host::rexglue: see the header. The graphics binding inside a ReXGlue title: any360::sys's GraphicsKernel and
// any360::gfx behind the title's Vd* calls and GPU MMIO, interrupts run on a runtime host thread, frames presented
// into the title's own window, the runtime's display window, on every system: a Vulkan surface on it from the start
// (Win32 on Windows; elsewhere through the runtime's SDL, exported by its library: X11 or Wayland), and at our first
// frame the runtime's presenter is detached from it (its supported "detached" state) so the swapchain is ours. Input
// and focus stay the title window's, as before.
#include "any360/host/rexglue.h"
#include "any360/sys/activity.h"
#include "any360/host/hitch_tracer.h"
#include "any360/host/profiler.h"
#include "any360/host/hotkeys.h"
#include "any360/host/players.h"
#include "any360/host/settings.h"

#include "any360/gfx/png.h"
#include "any360/gfx/glyph_pages.h"
#include "any360/gfx/async_renderer.h"
#include "any360/gfx/capture.h"
#include "any360/gfx/registers.h"

#include <filesystem>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include <algorithm>
#include <string>
#include <bit>
#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/frame_counter.h"
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
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#ifdef _WIN32
using Window = HWND;
#else
using Window = SDL_Window*;
#endif

namespace any360::host::rexglue {
namespace {

constexpr uint32_t kPhysicalBytes = 0x20000000;  // 512 MB

// The title adapter's glyph pages (SetGlyphPages), patched again at each frame whose glyph answers changed.
std::mutex g_glyph_mutex;
std::unique_ptr<gfx::GlyphPages> g_glyph_pages;
std::vector<gfx::GlyphPage> g_glyph_pending;  // set before the graphics started
bool g_glyph_pending_set = false;
std::string g_glyphs_dir;
std::function<void(const std::string&)> g_glyph_log;
std::string g_glyph_error;  // the last one reported
// The thread that updates them, woken at each swap (loading a set and fitting its art take milliseconds: never on the
// renderer's thread).
std::thread g_glyph_thread;
std::condition_variable g_glyph_wake;
std::atomic<bool> g_glyph_swapped{false};  // set by Swap without the lock (the update holds it for milliseconds)
bool g_glyph_stop = false;

// The folder of the running executable.
std::filesystem::path ExecutableFolder() {
#ifdef _WIN32
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return n ? std::filesystem::path(std::wstring(path, n)).parent_path() : std::filesystem::path();
#else
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

// Once a frame (woken by the renderer's Swap), g_glyph_mutex held: nothing but a generation read while no answer
// changed.
void UpdateGlyphPages() {
  if (!g_glyph_pages) return;
  const auto start = std::chrono::steady_clock::now();
  if (!g_glyph_pages->Update(PlayerGlyphGeneration(), PlayerGlyph, gfx::TexturePatches::Global())) return;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!g_glyph_log) return;
  const sys::Glyph g = PlayerGlyph(0, sys::XboxControl::kA);
  char line[200];
  std::snprintf(line, sizeof(line), "Any360: glyph pages: player 1's prompts drawn with the %.*s set (%.1f ms)",
                int(g.set.size()), g.set.data(), ms);
  g_glyph_log(line);
  if (g_glyph_pages->last_error() != g_glyph_error) {
    g_glyph_error = g_glyph_pages->last_error();
    g_glyph_log("Any360: glyph pages: " + g_glyph_error);
  }
}

// The title's user data folder (--user_data_root), where the keys write; empty in tool mode.
std::filesystem::path UserDataRoot() {
  rex::Runtime* runtime = rex::Runtime::instance();
  return runtime ? runtime->user_data_root() : std::filesystem::path();
}
constexpr uint32_t kGpuMmio = 0x7FC80000;

// What a byte swap costs over a plain copy on this machine, in ns a byte: vectorized (indices, register runs) and one
// word at a time (texels and resolved elements, swapped between other work). Measured once, best of a few runs.
struct SwapCost {
  double vectorized = 0, scalar = 0;
};
// Samples every thread of the process (the title's, the runtime's, ours) every 2 ms while the diagnostics are off:
// where each is, as offsets in the executable, or, in another module (waits in ntdll, the driver), the first caller in
// the executable (bit 63 set), with the leaf's module counted apart.
#ifdef _WIN32
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
#else
// No thread sampling outside Windows yet (the profile option does nothing there): the samples need the Win32
// thread snapshot and unwinder.
class Sampler {
 public:
  explicit Sampler(std::string, bool = false) {}
  void Write() {}
};
#endif

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
  // `window`: the title's window (`title`, the runtime's), presented to from our first frame on.
  PresentingRenderer(gfx::vk::Device& device, uint8_t* physical, Window window, rex::ui::Window* title,
                     std::function<void(const std::string&)> log, double stats_seconds, uint32_t resolution_scale)
      : VulkanRenderer(device, physical, kPhysicalBytes, resolution_scale), log_(std::move(log)), stats_seconds_(stats_seconds),
        presenter_(device), window_(window), title_(title) {
    // ANY360_PRESENT_MODE=mailbox|immediate takes that present mode when the surface has it (FIFO otherwise: see
    // Presenter::PreferPresentMode).
    if (const char* mode = std::getenv("ANY360_PRESENT_MODE")) {
      const std::string m = mode;
      if (m == "fifo") presenter_.PreferPresentMode(VK_PRESENT_MODE_FIFO_KHR);
      if (m == "mailbox") presenter_.PreferPresentMode(VK_PRESENT_MODE_MAILBOX_KHR);
      if (m == "immediate") presenter_.PreferPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR);
    }
  }

  bool Resize() {
    const auto [w, h] = Size();
    if (!w || !h) return true;  // minimised: keep the swapchain
    width_ = w, height_ = h;
    const bool made = presenter_.Resize(width_, height_);
    if (!made) {
      PresentFailed("swapchain");
    } else if (log_) {
      log_("Any360: swapchain " + presenter_.Describe() + " for a " + std::to_string(w) + "x" + std::to_string(h) +
           " window");
    }
    return made;
  }

  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* registers) override {
    VulkanRenderer::Swap(frontbuffer, width, height, registers);
    g_glyph_swapped.store(true, std::memory_order_release);  // the glyph thread updates the pages; the next frames
    g_glyph_wake.notify_one();                               // see the patches
    if (!title_) {  // no window (tool mode): rendered, not presented
      Screenshot();
      Report();
      return;
    }
    // The window stays the runtime's (its launcher) until our first frame; then its presenter lets go of it on the UI
    // thread, and the frames after that are presented here.
    if (!owned_->load(std::memory_order_acquire)) {
      TakeWindow();
      Report();
      return;
    }
    const auto [w, h] = Size();
    if ((w != width_ || h != height_) && !Resize()) return;
    const Frame& f = frame();
    if (!f.width || !f.height) return;
    WaitSubmitted();  // the frame's batches on the queue before the present's
    const bool shown = f.on_gpu ? presenter_.Present([this](VkCommandBuffer cb, VkImage target, VkOffset3D a,
                                                            VkOffset3D b) { RecordFrameBlit(cb, target, a, b); },
                                                     f.width, f.height)
                                : presenter_.Present(f.pixels.data(), f.width, f.height);
    if (!shown) {  // out of date: the next frame goes to a new swapchain
      PresentFailed("present");
      Resize();
    }
    Screenshot();
    Report();
  }

  void SetScreenshotPath(std::string path) { screenshot_path_ = std::move(path); }
  ~PresentingRenderer() override {
    {
      std::lock_guard lock(burst_mutex_);
      burst_stop_ = true;  // what is queued is still written
    }
    burst_ready_.notify_one();
    if (burst_writer_.joinable()) burst_writer_.join();
  }

 private:
  // The title's window in pixels, once it is ours (before that: no swapchain, the runtime's presenter has it).
  std::pair<uint32_t, uint32_t> Size() const {
    if (!owned_->load(std::memory_order_acquire)) return {0, 0};
#ifdef _WIN32
    RECT r{};
    if (!IsWindow(window_) || !GetClientRect(window_, &r)) return {0, 0};
    return {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
#else
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &w, &h)) return {0, 0};
    return {uint32_t(std::max(w, 0)), uint32_t(std::max(h, 0))};
#endif
  }

  // Asks the runtime's UI thread, once, to detach its presenter from the title's window (Window::SetPresenter(nullptr),
  // its detached mode): a native window takes one swapchain at a time. The flag is shared with the UI thread's call,
  // which may run after this renderer is gone.
  void TakeWindow() {
    if (take_asked_) return;
    take_asked_ = true;
    rex::ui::Window* window = title_;
    auto owned = owned_;
    if (!window || !window->app_context().CallInUIThread([window, owned] {
          window->SetPresenter(nullptr);
          owned->store(true, std::memory_order_release);
        })) {
      owned->store(true, std::memory_order_release);
    }
    if (log_) log_("Any360: presenting in the title's window (the runtime's presenter detached from it)");
  }
  std::shared_ptr<std::atomic<bool>> owned_ = std::make_shared<std::atomic<bool>>(false);
  bool take_asked_ = false;

  // A frame the window did not get (its swapchain not made, or the present refused): the first ones logged with the
  // presenter's reason, all counted in the report (a window that stays black says why).
  void PresentFailed(const char* step) {
    ++present_failures_;
    if (log_ && present_failures_ <= 5) {
      log_(std::string("Any360: the window did not get a frame (") + step + "): " + presenter_.error() +
           (present_failures_ == 5 ? "; the next failures are only counted" : ""));
    }
  }
  uint64_t present_failures_ = 0, last_present_failures_ = 0, last_presented_ = 0;

  // The presented frame as a PNG when the trigger file appears (looked for every 15 frames). A burst: when
  // <path>.burst appears, the next frames it names (900 if it is empty) at half size, as raw PPM files in
  // <path>_burst, written by a thread of their own (what flickers for a frame or two is caught that way).
  void Screenshot() {
    if (screenshot_key_.exchange(false, std::memory_order_relaxed)) {  // the screenshot key (hotkeys.h)
      WriteScreenshot(NextNumberedPath(TitleDataFolder(UserDataRoot(), "screenshots"), "any360_", ".png").string());
    }
    if (screenshot_path_.empty()) return;
    if (burst_left_) BurstFrame();
    if (++screenshot_poll_ % 15) return;
    std::error_code error;
    const std::filesystem::path burst_trigger = screenshot_path_ + ".burst";
    if (!burst_left_ && std::filesystem::exists(burst_trigger, error)) {
      uint32_t frames = 900;
      if (std::FILE* file = std::fopen(burst_trigger.string().c_str(), "rb")) {
        unsigned n = 0;
        if (std::fscanf(file, "%u", &n) == 1 && n) frames = n;
        std::fclose(file);
      }
      std::filesystem::remove(burst_trigger, error);
      burst_dir_ = screenshot_path_ + "_burst";
      std::filesystem::create_directories(burst_dir_, error);
      burst_left_ = frames, burst_index_ = 0;
      if (log_) log_("Any360: recording " + std::to_string(frames) + " frames into " + burst_dir_);
    }
    const std::filesystem::path trigger = screenshot_path_ + ".go";
    if (!std::filesystem::exists(trigger, error)) return;
    std::filesystem::remove(trigger, error);
    WriteScreenshot(screenshot_path_);
  }

  // The frame as rendered to `path` (PNG), with the frame counter as the window shows it when Any360's switch (the
  // flag or its key) shows it: the presenter draws it on the swapchain image, not on this frame; a title's own
  // show_fps leaves its screenshots as they were.
  void WriteScreenshot(const std::string& path) {
    std::vector<uint32_t> pixels;
    bool ok = ReadFrame(pixels) && pixels.size() == size_t(frame().width) * frame().height;
    if (ok && composite_fps_.load(std::memory_order_relaxed) && show_fps_.load(std::memory_order_relaxed)) {
      gfx::CompositeOverlayRgba(overlay_text_, pixels.data(), frame().width, frame().height);
    }
    ok = ok && gfx::WritePng(path, frame().width, frame().height, pixels.data());
    if (log_) {
      log_(ok ? "Any360: screenshot written to " + path + " (" + std::to_string(frame().width) + "x" +
                    std::to_string(frame().height) + ")"
              : "Any360: no screenshot (the frame could not be read back)");
    }
  }

  // One frame of a burst: read back, halved (2x2 averaged), queued for the writer thread.
  void BurstFrame() {
    std::vector<uint32_t> pixels;
    const uint32_t w = frame().width, h = frame().height;
    if (!ReadFrame(pixels) || pixels.size() != size_t(w) * h || w < 2 || h < 2) return;
    BurstImage image;
    image.width = w / 2, image.height = h / 2;
    image.rgb.resize(size_t(image.width) * image.height * 3);
    for (uint32_t y = 0; y < image.height; ++y) {
      for (uint32_t x = 0; x < image.width; ++x) {
        const uint32_t* p = &pixels[size_t(2 * y) * w + 2 * x];
        const uint32_t q[4] = {p[0], p[1], p[w], p[w + 1]};
        for (uint32_t c = 0; c < 3; ++c) {
          uint32_t sum = 0;
          for (uint32_t v : q) sum += (v >> (8 * c)) & 0xFF;
          image.rgb[(size_t(y) * image.width + x) * 3 + c] = uint8_t(sum / 4);
        }
      }
    }
    char name[64];
    std::snprintf(name, sizeof(name), "/frame_%05u.ppm", burst_index_++);
    image.path = burst_dir_ + name;
    {
      std::lock_guard lock(burst_mutex_);
      if (burst_queue_.size() < 64) burst_queue_.push_back(std::move(image));
      else ++burst_dropped_;
    }
    burst_ready_.notify_one();
    if (!burst_writer_.joinable()) {
      burst_writer_ = std::thread([this] {
        for (;;) {
          BurstImage next;
          {
            std::unique_lock lock(burst_mutex_);
            burst_ready_.wait(lock, [this] { return burst_stop_ || !burst_queue_.empty(); });
            if (burst_queue_.empty()) return;
            next = std::move(burst_queue_.front());
            burst_queue_.pop_front();
          }
          if (std::FILE* file = std::fopen(next.path.c_str(), "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", next.width, next.height);
            std::fwrite(next.rgb.data(), 1, next.rgb.size(), file);
            std::fclose(file);
          }
        }
      });
    }
    if (--burst_left_ == 0 && log_) {
      log_("Any360: burst of " + std::to_string(burst_index_) + " frames recorded (" + std::to_string(burst_dropped_) +
           " dropped: the writer fell behind)");
    }
  }

  // The period's frame times (taken: the next period starts empty): the median, and the 10% and 1% lows (the frame
  // rates of the 90th and 99th percentile frame times: the stutter the averages hide).
  std::string TakeFrameTimes() {
    char text[160] = "frame times: none";
    if (const gfx::FrameTimeSummary f = gfx::SummarizeFrameTimes(intervals_); f.frames) {
      std::snprintf(text, sizeof(text), "frame times: median %.2f ms (%.1f FPS), 10%% low %.1f FPS, 1%% low %.1f FPS",
                    f.median_ms, 1e3 / f.median_ms, 1e3 / f.p90_ms, 1e3 / f.p99_ms);
    }
    intervals_.clear();
    return text;
  }

  // What a frame cost, from cumulative counters read at each frame: the command processor's busy time, texture
  // uploads and their loading, images created, staging buffers made, pipelines created. Kept for each period's slowest
  // frame, to tell a stall in the renderer from one in the title (the command processor then idle).
  struct FrameMarks {
    uint64_t cp_busy = 0, ns_load = 0, ns_create = 0, ns_staging = 0, staging_bytes = 0;
    uint32_t uploads = 0, images = 0, staging = 0, pipelines = 0;
    // any360::sys's side (any360/sys/activity.h): file reads, sleeping waits, XMA decoding.
    uint64_t file_reads = 0, file_bytes = 0, file_ns = 0, waits = 0, xma_ns = 0, memory_allocs = 0, memory_ns = 0;
  };
  FrameMarks Marks() {
    FrameMarks m;
    if (cp_) m.cp_busy = cp_->ns_busy();
    if (gfx::vk::TextureCache* t = texture_cache()) {
      m.ns_load = t->ns_load(), m.ns_create = t->ns_create(), m.ns_staging = t->ns_staging();
      m.staging_bytes = t->staging_created_bytes(), m.uploads = t->uploads(), m.images = t->images_created();
      m.staging = t->staging_created();
    }
    m.pipelines = stats().pipelines;
    const sys::Activity& a = sys::SysActivity();
    m.file_reads = a.file_reads.load(std::memory_order_relaxed), m.file_bytes = a.file_bytes.load(std::memory_order_relaxed);
    m.file_ns = a.file_ns.load(std::memory_order_relaxed), m.waits = a.waits_slept.load(std::memory_order_relaxed);
    m.xma_ns = a.xma_ns.load(std::memory_order_relaxed);
    m.memory_allocs = a.memory_allocs.load(std::memory_order_relaxed), m.memory_ns = a.memory_ns.load(std::memory_order_relaxed);
    return m;
  }
  std::string SlowestFrame() const {
    const FrameMarks& w = worst_cost_;
    char text[560];
    std::snprintf(text, sizeof(text),
                  "slowest frame %.1f ms (command processor busy %.1f ms; %u texture uploads, %.2f ms loading; %u images "
                  "created, %.2f ms; %u staging buffers, %.1f MB, %.2f ms; %u pipelines created; files: %llu reads, %.2f MB, "
                  "%.2f ms; %llu waits slept; XMA %.2f ms; %llu physical allocations, %.2f ms)",
                  worst_ms_, double(w.cp_busy) / 1e6, w.uploads, double(w.ns_load) / 1e6, w.images, double(w.ns_create) / 1e6,
                  w.staging, double(w.staging_bytes) / 1048576.0, double(w.ns_staging) / 1e6, w.pipelines,
                  static_cast<unsigned long long>(w.file_reads), double(w.file_bytes) / 1048576.0, double(w.file_ns) / 1e6,
                  static_cast<unsigned long long>(w.waits), double(w.xma_ns) / 1e6,
                  static_cast<unsigned long long>(w.memory_allocs), double(w.memory_ns) / 1e6);
    return text;
  }

  // Frames per second and the renderer's cost per frame since the last report.
  void Report() {
    if (hitch_) hitch_->Frame();
    ++frames_;
    const auto now = std::chrono::steady_clock::now();
    // The period's slowest frames (the averages hide drops).
    const FrameMarks marks = log_ ? Marks() : FrameMarks{};
    const auto previous = last_frame_;
    if (frames_ > 1) {
      const double interval = std::chrono::duration<double, std::milli>(now - last_frame_).count();
      if (log_ && interval > worst_ms_) {
        const FrameMarks& l = last_marks_;
        worst_cost_ = {marks.cp_busy - l.cp_busy,   marks.ns_load - l.ns_load,   marks.ns_create - l.ns_create,
                       marks.ns_staging - l.ns_staging, marks.staging_bytes - l.staging_bytes, marks.uploads - l.uploads,
                       marks.images - l.images,     marks.staging - l.staging,   marks.pipelines - l.pipelines,
                       marks.file_reads - l.file_reads, marks.file_bytes - l.file_bytes, marks.file_ns - l.file_ns,
                       marks.waits - l.waits,       marks.xma_ns - l.xma_ns,     marks.memory_allocs - l.memory_allocs,
                       marks.memory_ns - l.memory_ns};
      }
      worst_ms_ = std::max(worst_ms_, interval);
      if (log_) intervals_.push_back(float(interval));  // taken by each report
      slow_frames_ += interval > 1000.0 / 60.0;
      very_slow_frames_ += interval > 1000.0 / 20.0;
    }
    last_frame_ = now;
    last_marks_ = marks;
    if (show_fps_.load(std::memory_order_relaxed)) {
      Overlay(now, frames_ > 1 ? std::chrono::duration<float, std::milli>(now - previous).count() : -1.0f);
    } else if (overlay_frames_) {
      HideOverlay();  // hidden by its key
    }
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
      char off[1536];
      std::snprintf(off, sizeof(off),
                    "Any360: %.1f FPS with the diagnostics off (%.1f with them, the period before: %+.0f%%); %s; byte "
                    "swapping %.2f MB a frame, ~%.2f ms over plain copies: without it ~%.0f FPS (the command processor's "
                    "thread bound); slowest frame %.1f ms, %u over 16.7 ms, %u over 50 ms (%s); %.1f watched writes a frame (%.2f MB "
                    "invalidated); %.0f MB private, %zu textures, %zu targets, %zu resolves kept, %zu pipelines; "
                    "%llu frames presented, %llu not",
                    fps, last_on_fps_, last_on_fps_ > 0 ? 100.0 * (fps / last_on_fps_ - 1.0) : 0.0,
                    TakeFrameTimes().c_str(), swap_bytes / 1e6, swap_ms,
                    1e3 / std::max(0.1, frame_ms - swap_ms), worst_ms_, slow_frames_, very_slow_frames_, SlowestFrame().c_str(),
                    double(watch_writes - last_watch_writes_) / n, double(watch_bytes - last_watch_bytes_) / n / 1e6,
                    double(PrivateBytes()) / 1e6, texture_cache() ? texture_cache()->texture_count() : size_t(0),
                    surface_count(), resolved_count(), pipeline_count(),
                    (unsigned long long)(presenter_.presented() - last_presented_),
                    (unsigned long long)(present_failures_ - last_present_failures_));
      log_(off);
      last_presented_ = presenter_.presented(), last_present_failures_ = present_failures_;
      last_watch_writes_ = watch_writes, last_watch_bytes_ = watch_bytes;
      // What accumulated meanwhile is not reported: the next period starts clean.
      if (cp_) {
        last_busy_ = cp_->ns_busy(), last_waiting_ = cp_->ns_waiting(), last_kicks_ = cp_->kicks();
        last_direct_ = cp_->direct_kicks(), last_bins_ = cp_->direct_bins();
        last_brackets_ = TilingBrackets(true), last_runtime_brackets_ = TilingBrackets(false);
        last_runs_ = TileOpRuns(true), last_runtime_runs_ = TileOpRuns(false), last_ring_writes_ = cp_->ring_writes();
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
      last_shader_times_ = shader_times();  // the next report: its own period only (empty without ANY360_DRAW_TIMING)
      worst_ms_ = 0, slow_frames_ = very_slow_frames_ = 0, worst_cost_ = {};
      if (sampler_) sampler_->Write();  // the samples so far (the title may end without StopGraphics)
      gfx::g_diagnostics = true;
      return;
    }
    char line[768];
    std::snprintf(line, sizeof(line),
                  "Any360: %.1f FPS; per frame: %.0f draws, %.0f skipped, %.0f failed, %.0f resolves, %.1f texture uploads, "
                  "%.1f texture copies from resolves (%.1f sampled in place), %.1f translations, %.1f pipelines (%.1f draws waiting), %.1f flushes; ms: draws %.2f (textures %.2f, shaders %.2f, "
                  "pipelines %.2f, guest memory %.2f, submit %.2f), resolves %.2f, swaps %.2f; guest memory %s",
                  n / seconds, (s.draws - last_.draws) / n, (s.skipped - last_.skipped) / n, (s.failed - last_.failed) / n,
                  (s.resolves - last_.resolves) / n, (s.texture_uploads - last_.texture_uploads) / n,
                  (s.texture_gpu_copies - last_.texture_gpu_copies) / n, (s.texture_direct_views - last_.texture_direct_views) / n,
                  (s.translations - last_.translations) / n, (s.pipelines - last_.pipelines) / n,
                  (s.pipeline_waits - last_.pipeline_waits) / n,
                  (s.flushes - last_.flushes) / n, (s.ns_draws - last_.ns_draws) / ms,
                  (s.ns_textures - last_.ns_textures) / ms, (s.ns_shaders - last_.ns_shaders) / ms,
                  (s.ns_pipelines - last_.ns_pipelines) / ms, (s.ns_memory - last_.ns_memory) / ms,
                  (s.ns_submit - last_.ns_submit) / ms, (s.ns_resolves - last_.ns_resolves) / ms,
                  (s.ns_swaps - last_.ns_swaps) / ms, guest_memory_in_place() ? "in place" : "copied");
    log_(std::string(line) + "; " + TakeFrameTimes() + "; " + SlowestFrame());
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
    {
      // The GPU's time by kind of work (time stamps at each piece's start).
      auto kind = [&](int k) { return (s.ns_gpu_kind[k] - last_.ns_gpu_kind[k]) / ms; };
      char kinds[256];
      std::snprintf(kinds, sizeof(kinds),
                    "Any360: GPU ms per frame by kind: draws %.2f, resolves %.2f, MSAA resolves %.2f, depth resolves %.2f, "
                    "texture copies %.2f, write-backs %.2f, other %.2f",
                    kind(gfx::vk::kGpuDraws), kind(gfx::vk::kGpuResolves),
                    kind(gfx::vk::kGpuSampleResolves), kind(gfx::vk::kGpuDepthResolves),
                    kind(gfx::vk::kGpuTextureCopies), kind(gfx::vk::kGpuWriteBacks),
                    kind(gfx::vk::kGpuOther));
      log_(kinds);
    }
    if (std::map<uint64_t, ShaderTime> times = shader_times(); !times.empty()) {
      // ANY360_DRAW_TIMING: the draws' GPU time by pixel shader over the period, the costliest first.
      std::vector<std::pair<uint64_t, uint64_t>> by_time;  // ns this period, pixel shader hash
      uint64_t total = 0;
      for (const auto& [hash, st] : times) {
        const auto before = last_shader_times_.find(hash);
        const uint64_t ns = st.ns - (before == last_shader_times_.end() ? 0 : before->second.ns);
        if (ns) by_time.push_back({ns, hash}), total += ns;
      }
      std::sort(by_time.rbegin(), by_time.rend());
      std::string top = "Any360: GPU draw ms per frame by pixel shader (" + std::to_string(by_time.size()) + " shaders, " +
                        std::to_string(total / ms).substr(0, 5) + " ms):";
      for (size_t i = 0; i < std::min<size_t>(12, by_time.size()); ++i) {
        const uint64_t hash = by_time[i].second;
        const auto before = last_shader_times_.find(hash);
        const uint64_t draws = times[hash].draws - (before == last_shader_times_.end() ? 0 : before->second.draws);
        char entry[160];
        std::snprintf(entry, sizeof(entry), " ps %016llX (vs %016llX) %.2f ms, %.1f draws, %.1f us a draw;",
                      (unsigned long long)hash, (unsigned long long)times[hash].vertex_hash, by_time[i].first / ms,
                      draws / n, draws ? by_time[i].first / 1e3 / double(draws) : 0.0);
        top += entry;
      }
      log_(top);
      last_shader_times_ = std::move(times);
    }
    if (cp_) {
      const uint64_t busy = cp_->ns_busy(), waiting = cp_->ns_waiting(), kicks = cp_->kicks(), direct = cp_->direct_kicks();
      const uint64_t ring_writes = cp_->ring_writes();
      const uint64_t bins = cp_->direct_bins(), brackets = TilingBrackets(true), runtime_brackets = TilingBrackets(false);
      const uint64_t runs = TileOpRuns(true), runtime_runs = TileOpRuns(false);
      char line_cp[640];
      std::snprintf(line_cp, sizeof(line_cp),
                    "Any360: command processor per frame: %.2f ms running the ring (%.2f ms of it in WAIT_REG_MEM), "
                    "%.1f kicks (%.1f through the title's ring; %.1f command buffers and %.1f tile bin selects submitted "
                    "directly), %.1f tiled brackets (%.1f the runtime's), %.1f tile op runs (%.1f the runtime's); %.1f%% "
                    "of the time with nothing to run",
                    (busy - last_busy_) / ms, (waiting - last_waiting_) / ms, (kicks - last_kicks_) / n,
                    (ring_writes - last_ring_writes_) / n, (direct - last_direct_) / n, (bins - last_bins_) / n,
                    (brackets + runtime_brackets - last_brackets_ - last_runtime_brackets_) / n,
                    (runtime_brackets - last_runtime_brackets_) / n,
                    (runs + runtime_runs - last_runs_ - last_runtime_runs_) / n, (runtime_runs - last_runtime_runs_) / n,
                    100.0 * (1.0 - (busy - last_busy_) / 1e9 / seconds));
      log_(line_cp);
      last_busy_ = busy, last_waiting_ = waiting, last_kicks_ = kicks, last_direct_ = direct, last_bins_ = bins;
      last_brackets_ = brackets, last_runtime_brackets_ = runtime_brackets;
      last_runs_ = runs, last_runtime_runs_ = runtime_runs, last_ring_writes_ = ring_writes;
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
    const gfx::vk::GpuObjectCounts objects = gfx::vk::CreatedGpuObjects();
    char detail[640];
    std::snprintf(detail, sizeof(detail),
                  "Any360: per frame: resolves %.1f on the GPU, %.1f read back, %.1f depth, %.1f written back later "
                  "(%.3f ms; the first version %.3f ms), GPU objects created %.2f buffers %.2f images (%.2f MB), "
                  "%.2f superseded by the CPU; textures ms: hashing %.2f, "
                  "loading for uploads %.2f",
                  (s.resolves_on_gpu - last_.resolves_on_gpu) / n, (s.resolves_read_back - last_.resolves_read_back) / n,
                  (s.resolves_depth - last_.resolves_depth) / n, (s.resolve_write_backs - last_.resolve_write_backs) / n,
                  (s.ns_write_backs - last_.ns_write_backs) / 1e6 / n,
                  (s.ns_write_backs_reference - last_.ns_write_backs_reference) / 1e6 / n,
                  (objects.buffers - last_objects_.buffers) / n, (objects.images - last_objects_.images) / n,
                  (objects.bytes - last_objects_.bytes) / 1048576.0 / n,
                  (s.resolves_superseded - last_.resolves_superseded) / n,
                  (s.ns_texture_hash - last_.ns_texture_hash) / ms,
                  (s.ns_texture_load - last_.ns_texture_load) / ms);
    log_(detail);
    last_objects_ = objects;
    for (const std::string& line : TakeAuditLog()) log_("Any360: " + line);
    if (audit_hits_.load()) {
      log_("Any360: audited writes over compressed textures so far: " + std::to_string(audit_hits_.load()) + ", over one in use " +
           std::to_string(audit_hits_in_use_.load()));
    }
    if (gfx::vk::TextureCache* textures = texture_cache()) {
      // The textures uploaded most since the last report (by loading time): base, format, size, tiled or linear,
      // endianness and dimension, how many times, the loading time (all, the slowest), what last resolved there.
      std::vector<std::pair<gfx::vk::TextureCache::UploadStats, std::array<uint32_t, 5>>> top;
      for (const auto& [key, u] : textures->upload_log()) top.emplace_back(u, key);
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first.ns > b.first.ns; });
      std::string uploads = "Any360: uploaded most:";
      for (size_t i = 0; i < top.size() && i < 10; ++i) {
        const auto& [u, key] = top[i];
        const uint32_t resolved = ResolveFormatAt(key[0]);
        char item[200];
        std::snprintf(item, sizeof(item), " %08X fmt %u %ux%u %s e%u d%u x%u, %.2f ms (slowest %.2f)%s;", key[0], key[1],
                      key[2], key[3], (key[4] & 1) ? "tiled" : "linear", (key[4] >> 1) & 3, (key[4] >> 3) & 3, u.count,
                      double(u.ns) / 1e6, double(u.ns_max) / 1e6,
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
    worst_ms_ = 0, slow_frames_ = very_slow_frames_ = 0, worst_cost_ = {};
  }

  // The frame counter (--any360_fps_overlay, gfx/frame_counter.h), its text made every half second: frames per
  // second, the average and slowest frame time (ms) of that half second; the median and the 10% / 1% lows of the last
  // FrameTimeWindow::kWindow frames; the GPU's time per frame (the batches' timestamps) and the command processor's
  // idle share. `interval`: this frame's time (ms), < 0 for the first.
  void Overlay(std::chrono::steady_clock::time_point now, float interval) {
    if (interval >= 0) overlay_window_.Add(interval);
    if (!overlay_frames_++) {
      overlay_since_ = now;
      overlay_stats_ = stats();
      overlay_busy_ = cp_ ? cp_->ns_busy() : 0;
      overlay_worst_ = 0;
      return;
    }
    overlay_worst_ = std::max(overlay_worst_, double(interval));
    const double seconds = std::chrono::duration<double>(now - overlay_since_).count();
    if (seconds < 0.5) return;
    const double n = double(overlay_frames_ - 1);
    const gfx::vk::RendererStats& s = stats();
    const uint64_t busy = cp_ ? cp_->ns_busy() : 0;
    gfx::OverlayFigures figures;
    figures.fps = n / seconds, figures.frame_ms = 1e3 * seconds / n, figures.max_ms = overlay_worst_;
    figures.window = overlay_window_.Summary();
    figures.gpu_ms = double(s.ns_gpu - overlay_stats_.ns_gpu) / 1e6 / n;
    if (cp_) figures.cp_idle_pct = 100.0 * std::max(0.0, 1.0 - double(busy - overlay_busy_) / 1e9 / seconds);
    overlay_text_ = gfx::FormatOverlay(figures);
    presenter_.SetOverlay(overlay_text_);
    overlay_frames_ = 1;
    overlay_since_ = now;
    overlay_stats_ = s;
    overlay_busy_ = busy;
    overlay_worst_ = 0;
  }

  // The counter hidden (its key): nothing drawn, its window and text started again when it is shown.
  void HideOverlay() {
    presenter_.SetOverlay("");
    overlay_text_.clear();
    overlay_window_.Clear();
    overlay_frames_ = 0;
  }

  std::function<void(const std::string&)> log_;
  double stats_seconds_;
 public:
  std::string screenshot_path_;
  uint32_t screenshot_poll_ = 0;
  // A burst of frames (Screenshot): the frames left, the folder, and the writer thread with its queue.
  struct BurstImage {
    std::string path;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb;
  };
  uint32_t burst_left_ = 0, burst_index_ = 0, burst_dropped_ = 0;
  std::string burst_dir_;
  std::mutex burst_mutex_;
  std::condition_variable burst_ready_;
  std::deque<BurstImage> burst_queue_;
  bool burst_stop_ = false;
  std::thread burst_writer_;
  // The frame counter shown (the title's show_fps, --any360_fps_overlay, its key on the UI thread); Any360's switch
  // (the flag or the key), which puts it in screenshots too; the screenshot key pressed.
  std::atomic<bool> show_fps_{false}, composite_fps_{false}, screenshot_key_{false};
  std::unique_ptr<diagnostics::HitchTracer> hitch_;  // any360_hitch_trace_ms
  void StartHitchTracer(int threshold_ms, const std::string& folder) {
    hitch_ = std::make_unique<diagnostics::HitchTracer>(double(threshold_ms), folder, 5, log_);
  }
 private:
  uint64_t overlay_frames_ = 0, overlay_busy_ = 0;
  double overlay_worst_ = 0;
  std::chrono::steady_clock::time_point overlay_since_;
  gfx::vk::RendererStats overlay_stats_;
  gfx::FrameTimeWindow overlay_window_;
  std::string overlay_text_;  // the counter's text, for screenshots
 public:
  gfx::CommandProcessor* cp_ = nullptr;  // for the report: where the command processor's time goes
 private:
  uint64_t last_busy_ = 0, last_waiting_ = 0, last_kicks_ = 0, last_direct_ = 0, last_bins_ = 0, last_brackets_ = 0, last_runtime_brackets_ = 0, last_runs_ = 0, last_runtime_runs_ = 0, last_ring_writes_ = 0;
  uint64_t last_tsc_ = 0;
  uint64_t last_swapped_[5] = {};
  double last_on_fps_ = 0;
 public:
  SwapCost swap_cost_;
  Sampler* sampler_ = nullptr;
 private:
  std::array<uint64_t, 129> last_opcode_ticks_{};
  gfx::vk::GpuObjectCounts last_objects_{};
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
  FrameMarks last_marks_, worst_cost_;  // the last frame's counters; the cost of the period's slowest frame
  std::vector<float> intervals_;  // the period's frame times (ms)
  uint32_t slow_frames_ = 0, very_slow_frames_ = 0;
  std::map<uint64_t, ShaderTime> last_shader_times_;  // ANY360_DRAW_TIMING: shader_times() at the last report
  gfx::vk::RendererStats last_;

  gfx::vk::Presenter presenter_;
  Window window_;
  rex::ui::Window* title_;
  uint32_t width_ = 0, height_ = 0;
};

#ifndef _WIN32
// The title's window: the runtime's largest visible SDL window (the one it presents to).
SDL_Window* FindTitleWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* best = nullptr;
  int64_t area = 0;
  for (int i = 0; i < count; ++i) {
    int w = 0, h = 0;
    if (!(SDL_GetWindowFlags(windows[i]) & SDL_WINDOW_HIDDEN) && SDL_GetWindowSizeInPixels(windows[i], &w, &h) &&
        int64_t(w) * h > area) {
      best = windows[i];
      area = int64_t(w) * h;
    }
  }
  SDL_free(windows);
  return best;
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
  Window window = nullptr;  // the title's (the runtime's window: nothing of ours to destroy)
  gfx::vk::Device device;
  std::unique_ptr<PresentingRenderer> renderer;
  std::unique_ptr<gfx::CommandProcessor> cp;
  std::unique_ptr<gfx::AsyncRenderer> async;
  InterruptThread interrupts;
  std::unique_ptr<Sampler> sampler;
  // A frame capture: requested by a trigger file, opened on the command processor's thread before its next kick
  // (RequestCapture: a title may never leave it idle at a kick), closed on the title's thread once it has recorded
  // its kicks.
  uint8_t* physical = nullptr;
  std::string capture_path;
  uint32_t capture_kicks = 0;
  std::mutex capture_mutex;  // the writer, set on the command processor's thread
  bool capture_requested = false;
  std::atomic<bool> capture_key{false};  // the capture key pressed (hotkeys.h): no trigger file needed
  std::unique_ptr<gfx::capture::Writer> writer;
  std::chrono::steady_clock::time_point next_trigger_check{};
  std::function<void(const std::string&)> log;
};

// Any360's keys (hotkeys.h), with the runtime's key binds: their callbacks run on the UI thread and only set flags the
// renderer's and the title's threads pick up, except the profiler, which runs on a thread of its own.
void RegisterHotkeys(Binding& b) {
  PresentingRenderer* r = b.renderer.get();
  const auto log = b.log;
  rex::ui::RegisterBind(kFrameCounterKey.bind, kFrameCounterKey.default_key, kFrameCounterKey.description, [r, log] {
    const bool show = !r->show_fps_.load();
    r->composite_fps_ = show;
    r->show_fps_ = show;
    if (log) log(show ? "Any360: frame counter shown (its key)" : "Any360: frame counter hidden (its key)");
  });
  rex::ui::RegisterBind(kScreenshotKey.bind, kScreenshotKey.default_key, kScreenshotKey.description,
                        [r] { r->screenshot_key_ = true; });
  rex::ui::RegisterBind(kProfileKey.bind, kProfileKey.default_key, kProfileKey.description, [log] {
    static std::unique_ptr<diagnostics::Profiler> profiler;  // a new press starts again (the last one stopped)
    const int seconds = ProfileSeconds() > 0 ? ProfileSeconds() : 10;
    profiler.reset();
    profiler = std::make_unique<diagnostics::Profiler>(0.0, double(seconds), HitchTraceFolder(), log);
    if (log) log("Any360: profiling every thread for " + std::to_string(seconds) + " s (its key)");
  });
  rex::ui::RegisterBind(kGpuCaptureKey.bind, kGpuCaptureKey.default_key, kGpuCaptureKey.description,
                        [&b] { b.capture_key = true; });
}

std::atomic<bool> g_bound{false};
std::atomic<double> g_vblank_hz{60.0};  // the rate StartGraphics chose (SetVblankHz(0) goes back to it)
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
  renderer->CpuWrote(start, start + length);  // resolves kept on the GPU there are superseded
  return {start, length};
}

uint32_t ReadGpuRegister(void*, void*, uint32_t address) { return sys::Graphics().ReadRegister(sys::GpuRegisterIndex(address)); }

// At a kick (the title's write of CP_RB_WPTR, before the command processor sees it): starts a requested capture
// while the command processor is idle, or closes one whose kicks the command processor has run (it lets go of the
// writer itself: a title may never leave it idle at a kick).
void CaptureAtKick(Binding& b) {
  if (b.capture_path.empty() && !b.capture_key.load(std::memory_order_relaxed)) return;
  {
    std::lock_guard lock(b.capture_mutex);
    if (b.writer) {
      if (b.cp->CaptureDone()) {
        const uint64_t bytes = b.writer->bytes_written();
        b.writer.reset();
        b.capture_requested = false;
        if (b.log) b.log("Any360: capture written to " + b.capture_path + " (" + std::to_string(bytes >> 20) + " MB)");
      }
      return;
    }
    if (b.capture_requested) return;
  }
  if (b.capture_key.exchange(false, std::memory_order_relaxed)) {
    // The key: to any360_gpu_capture when it is set, else to the next <user data>/captures/any360_<n>.a360cap.
    if (b.capture_path.empty()) {
      b.capture_path = NextNumberedPath(TitleDataFolder(UserDataRoot(), "captures"), "any360_", ".a360cap").string();
    }
  } else {
    if (b.capture_path.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < b.next_trigger_check) return;
    b.next_trigger_check = now + std::chrono::milliseconds(500);
    const std::string trigger = b.capture_path + ".go";
    if (std::FILE* f = std::fopen(trigger.c_str(), "rb")) {
      std::fclose(f);
    } else {
      return;
    }
    std::remove(trigger.c_str());
  }
  {
    std::lock_guard lock(b.capture_mutex);
    b.capture_requested = true;
  }
  // Opened by the command processor before the next kick it runs, every kick before done.
  b.cp->RequestCapture([&b] {
    auto writer = std::make_unique<gfx::capture::Writer>();
    if (!writer->Open(b.capture_path, b.physical, kPhysicalBytes, b.cp->registers(), gfx::reg::kRegisterCount,
                      "any360::host::rexglue binding")) {
      if (b.log) b.log("Any360: cannot open the capture " + b.capture_path);
      std::lock_guard lock(b.capture_mutex);
      b.capture_requested = false;
      return;
    }
    writer->Shaders(b.cp->shader(0).microcode, b.cp->shader(1).microcode);
    std::lock_guard lock(b.capture_mutex);
    b.writer = std::move(writer);
    b.cp->SetCapture(b.writer.get(), b.capture_kicks);
    if (b.log) b.log("Any360: capturing " + std::to_string(b.capture_kicks) + " kicks into " + b.capture_path);
  });
}

void WriteGpuRegister(void*, void*, uint32_t address, uint32_t value) {
  const uint32_t index = sys::GpuRegisterIndex(address);
  if (index == gfx::reg::CP_RB_WPTR && g_binding) CaptureAtKick(*g_binding);
  sys::Graphics().WriteRegister(index, value);
}

}  // namespace

bool SubmitIndirect(uint32_t position, uint32_t address, uint32_t dwords) {
  if (g_binding) CaptureAtKick(*g_binding);
  return sys::Graphics().SubmitIndirect(position, address, dwords);
}

bool SubmitBins(uint32_t position, uint32_t which, uint32_t value) {
  return sys::Graphics().SubmitBins(position, which, value);
}

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
  // The title's own window, the runtime's display window, on every system: the frames go there from our first one on
  // (see PresentingRenderer::TakeWindow); until then the runtime draws its own (its launcher). Only the surface and the
  // size are read per system.
  rex::Runtime* const runtime_for_window = rex::Runtime::instance();
  rex::ui::Window* const title_window = runtime_for_window ? runtime_for_window->display_window() : nullptr;
  if (!title_window) {
    // Tool mode (tests, tools): no window, frames rendered and not presented.
    if (options.log) options.log("Any360: the runtime has no display window; frames are rendered, not presented");
  } else {
#ifdef _WIN32
    const HWND window = static_cast<HWND>(title_window->GetNativeWindowHandle());
    if (!window || !IsWindow(window)) {
      error = "window: the runtime's display window has no Win32 handle";
      return false;
    }
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
    // The runtime's SDL (exported by its library) gives the window's SDL handle and the surface: X11 or Wayland.
    SDL_Window* const window = FindTitleWindow();
    if (!window) {
      error = "window: the runtime has no visible SDL window to draw in";
      return false;
    }
    Uint32 extension_count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!extensions) {
      error = std::string("window: SDL_Vulkan_GetInstanceExtensions failed (") + SDL_GetError() + ")";
      return false;
    }
    device_options.instance_extensions.assign(extensions, extensions + extension_count);
    device_options.make_surface = [window](VkInstance instance) {
      // SDL calls the same loader (libvulkan.so.1) any360::gfx loaded: the instance is valid for it.
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) return VkSurfaceKHR(VK_NULL_HANDLE);
      return surface;
    };
#endif
    binding->window = window;
  }
  if (!binding->device.Create(device_options)) {
    error = "Vulkan: " + binding->device.error();
    return false;
  }
  if (options.log) options.log(std::string("Any360: Vulkan device ") + binding->device.properties().deviceName);

  // The renderer over the guest's physical memory, the command processor in front of it.
  uint8_t* physical = memory->TranslatePhysical<uint8_t*>(0);
  binding->physical = physical;
  binding->capture_path = options.capture_path;
  binding->capture_kicks = std::max(1u, options.capture_kicks);
  binding->log = options.log;
  binding->renderer = std::make_unique<PresentingRenderer>(binding->device, physical, binding->window, title_window,
                                                          options.log, options.stats_seconds, options.resolution_scale);
  binding->renderer->SetPipelineCache(options.pipeline_cache_path);
  binding->renderer->SetScreenshotPath(options.screenshot_path);
  // The frame counter: the title's own setting (GraphicsOptions::show_fps) or --any360_fps_overlay, for every title.
  binding->renderer->composite_fps_ = FpsOverlay();
  binding->renderer->show_fps_ = options.show_fps || binding->renderer->composite_fps_.load();
  if (binding->renderer->show_fps_.load() && options.log) options.log("Any360: frame counter drawn over the picture");
  if (const int hitch_ms = HitchTraceMs(); hitch_ms > 0) {
    binding->renderer->StartHitchTracer(hitch_ms, HitchTraceFolder());
  }
  if (const int seconds = ProfileSeconds(); seconds > 0) {
    static std::unique_ptr<diagnostics::Profiler> profiler;  // for the process's life
    profiler = std::make_unique<diagnostics::Profiler>(double(ProfileAfterSeconds()), double(seconds), HitchTraceFolder(),
                                                       options.log);
  }
  binding->renderer->SetAsyncPipelines(options.async_pipelines);
  binding->renderer->SetMsaa(options.msaa_samples);
  binding->renderer->SetOcclusionCounts(options.occlusion_counts);
  if (!binding->renderer->Initialize() || !binding->renderer->Resize()) {
    error = "renderer: " + binding->renderer->last_error();
    binding->renderer.reset();
    return false;
  }
  if (options.log && binding->renderer->recipes_loaded()) {
    options.log("Any360: " + std::to_string(binding->renderer->recipes_loaded()) +
                " pipeline recipes of earlier runs, compiled for this run's settings before the title draws with them");
  }
  // Textures are checked again only after the CPU writes their memory, as the runtime's own GPU does.
  if (gfx::vk::TextureCache* textures = binding->renderer->texture_cache(); textures && options.watch_memory) {
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
  binding->renderer->SetLateResolveReads(options.late_resolve_reads);
  // What the command processor writes into guest memory itself (fences, query counts), the page watch does not see:
  // the vertices copied into video memory over it are stale (textures are not kept over such words).
  binding->cp->SetMemoryWrittenHook([renderer = binding->renderer.get()](uint32_t begin, uint32_t end) {
    renderer->GuestMemoryWritten(begin, end);
    renderer->AuditWrite("command processor", begin, end);
  });
  if (options.log) binding->renderer->SetStallLog(options.log);
  // Diagnostics: ANY360_GPU_AUDIT_WRITES=1 notes GPU-side writes that land on compressed textures (frame report).
  if (const char* audit = std::getenv("ANY360_GPU_AUDIT_WRITES"); audit && *audit == '1') {
    binding->renderer->SetAuditWrites(true);
    if (options.log) options.log("Any360: GPU-side writes into guest memory audited");
  }
  // Diagnostics: ANY360_LOG_ALLOCATIONS=1 logs every GPU buffer and image created, with its caller.
  if (const char* allocations = std::getenv("ANY360_LOG_ALLOCATIONS"); allocations && *allocations == '1' && options.log) {
    gfx::vk::SetAllocationLog(options.log);
  }
  sys::Graphics().StartVblank(vblank_hz);
  g_vblank_hz = vblank_hz;
  g_binding = binding.release();
  g_bound = true;
  RegisterHotkeys(*g_binding);
  {
    std::lock_guard lock(g_glyph_mutex);
    g_glyphs_dir = options.glyphs_dir.empty() ? (ExecutableFolder() / "glyphs").string() : options.glyphs_dir;
    g_glyph_log = options.log;
    g_glyph_pages = std::make_unique<gfx::GlyphPages>(g_glyphs_dir, options.glyph_scale);
    if (g_glyph_pending_set) g_glyph_pages->SetPages(std::move(g_glyph_pending));
    g_glyph_pending_set = false;
  }
  g_glyph_thread = std::thread([] {
    std::unique_lock lock(g_glyph_mutex);
    while (!g_glyph_stop) {
      // A wake-up missed (the flag set between the check and the wait) costs at most the timeout.
      g_glyph_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return g_glyph_swapped.load() || g_glyph_stop; });
      g_glyph_swapped.store(false);
      if (!g_glyph_stop) UpdateGlyphPages();
    }
  });
  return true;
}

void SetGlyphPages(std::vector<gfx::GlyphPage> pages) {
  std::lock_guard lock(g_glyph_mutex);
  if (!g_glyph_pages) {
    g_glyph_pending = std::move(pages);
    g_glyph_pending_set = true;
    return;
  }
  g_glyph_pages->SetPages(std::move(pages));
}

void StopGraphics() {
  if (!g_binding) return;
  {
    std::lock_guard lock(g_glyph_mutex);
    g_glyph_stop = true;
  }
  g_glyph_wake.notify_all();
  if (g_glyph_thread.joinable()) g_glyph_thread.join();
  for (const Hotkey& key : kHotkeys) rex::ui::UnregisterBind(key.bind);
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

void SetVblankHz(double hz) {
  if (!g_bound) return;
  sys::Graphics().SetVblankHz(hz > 0.0 ? hz : g_vblank_hz.load());
}

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

void WatchReferenceCapture(const std::string& trace_path, uint32_t frames, std::function<void(const std::string&)> log) {
  if (trace_path.empty()) return;
  std::thread([trace_path, frames, log = std::move(log)] {
    const std::filesystem::path trigger = trace_path + ".go";
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      std::error_code error;
      if (!std::filesystem::exists(trigger, error)) continue;
      std::filesystem::remove(trigger, error);
#ifdef _WIN32
      // The runtime's capture reads every page of physical memory, and a page only reserved on the host (a title
      // reserves more than it commits) made it crash: those are committed first (they read as zero). Committed
      // pages are left as they are, with their protection (the runtime's write watches).
      if (rex::Runtime* runtime = rex::Runtime::instance(); runtime && runtime->memory()) {
        uint8_t* at = runtime->memory()->TranslatePhysical<uint8_t*>(0);
        uint8_t* const end = at + (size_t(512) << 20);
        uint64_t committed = 0;
        while (at < end) {
          MEMORY_BASIC_INFORMATION info{};
          if (!VirtualQuery(at, &info, sizeof info)) break;
          uint8_t* const region_end = std::min(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
          if (info.State == MEM_RESERVE && VirtualAlloc(at, size_t(region_end - at), MEM_COMMIT, PAGE_READWRITE)) {
            committed += uint64_t(region_end - at);
          }
          at = region_end;
        }
        if (log && committed) log("Any360: " + std::to_string(committed >> 20) + " MB of reserved physical memory committed for the capture");
        // What the host still cannot read (the capture would crash on it): said, and no capture then.
        std::string unreadable;
        uint64_t unreadable_bytes = 0;
        for (at = runtime->memory()->TranslatePhysical<uint8_t*>(0); at < end;) {
          MEMORY_BASIC_INFORMATION info{};
          if (!VirtualQuery(at, &info, sizeof info)) break;
          uint8_t* const region_end = std::min(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
          const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
          if (!readable) {
            unreadable_bytes += uint64_t(region_end - at);
            if (unreadable.size() < 600) {
              char text[96];
              std::snprintf(text, sizeof text, " %08llX+%llX state %lX protect %lX;",
                            (unsigned long long)(at - runtime->memory()->TranslatePhysical<uint8_t*>(0)),
                            (unsigned long long)(region_end - at), info.State, info.Protect);
              unreadable += text;
            }
          }
          at = region_end;
        }
        if (unreadable_bytes) {
          if (log) log("Any360: no capture, " + std::to_string(unreadable_bytes >> 10) + " KB of physical memory unreadable:" + unreadable);
          continue;
        }
      }
#endif
      const bool ok = rex::cvar::SetFlagByName("gpu_any360_capture", "true") &&
                      rex::cvar::SetFlagByName("gpu_draw_trace_frames", std::to_string(std::max(1u, frames))) &&
                      rex::cvar::SetFlagByName("gpu_draw_trace", trace_path);
      if (log) {
        log(ok ? "Any360: the runtime's GPU records " + std::to_string(frames) + " frame(s) to " + trace_path + ".a360cap"
               : "Any360: the runtime's GPU has no draw trace / Any360 capture settings");
      }
    }
  }).detach();
}

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
