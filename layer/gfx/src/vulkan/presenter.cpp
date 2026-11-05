#include "any360/gfx/vulkan/presenter.h"

#include <algorithm>
#include <cstring>

namespace any360::gfx::vk {

namespace {
void Barrier(const Functions& f, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  f.vkCmdPipelineBarrier2(cb, &dependency);
}
// The overlay's font: 5 x 7 glyphs, a row a byte (bit 4 the leftmost pixel).
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}}, {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},
    {'d', {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}}, {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
    {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}}, {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}}, {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}},
    {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}}, {'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
};
}  // namespace

void Presenter::SetOverlay(const std::string& text) {
  if (text == overlay_text_) return;
  overlay_text_ = text;
  overlay_dirty_ = true;
}

void Presenter::DrawOverlay(VkCommandBuffer cb, VkImage target) {
  if (overlay_text_.empty()) return;
  const Functions& f = device_.fn();
  // Glyph cells of 6 x 9 font pixels (a pixel apart, a pixel of border), each font pixel `scale` swapchain pixels.
  const uint32_t scale = std::clamp(extent_.height / 360, 1u, 4u);
  const uint32_t width = std::min(uint32_t(overlay_text_.size() * 6 + 2) * scale, extent_.width);
  const uint32_t height = std::min(9 * scale, extent_.height);
  if (!width || !height) return;
  if (overlay_dirty_ || scale != overlay_scale_ || width != overlay_width_ || height != overlay_height_) {
    if (overlay_.size < VkDeviceSize(width) * height * 4) {
      DestroyBuffer(device_, overlay_);
      if (!CreateBuffer(device_, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, overlay_)) return;
    }
    // Yellow on a dark grey band, in the swapchain's byte order.
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
    const uint32_t ink = bgra ? 0xFFFFFF40u : 0xFF40FFFFu, paper = 0xFF202020u;
    uint32_t* px = static_cast<uint32_t*>(overlay_.mapped);
    std::fill(px, px + size_t(width) * height, paper);
    for (size_t i = 0; i < overlay_text_.size(); ++i) {
      const Glyph* glyph = nullptr;
      for (const Glyph& g : kFont) {
        if (g.c == overlay_text_[i]) glyph = &g;
      }
      if (!glyph) continue;
      for (uint32_t row = 0; row < 7; ++row) {
        for (uint32_t col = 0; col < 5; ++col) {
          if (!(glyph->rows[row] >> (4 - col) & 1)) continue;
          const uint32_t x0 = uint32_t(1 + i * 6 + col) * scale, y0 = (1 + row) * scale;
          for (uint32_t y = y0; y < y0 + scale && y < height; ++y) {
            for (uint32_t x = x0; x < x0 + scale && x < width; ++x) px[size_t(y) * width + x] = ink;
          }
        }
      }
    }
    overlay_width_ = width, overlay_height_ = height, overlay_scale_ = scale;
    overlay_dirty_ = false;
  }
  // After the frame's blit onto the same image.
  Barrier(f, cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {width, height, 1};
  f.vkCmdCopyBufferToImage(cb, overlay_.buffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

Presenter::~Presenter() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!d) return;
  f.vkDeviceWaitIdle(d);
  DestroySwapchain();
  if (fence_) f.vkDestroyFence(d, fence_, nullptr);
  if (acquired_) f.vkDestroySemaphore(d, acquired_, nullptr);
  if (pool_) f.vkDestroyCommandPool(d, pool_, nullptr);
  DestroyImage(device_, frame_);
  DestroyBuffer(device_, staging_);
  DestroyBuffer(device_, overlay_);
}

void Presenter::DestroySwapchain() {
  const Functions& f = device_.fn();
  for (VkSemaphore s : rendered_) f.vkDestroySemaphore(device_.device(), s, nullptr);
  rendered_.clear();
  if (swapchain_) f.vkDestroySwapchainKHR(device_.device(), swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  images_.clear();
}

bool Presenter::Resize(uint32_t width, uint32_t height) {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!device_.surface()) return Fail("no surface: the device was made without a window");
  f.vkDeviceWaitIdle(d);
  if (!pool_) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_.queue_family();
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (f.vkCreateCommandPool(d, &pool_info, nullptr, &pool_) != VK_SUCCESS) return Fail("command pool");
    alloc.commandPool = pool_;
    if (f.vkAllocateCommandBuffers(d, &alloc, &cb_) != VK_SUCCESS || f.vkCreateSemaphore(d, &semaphore, nullptr, &acquired_) != VK_SUCCESS ||
        f.vkCreateFence(d, &fence, nullptr, &fence_) != VK_SUCCESS) {
      return Fail("presentation objects");
    }
  }

  VkSurfaceCapabilitiesKHR caps;
  f.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical(), device_.surface(), &caps);
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return Fail("the surface takes no transfers");
  uint32_t count = 0;
  f.vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical(), device_.surface(), &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  f.vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical(), device_.surface(), &count, formats.data());
  if (formats.empty()) return Fail("the surface has no format");
  // 8-bit UNORM: the guest front buffer is already gamma-encoded, so no sRGB conversion on top.
  VkSurfaceFormatKHR chosen = formats[0];
  for (const VkSurfaceFormatKHR& sf : formats) {
    if (sf.format == VK_FORMAT_B8G8R8A8_UNORM || sf.format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = sf;
      break;
    }
  }
  extent_ = caps.currentExtent.width != UINT32_MAX
                ? caps.currentExtent
                : VkExtent2D{std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                             std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
  if (!extent_.width || !extent_.height) return true;  // minimized: nothing to present into

  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = device_.surface();
  info.minImageCount = std::max(caps.minImageCount, vsync_ ? 2u : 3u);
  if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent_;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available
  if (!vsync_) {
    uint32_t mode_count = 0;
    f.vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    f.vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &mode_count, modes.data());
    for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
      if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
        info.presentMode = preferred;
        break;
      }
    }
  }
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR next = VK_NULL_HANDLE;
  if (f.vkCreateSwapchainKHR(d, &info, nullptr, &next) != VK_SUCCESS) return Fail("vkCreateSwapchainKHR failed");
  DestroySwapchain();
  swapchain_ = next;
  format_ = chosen.format;
  f.vkGetSwapchainImagesKHR(d, swapchain_, &count, nullptr);
  images_.resize(count);
  f.vkGetSwapchainImagesKHR(d, swapchain_, &count, images_.data());
  rendered_.resize(count);
  for (VkSemaphore& s : rendered_) {
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (f.vkCreateSemaphore(d, &semaphore, nullptr, &s) != VK_SUCCESS) return Fail("semaphore");
  }
  return true;
}

