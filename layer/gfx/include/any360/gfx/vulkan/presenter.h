// Presentation: a swapchain on the window surface the platform layer made (Device's surface), each frame of RGBA8
// pixels (a swap's front buffer) uploaded and blitted onto the swapchain image, scaled with its aspect ratio kept
// (black bars), FIFO (vsync) unless asked otherwise. The blit converts to whatever the surface's format is (BGRA on most desktops).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "device.h"

namespace any360::gfx::vk {

// The present mode a swapchain is made with: `wanted` when the surface has it, else FIFO (the one every surface has).
inline VkPresentModeKHR ChoosePresentMode(VkPresentModeKHR wanted, const std::vector<VkPresentModeKHR>& available) {
  for (const VkPresentModeKHR mode : available) {
    if (mode == wanted) return wanted;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

// The overlay's draw, recorded after the frame's blit: a `width` x `height` band of pixels (rows `width` apart, in the
// target's format) copied from `band` onto the top-left corner of `target`, which is in TRANSFER_DST layout.
void RecordOverlayCopy(const Functions& f, VkCommandBuffer cb, VkBuffer band, VkImage target, uint32_t width,
                       uint32_t height);

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
  // Text drawn over the top-left corner of every frame presented (the frame counter, --any360_fps_overlay), a line
  // per newline, in the small built-in font of gfx/frame_counter.h (characters it lacks are blank). Empty: none, and
  // nothing recorded. Rasterized into a frame slot's own buffer when it changes (no wait on the other frames in
  // flight), copied onto the swapchain image each frame.
  void SetOverlay(const std::string& text);
  // The present mode: FIFO (vertical sync) unless another is asked for and the surface has it. FIFO is the one mode
  // every driver must have, and the one that shows: on NVIDIA's Windows driver (2026-11, RTX 5070 Ti, 32.0.16.1088)
  // MAILBOX and IMMEDIATE show nothing on a window a D3D12 flip swapchain presented to before (the runtime's own
  // presenter draws the title's window first; docs/GFX.md, tools/present_probe.cpp). A minimized window has
  // no swapchain, so measuring runs are not held to the display's rate. Takes effect at the next Resize.
  void PreferPresentMode(VkPresentModeKHR mode) { wanted_ = mode; }
  VkFormat format() const { return format_; }
  const std::string& error() const { return error_; }
  // The swapchain as made by the last Resize ("1280x720, format 44, mailbox, 3 images"), or why there is none.
  std::string Describe() const;

 private:
  bool Fail(const std::string& why) {
    error_ = why;
    return false;
  }
  bool Fail(const std::string& why, VkResult result) { return Fail(why + " (VkResult " + std::to_string(int(result)) + ")"); }
  void DestroySwapchain();
  bool PresentWith(const uint32_t* rgba, const BlitFn* blit, uint32_t width, uint32_t height);

  Device& device_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> images_;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkPresentModeKHR wanted_ = VK_PRESENT_MODE_FIFO_KHR;
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
  uint64_t overlay_version_ = 0;  // bumped by each new text
  // The overlay's pixels for each frame slot, in the swapchain's format, and the text version and size they hold.
  struct OverlaySlot {
    Buffer buffer;
    uint64_t version = ~0ull;
    uint32_t width = 0, height = 0;
  };
  OverlaySlot overlay_[kFrames];
  void DrawOverlay(VkCommandBuffer cb, VkImage target, uint32_t slot);  // slot: the frame being recorded
  uint64_t presented_ = 0;
  std::string error_;
};

}  // namespace any360::gfx::vk
