// A window showing any360::gfx end to end, every frame: PM4 written by hand (no game data) runs through the command
// processor into the Vulkan renderer: a textured quad (a 32x32 tiled texture in guest memory) and a triangle moving
// over it, drawn into a 1280x720 EDRAM surface, resolved to guest memory, swapped as VdSwap would, and presented in
// an SDL3 window through the Presenter.
//
//   gfx_window_demo [--frames N] [--draws N] [--gpu NAME]
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/presenter.h"
#include "any360/gfx/vulkan/renderer.h"
#include "ucode_encode.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
// The native window handle on Windows (Linux: X11 / Wayland handles, next).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>

namespace {

using namespace any360::gfx;
using pm4::Type0;
using pm4::Type3;
namespace t = any360::gfx::test;
namespace uc = rex::graphics::ucode;

constexpr uint32_t kMemory = 8 << 20, kRing = 0x10000, kQuad = 0x40000, kTriangle = 0x500000, kRect = 0x42000,
                   kTexture = 0x50000, kFrontBuffer = 0x100000, kWidth = 1280, kHeight = 720;
// Guest memory: page-aligned so the renderer imports it in place.
uint8_t* const g_memory = [] {
  auto* p = static_cast<uint8_t*>(::operator new(kMemory, std::align_val_t(65536)));
  std::memset(p, 0, kMemory);
  return p;
}();

void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_memory + address, &value, 4);
}
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | b << 16 | g << 8 | r; }

struct Ring {
  CommandProcessor& cp;
  uint32_t write = 0;
  void Put(uint32_t dword) { SetBig(kRing + 4 * write++, dword); }
  void Put(std::initializer_list<uint32_t> dwords) {
    for (uint32_t d : dwords) Put(d);
  }
  void Reg(uint32_t index, uint32_t value) { Put({Type0(index, 1), value}); }
  void Shader(uint32_t type, const std::vector<uint32_t>& words) {
    Put({Type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(words.size())), type, uint32_t(words.size())});
    for (uint32_t w : words) Put(w);
  }
};

