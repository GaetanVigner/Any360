// Presentation: a swapchain on the window surface the platform layer made (Device's surface), each frame of RGBA8
// pixels (a swap's front buffer) uploaded and blitted onto the swapchain image, scaled with its aspect ratio kept
// (black bars), FIFO (vsync). The blit converts to whatever the surface's format is (BGRA on most desktops).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "device.h"

namespace any360::gfx::vk {

class Presenter {
 public:
  explicit Presenter(Device& device) : device_(device) {}
  ~Presenter();

  // (Re)creates the swapchain for the window's drawable size (call again when it changes).
  bool Resize(uint32_t width, uint32_t height);
  // Shows `rgba` (width x height, R in the low byte). False with error() on failure; a swapchain the window has
  // outgrown is recreated on the next Resize.
  bool Present(const uint32_t* rgba, uint32_t width, uint32_t height);
  // Shows a width x height image already on the GPU: `blit` records its blit onto the swapchain image (in
  // TRANSFER_DST layout) over the rectangle given.
  using BlitFn = std::function<void(VkCommandBuffer, VkImage target, VkOffset3D dst0, VkOffset3D dst1)>;
  bool Present(const BlitFn& blit, uint32_t width, uint32_t height);

  uint64_t presented() const { return presented_; }
  // Vertical sync (FIFO). Off (the default): frames as fast as they come, MAILBOX where the driver has it (no tearing),
  // else IMMEDIATE. Takes effect at the next Resize.
  void SetVsync(bool on) { vsync_ = on; }
  VkFormat format() const { return format_; }
  const std::string& error() const { return error_; }

 private:
  bool Fail(const std::string& why) {
    error_ = why;
    return false;
  }
  void DestroySwapchain();
  bool PresentWith(const uint32_t* rgba, const BlitFn* blit, uint32_t width, uint32_t height);

  Device& device_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> images_;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE;
  std::vector<VkSemaphore> rendered_;  // one per swapchain image
  VkFence fence_ = VK_NULL_HANDLE;
  Image frame_;
  Buffer staging_;
  uint64_t presented_ = 0;
  bool vsync_ = false;
  std::string error_;
};

}  // namespace any360::gfx::vk
