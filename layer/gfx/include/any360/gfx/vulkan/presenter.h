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
  // A line of text drawn over the top-left corner of every frame presented (a frame rate counter), in a small built-in
  // font (digits, a few letters and signs; others are blank). Empty: none. Cheap: rasterized when it changes, copied
  // onto the swapchain image (a few kilobytes) each frame.
  void SetOverlay(const std::string& text);
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
  // Frames in flight: a present waits for the one three frames back, not the last (the renderer's thread otherwise
  // waits for the GPU to finish every frame before it records the next).
  static constexpr uint32_t kFrames = 3;
  VkCommandBuffer cbs_[kFrames] = {};
  VkSemaphore acquired_[kFrames] = {};
  VkFence fences_[kFrames] = {};
  uint32_t slot_ = 0;
  std::vector<VkSemaphore> rendered_;  // one per swapchain image
  Image frame_;
  Buffer staging_;
  std::string overlay_text_;
  Buffer overlay_;  // the overlay's pixels, in the swapchain's format
  uint32_t overlay_width_ = 0, overlay_height_ = 0, overlay_scale_ = 0;
  bool overlay_dirty_ = false;
  void DrawOverlay(VkCommandBuffer cb, VkImage target, uint32_t slot);  // slot: the frame being recorded
  uint64_t presented_ = 0;
  bool vsync_ = false;
  std::string error_;
};

}  // namespace any360::gfx::vk
