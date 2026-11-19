// Present probe (Windows): does a window show a Vulkan swapchain's frames after a D3D12 swapchain presented to it, as
// a title's window does when the runtime's D3D12 presenter draws it before Any360 takes it? Four windows (always on
// top) on one screen, each cleared to a pulsing colour by Vulkan from a render thread; the eye says which show:
//   1 red    plain window, MAILBOX
//   2 green  a D3D12 FLIP_DISCARD swapchain presented then released; the Vulkan surface made while it still lived; MAILBOX
//   3 blue   the same D3D12 swapchain, released before the Vulkan surface is made; MAILBOX
//   4 yellow like 2, FIFO (control)
// NVIDIA RTX 5070 Ti, driver 32.0.16.1088 (2026-11-19): 1 and 4 show, 2 and 3 stay black (docs/GFX.md, "A black
// window"). Stdout: each swapchain made, then the frames presented and failed per window.
// Build (Developer prompt): cl /O1 /EHsc /std:c++17 /I %VULKAN_SDK%\Include present_probe.cpp
//                           /link /LIBPATH:%VULKAN_SDK%\Lib
// Usage: present_probe.exe [x y seconds]   (x, y: top-left of the 2x2 grid, in desktop coordinates; default 120 s)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "vulkan-1.lib")
using Microsoft::WRL::ComPtr;

struct Variant {
  const char* title;
  bool d3d12;
  bool surface_while_d3d12;
  VkPresentModeKHR mode;
  float r, g, b;
};
static const Variant kVariants[4] = {
    {"probe 1 red: plain, MAILBOX", false, false, VK_PRESENT_MODE_MAILBOX_KHR, 1, 0, 0},
    {"probe 2 green: D3D12 before, surface while it lived, MAILBOX", true, true, VK_PRESENT_MODE_MAILBOX_KHR, 0, 1, 0},
    {"probe 3 blue: D3D12 before, surface after, MAILBOX", true, false, VK_PRESENT_MODE_MAILBOX_KHR, 0, 0, 1},
    {"probe 4 yellow: D3D12 before, surface while it lived, FIFO", true, true, VK_PRESENT_MODE_FIFO_KHR, 1, 1, 0},
};

struct Win {
  HWND hwnd = nullptr;
  ComPtr<IDXGISwapChain1> dxgi;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::vector<VkImage> images;
  VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  unsigned long long presented = 0, failed = 0;
};