// One frame's PM4: state, the textured quad, the moving triangle, the resolve, the swap.
void WriteFrame(Ring& r, float time, uint32_t count) {
  // Surfaces: pitch 1280, colour 0 at tile 0 (k_8_8_8_8); viewport over 1280x720 (y up, W as is); scissors.
  r.Put({Type0(0x2000, 3), kWidth, 0, 0});
  r.Reg(0x2208, 4);
  r.Reg(0x2206, 0x3F | 1u << 10);
  r.Put({Type0(0x210F, 6), Bits(kWidth / 2.0f), Bits(kWidth / 2.0f), Bits(-float(kHeight) / 2.0f), Bits(kHeight / 2.0f), Bits(1), Bits(0)});
  r.Put({Type0(0x200E, 2), 0, kHeight << 16 | kWidth});
  r.Put({Type0(0x2080, 3), 0, 0, kHeight << 16 | kWidth});
  r.Put({Type0(0x2200, 2), 0, 0x00010001});  // no depth; no blending
  r.Put({Type0(0x2209, 3), 0x00010001, 0x00010001, 0x00010001});
  r.Reg(0x2104, 0xF);
  r.Reg(0x2205, 0);
  // The textured quad: tf1 = the 32x32 tiled k_8_8_8_8 texture, linear filtering.
  r.Shader(0, t::BuildTexturedVertexShader());
  r.Shader(1, t::BuildTexturedPixelShader(1));
  r.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, kQuad | 3, 2u | 24u << 2});
  r.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 1u << 22 | 1u << 31, 6u | 2u << 6 | kTexture, 31u | 31u << 13,
         0x688u << 1 | 1u << 19 | 1u << 21, 0, 1u << 9});
  r.Put({Type3(pm4::DRAW_INDX_2, 1), 4u << 16 | 2u << 6 | 6});
  // Triangles moving in circles, each in a colour that cycles: one by default, `count` to measure the cost per draw
  // at a game's scale (each its own vertex buffer and constants, as a game's draws have).
  r.Shader(0, t::BuildFetchPositionShader());
  r.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
  for (uint32_t n = 0; n < count; ++n) {
    const float phase = time + 6.2832f * float(n) / float(count), radius = count > 1 ? 0.2f + 0.6f * float(n % 7) / 7 : 0.5f;
    const float size = count > 1 ? 0.05f : 0.2f;
    const float cx = radius * std::cos(phase), cy = radius * std::sin(phase);
    const float tri[3][4] = {{cx - size, cy - size, 0, 1}, {cx + size, cy - size, 0, 1}, {cx, cy + 1.25f * size, 0, 1}};
    const uint32_t at = kTriangle + 48 * n;
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t c = 0; c < 4; ++c) SetBig(at + 16 * i + 4 * c, Bits(tri[i][c]));
    }
    r.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, at | 3, 2u | 12u << 2});
    r.Put({Type3(pm4::SET_CONSTANT, 5), 0u << 16 | 1024, Bits(0.5f + 0.5f * std::sin(phase * 1.3f)),
           Bits(0.5f + 0.5f * std::sin(phase * 1.7f + 2)), Bits(0.5f + 0.5f * std::sin(phase * 2.3f + 4)), Bits(1)});
    r.Put({Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 4});
  }
  // Resolve colour 0 to the front buffer (k_8_8_8_8, 8-in-32, tiled, pitch 1280), no clear.
  r.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, kRect | 3, 2u | 6u << 2});
  r.Put({Type0(0x2318, 4), 0, kFrontBuffer, kWidth | kHeight << 16, 2u | 6u << 7});
  r.Reg(0x2208, 6);
  r.Put({Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 8});
  r.Reg(0x2208, 4);
  // The swap, as VdSwap writes it: the front buffer's fetch constant 0, then XE_SWAP.
  r.Put({Type0(0x4800, 6), 2u | 2u << 10 | 2u << 13 | (kWidth / 32) << 22 | 1u << 31, 6u | 2u << 6 | kFrontBuffer,
         (kWidth - 1) | (kHeight - 1) << 13, 0x688u << 1, 0, 1u << 9});
  r.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, kFrontBuffer, kWidth, kHeight});
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t frames_wanted = 0;
  uint32_t triangles = 1;
  vk::DeviceOptions options;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames")) frames_wanted = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "--draws")) triangles = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    else if (!std::strcmp(argv[i], "--gpu")) options.prefer = argv[++i];
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("gfx_window_demo: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  // A plain window: the surface is made from its native handle (SDL builds without its own Vulkan support work too).
  SDL_Window* window = SDL_CreateWindow("Any360 - any360::gfx", int(kWidth), int(kHeight), SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::printf("gfx_window_demo: no window: %s\n", SDL_GetError());
    return 1;
  }
  options.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  options.make_surface = [window](VkInstance instance) {
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
  vk::Device device;
  if (!device.Create(options)) {
    std::printf("gfx_window_demo: no device: %s\n", device.error().c_str());
    return 1;
  }
  vk::VulkanRenderer renderer(device, g_memory, kMemory);
  if (!renderer.Initialize()) {
    std::printf("gfx_window_demo: renderer: %s\n", renderer.last_error().c_str());
    return 1;
  }
  vk::Presenter presenter(device);
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
  if (!presenter.Resize(uint32_t(pw), uint32_t(ph))) {
    std::printf("gfx_window_demo: swapchain: %s\n", presenter.error().c_str());
    return 1;
  }

  // Guest memory: the quad (position, texcoord), the resolve rectangle, the 32x32 texture of four quadrants.
  const float quad[4][6] = {{-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}};
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t c = 0; c < 6; ++c) SetBig(kQuad + 24 * i + 4 * c, Bits(quad[i][c]));
  }
  const float rect[6] = {0, 0, float(kWidth), 0, float(kWidth), float(kHeight)};
  for (uint32_t i = 0; i < 6; ++i) SetBig(kRect + 4 * i, Bits(rect[i]));
  const uint32_t colors[4] = {Rgba(200, 60, 60), Rgba(60, 160, 80), Rgba(60, 90, 200), Rgba(230, 230, 230)};
  for (uint32_t y = 0; y < 32; ++y) {
    for (uint32_t x = 0; x < 32; ++x) {
      SetBig(kTexture + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2)), colors[(y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0)]);
    }
  }

  CommandProcessor cp(g_memory);
  cp.SetRenderer(&renderer);
  const auto start = std::chrono::steady_clock::now();
  auto window_start = start;
  uint64_t window_frames = 0;
  bool running = true, resize = false;
  std::string failure;
  while (running && (!frames_wanted || presenter.presented() < frames_wanted)) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) running = false;
      if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) resize = true;
    }
    if (resize) {
      SDL_GetWindowSizeInPixels(window, &pw, &ph);
      presenter.Resize(uint32_t(pw), uint32_t(ph));
      resize = false;
    }
    const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    cp.InitializeRingBuffer({}, kRing, 15);  // 256 KB
    Ring ring{cp};
    WriteFrame(ring, time, triangles);
    cp.WriteRegister(reg::CP_RB_WPTR, ring.write);
    cp.Execute();
    if (renderer.stats().failed) {
      failure = "renderer: " + renderer.last_error();
      break;
    }
    const vk::VulkanRenderer::Frame& frame = renderer.frame();
    const bool shown = frame.on_gpu ? presenter.Present([&](VkCommandBuffer cb, VkImage target, VkOffset3D a, VkOffset3D b) {
                                        renderer.RecordFrameBlit(cb, target, a, b);
                                      }, frame.width, frame.height)
                                    : presenter.Present(frame.pixels.data(), frame.width, frame.height);
    if (!shown) {
      if (presenter.error().find("out of date") != std::string::npos) {
        resize = true;
        continue;
      }
      failure = "presenter: " + presenter.error();
      break;
    }
    ++window_frames;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - window_start).count();
    if (seconds >= 1.0) {
      const std::string title = "Any360 - any360::gfx - " + std::to_string(int(window_frames / seconds)) + " FPS - " +
                                device.properties().deviceName;
      SDL_SetWindowTitle(window, title.c_str());
      window_start = std::chrono::steady_clock::now();
      window_frames = 0;
    }
  }
  const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("gfx_window_demo: %s, %llu frames presented in %.2f s (%.0f FPS), %u draws, %u resolves, surface format %d%s%s\n",
              device.properties().deviceName, (unsigned long long)presenter.presented(), total, presenter.presented() / total,
              renderer.stats().draws, renderer.stats().resolves, int(presenter.format()), failure.empty() ? "" : ", stopped: ",
              failure.c_str());
  const vk::RendererStats& st = renderer.stats();
  const double n = double(std::max<uint64_t>(1, presenter.presented())) * 1e6;
  std::printf("gfx_window_demo: per frame (ms): draws %.2f (guest memory copy %.2f, submit %.2f), resolve %.2f, swap %.2f\n",
              st.ns_draws / n, st.ns_memory / n, st.ns_submit / n, st.ns_resolves / n, st.ns_swaps / n);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return failure.empty() ? 0 : 1;
}
