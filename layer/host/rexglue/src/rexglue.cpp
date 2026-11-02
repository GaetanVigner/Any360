// any360::host::rexglue: see the header. The graphics binding inside a ReXGlue title: any360::sys's GraphicsKernel and
// any360::gfx behind the title's Vd* calls and GPU MMIO, interrupts run on a runtime host thread, frames presented
// into a window of the binding's own (a plain Win32 window for now: the runtime has its own SDL inside its library,
// and a second copy in the title would be a second SDL in the process; its messages are pumped by the runtime's UI
// thread, which creates it).
#include "any360/host/rexglue.h"

#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
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

// The renderer that presents each frame it swaps, on the command processor's thread (the thread that renders).
class PresentingRenderer : public gfx::vk::VulkanRenderer {
 public:
  // `parent`: the title's window `window` is a child of, kept covering its client area (null: none).
  PresentingRenderer(gfx::vk::Device& device, uint8_t* physical, Window window, Window parent,
                     std::function<void(const std::string&)> log, double stats_seconds)
      : VulkanRenderer(device, physical, kPhysicalBytes), log_(std::move(log)), stats_seconds_(stats_seconds),
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
    char line[768];
    std::snprintf(line, sizeof(line),
                  "Any360: %.1f FPS; per frame: %.0f draws, %.0f skipped, %.0f failed, %.0f resolves, %.1f texture uploads, "
                  "%.1f texture copies from resolves, %.1f translations, %.1f pipelines, %.1f flushes; ms: draws %.2f (textures %.2f, shaders %.2f, "
                  "pipelines %.2f, guest memory %.2f, submit %.2f), resolves %.2f, swaps %.2f; guest memory %s",
                  n / seconds, (s.draws - last_.draws) / n, (s.skipped - last_.skipped) / n, (s.failed - last_.failed) / n,
                  (s.resolves - last_.resolves) / n, (s.texture_uploads - last_.texture_uploads) / n,
                  (s.texture_gpu_copies - last_.texture_gpu_copies) / n,
                  (s.translations - last_.translations) / n, (s.pipelines - last_.pipelines) / n,
                  (s.flushes - last_.flushes) / n, (s.ns_draws - last_.ns_draws) / ms,
                  (s.ns_textures - last_.ns_textures) / ms, (s.ns_shaders - last_.ns_shaders) / ms,
                  (s.ns_pipelines - last_.ns_pipelines) / ms, (s.ns_memory - last_.ns_memory) / ms,
                  (s.ns_submit - last_.ns_submit) / ms, (s.ns_resolves - last_.ns_resolves) / ms,
                  (s.ns_swaps - last_.ns_swaps) / ms, guest_memory_in_place() ? "in place" : "copied");
    log_(line);
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
    frames_ = 1;
    since_ = now;
    last_ = s;
  }

  std::function<void(const std::string&)> log_;
  double stats_seconds_;
  uint64_t frames_ = 0;
  std::chrono::steady_clock::time_point since_;
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
  InterruptThread interrupts;
};

std::atomic<bool> g_bound{false};
Binding* g_binding = nullptr;

uint32_t ReadGpuRegister(void*, void*, uint32_t address) { return sys::Graphics().ReadRegister(sys::GpuRegisterIndex(address)); }
void WriteGpuRegister(void*, void*, uint32_t address, uint32_t value) {
  sys::Graphics().WriteRegister(sys::GpuRegisterIndex(address), value);
}

}  // namespace

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
  binding->renderer = std::make_unique<PresentingRenderer>(binding->device, physical, binding->window, binding->parent,
                                                          options.log, options.stats_seconds);
  if (!binding->renderer->Initialize() || !binding->renderer->Resize()) {
    error = "renderer: " + binding->renderer->last_error();
    binding->renderer.reset();
    DestroyWindow(binding->window);
    return false;
  }
  binding->cp = std::make_unique<gfx::CommandProcessor>(physical);
  binding->cp->SetRenderer(binding->renderer.get());
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
  sys::Graphics().StartVblank(options.vblank_hz);
  g_binding = binding.release();
  g_bound = true;
  return true;
}

void StopGraphics() {
  if (!g_binding) return;
  sys::Graphics().StopVblank();
  g_binding->interrupts.Stop();
  g_binding->cp->Stop();
  // The window, device and renderer stay until the process ends: the runtime may still be tearing down around them.
}

bool GraphicsBound() { return g_bound; }

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
  reference(ctx, base);
}

}  // namespace any360::host::rexglue