#define CHECK(x)                                                    \
  do {                                                              \
    VkResult r_ = (x);                                              \
    if (r_ != VK_SUCCESS) {                                         \
      std::printf("FAIL %s = %d (line %d)\n", #x, int(r_), __LINE__); \
      std::fflush(stdout);                                          \
      std::exit(1);                                                 \
    }                                                               \
  } while (0)

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char** argv) {
  const int x0 = argc > 2 ? std::atoi(argv[1]) : 2600;
  const int y0 = argc > 2 ? std::atoi(argv[2]) : 520;
  const int seconds = argc > 3 ? std::atoi(argv[3]) : 120;
  const int ww = 900, wh = 480;

  WNDCLASSW wc{};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"Any360PresentProbe";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  Win wins[4];
  for (int i = 0; i < 4; ++i) {
    wchar_t title[128];
    MultiByteToWideChar(CP_UTF8, 0, kVariants[i].title, -1, title, 128);
    wins[i].hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x0 + (i % 2) * (ww + 10),
                                   y0 + (i / 2) * (wh + 10), ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
  }
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

  // D3D12 on the NVIDIA adapter, as the runtime's presenter: a flip swapchain presented a few times.
  ComPtr<IDXGIFactory4> factory;
  CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    if (d.VendorId == 0x10DE) break;
    adapter.Reset();
  }
  ComPtr<ID3D12Device> d3d;
  ComPtr<ID3D12CommandQueue> queue12;
  if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d)))) {
    std::printf("FAIL no D3D12 device on NVIDIA\n");
    return 1;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12));
  for (int i = 0; i < 4; ++i) {
    if (!kVariants[i].d3d12) continue;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue12.Get(), wins[i].hwnd, &sd, nullptr, nullptr, &wins[i].dxgi);
    std::printf("window %d: D3D12 swapchain 0x%08lX\n", i + 1, hr);
    for (int k = 0; SUCCEEDED(hr) && k < 10; ++k) wins[i].dxgi->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  }
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

  // Vulkan on the NVIDIA device.
  const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 2;
  ici.ppEnabledExtensionNames = iext;
  VkInstance instance;
  CHECK(vkCreateInstance(&ici, nullptr, &instance));
  auto make_surface = [&](Win& w) {
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = wc.hInstance;
    si.hwnd = w.hwnd;
    CHECK(vkCreateWin32SurfaceKHR(instance, &si, nullptr, &w.surface));
  };
  for (int i = 0; i < 4; ++i) {
    if (!kVariants[i].d3d12 || kVariants[i].surface_while_d3d12) make_surface(wins[i]);
  }
  // The runtime's presenter detached: its swapchain released.
  for (int i = 0; i < 4; ++i) wins[i].dxgi.Reset();
  {
    ComPtr<ID3D12Fence> fence;
    d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue12->Signal(fence.Get(), 1);
    while (fence->GetCompletedValue() < 1) Sleep(1);
  }
  for (int i = 0; i < 4; ++i) {
    if (kVariants[i].d3d12 && !kVariants[i].surface_while_d3d12) make_surface(wins[i]);
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> pds(count);
  vkEnumeratePhysicalDevices(instance, &count, pds.data());
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  for (VkPhysicalDevice p : pds) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (std::strstr(props.deviceName, "NVIDIA")) {
      pd = p;
      std::printf("Vulkan device %s\n", props.deviceName);
    }
  }
  if (!pd) {
    std::printf("FAIL no NVIDIA Vulkan device\n");
    return 1;
  }
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  dq.queueFamilyIndex = 0;
  dq.queueCount = 1;
  dq.pQueuePriorities = &prio;
  const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &dq;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dext;
  VkDevice device;
  CHECK(vkCreateDevice(pd, &dci, nullptr, &device));
  VkQueue queue;
  vkGetDeviceQueue(device, 0, 0, &queue);

  std::atomic<bool> done{false};
  // The render thread makes the swapchains and presents, as the title's renderer does (not the window's thread).
  std::thread render([&] {
    for (int i = 0; i < 4; ++i) {
      Win& w = wins[i];
      VkBool32 ok = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, w.surface, &ok);
      VkSurfaceCapabilitiesKHR caps;
      CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, w.surface, &caps));
      uint32_t n = 0;
      vkGetPhysicalDeviceSurfacePresentModesKHR(pd, w.surface, &n, nullptr);
      std::vector<VkPresentModeKHR> modes(n);
      vkGetPhysicalDeviceSurfacePresentModesKHR(pd, w.surface, &n, modes.data());
      w.mode = VK_PRESENT_MODE_FIFO_KHR;
      for (VkPresentModeKHR m : modes) {
        if (m == kVariants[i].mode) w.mode = m;
      }
      VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
      sc.surface = w.surface;
      sc.minImageCount = std::max(caps.minImageCount, 3u);
      if (caps.maxImageCount) sc.minImageCount = std::min(sc.minImageCount, caps.maxImageCount);
      sc.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
      sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      sc.imageExtent = caps.currentExtent;
      sc.imageArrayLayers = 1;
      sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      sc.preTransform = caps.currentTransform;
      sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      sc.presentMode = w.mode;
      sc.clipped = VK_TRUE;
      CHECK(vkCreateSwapchainKHR(device, &sc, nullptr, &w.swapchain));
      n = 0;
      vkGetSwapchainImagesKHR(device, w.swapchain, &n, nullptr);
      w.images.resize(n);
      vkGetSwapchainImagesKHR(device, w.swapchain, &n, w.images.data());
      VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vkCreateSemaphore(device, &semi, nullptr, &w.acquired);
      vkCreateSemaphore(device, &semi, nullptr, &w.rendered);
      std::printf("window %d: present support %u, %ux%u, mode %d (%u modes), %u images\n", i + 1, ok,
                  caps.currentExtent.width, caps.currentExtent.height, int(w.mode), unsigned(modes.size()), n);
    }
    std::fflush(stdout);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cb));
    const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
    for (unsigned frame = 0; GetTickCount64() < end; ++frame) {
      const float pulse = 0.55f + 0.45f * std::sin(frame * 0.05f);
      for (int i = 0; i < 4; ++i) {
        Win& w = wins[i];
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(device, w.swapchain, 1000000000ull, w.acquired, VK_NULL_HANDLE, &index);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
          if (++w.failed <= 3) std::printf("window %d: acquire %d\n", i + 1, int(r));
          continue;
        }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = w.images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkClearColorValue c{{kVariants[i].r * pulse, kVariants[i].g * pulse, kVariants[i].b * pulse, 1.0f}};
        vkCmdClearColorImage(cb, w.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &w.acquired;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &w.rendered;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &w.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &w.swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
          ++w.presented;
        } else if (++w.failed <= 3) {
          std::printf("window %d: present %d\n", i + 1, int(r));
        }
        vkQueueWaitIdle(queue);
      }
      Sleep(4);
    }
    vkDeviceWaitIdle(device);
    for (int i = 0; i < 4; ++i) {
      std::printf("window %d: %llu presented, %llu failed\n", i + 1, wins[i].presented, wins[i].failed);
    }
    std::fflush(stdout);
    done = true;
  });
  while (!done) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(5);
  }
  render.join();
  return 0;
}