bool Presenter::Present(const uint32_t* rgba, uint32_t width, uint32_t height) { return PresentWith(rgba, nullptr, width, height); }

bool Presenter::Present(const BlitFn& blit, uint32_t width, uint32_t height) { return PresentWith(nullptr, &blit, width, height); }

bool Presenter::PresentWith(const uint32_t* rgba, const BlitFn* blit_source, uint32_t width, uint32_t height) {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!swapchain_) return extent_.width && extent_.height ? Fail("no swapchain (call Resize)") : true;
  if (!width || !height) return Fail("empty frame");

  f.vkWaitForFences(d, 1, &fence_, VK_TRUE, UINT64_MAX);
  // The frame image (RGBA8, the front buffer's size) and its staging memory, for CPU frames.
  if (rgba && (frame_.width != width || frame_.height != height)) {
    DestroyImage(device_, frame_);
    DestroyBuffer(device_, staging_);
    if (!CreateImage2D(device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, frame_) ||
        !CreateBuffer(device_, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging_)) {
      return Fail("frame image");
    }
  }
  if (rgba) std::memcpy(staging_.mapped, rgba, size_t(width) * height * 4);

  uint32_t index = 0;
  const VkResult acquired = f.vkAcquireNextImageKHR(d, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return Fail("swapchain out of date (resize)");
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return Fail("vkAcquireNextImageKHR failed");
  f.vkResetFences(d, 1, &fence_);

  f.vkResetCommandBuffer(cb_, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  f.vkBeginCommandBuffer(cb_, &begin);
  if (rgba) {
    Barrier(f, cb_, frame_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy upload{};
    upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    upload.imageExtent = {width, height, 1};
    f.vkCmdCopyBufferToImage(cb_, staging_.buffer, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    Barrier(f, cb_, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
  const VkImage target = images_[index];
  Barrier(f, cb_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearColorValue black{{0, 0, 0, 1}};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  f.vkCmdClearColorImage(cb_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
  // Fit the frame in the window with its aspect ratio (black bars on the long side).
  const float scale = std::min(float(extent_.width) / width, float(extent_.height) / height);
  const int32_t w = std::max(1, int32_t(width * scale)), h = std::max(1, int32_t(height * scale));
  const int32_t x = (int32_t(extent_.width) - w) / 2, y = (int32_t(extent_.height) - h) / 2;
  if (blit_source) {
    (*blit_source)(cb_, target, VkOffset3D{x, y, 0}, VkOffset3D{x + w, y + h, 1});
  } else {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    f.vkCmdBlitImage(cb_, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_LINEAR);
  }
  DrawOverlay(cb_, target);
  Barrier(f, cb_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  f.vkEndCommandBuffer(cb_);

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &acquired_;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb_;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &rendered_[index];
  if (f.vkQueueSubmit(device_.queue(), 1, &submit, fence_) != VK_SUCCESS) return Fail("vkQueueSubmit failed");
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &rendered_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;
  const VkResult presented = f.vkQueuePresentKHR(device_.queue(), &present);
  if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
    return presented == VK_ERROR_OUT_OF_DATE_KHR ? Fail("swapchain out of date (resize)") : Fail("vkQueuePresentKHR failed");
  }
  ++presented_;
  return true;
}

}  // namespace any360::gfx::vk
