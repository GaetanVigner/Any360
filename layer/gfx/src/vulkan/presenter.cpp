#include "any360/gfx/vulkan/presenter.h"

#include <algorithm>
#include <cstring>

#include "any360/gfx/frame_counter.h"

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
}  // namespace

void Presenter::SetOverlay(const std::string& text) {
  if (text == overlay_text_) return;
  overlay_text_ = text;
  ++overlay_version_;
}

void Presenter::DrawOverlay(VkCommandBuffer cb, VkImage target, uint32_t slot) {
  if (overlay_text_.empty()) return;
  const uint32_t scale = OverlayScale(extent_.height);
  const OverlaySize size = MeasureOverlay(overlay_text_, scale);
  const uint32_t width = std::min(size.width, extent_.width), height = std::min(size.height, extent_.height);
  if (!width || !height) return;
  // A buffer a frame slot: this slot's last present has finished (its fence was waited), so its buffer is rewritten
  // without waiting for the other frames in flight, which may still copy from theirs.
  OverlaySlot& o = overlay_[slot];
  if (o.version != overlay_version_ || o.width != width || o.height != height) {
    if (o.buffer.size < VkDeviceSize(width) * height * 4) {
      DestroyBuffer(device_, o.buffer);
      if (!CreateBuffer(device_, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, o.buffer)) return;
    }
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
    RasterizeOverlay(overlay_text_, scale, OverlayInk(bgra), kOverlayPaper, static_cast<uint32_t*>(o.buffer.mapped),
                     width, height);
    o.version = overlay_version_, o.width = width, o.height = height;
  }
  RecordOverlayCopy(device_.fn(), cb, o.buffer.buffer, target, width, height);
}

void RecordOverlayCopy(const Functions& f, VkCommandBuffer cb, VkBuffer band, VkImage target, uint32_t width,
                       uint32_t height) {
  // After the frame's blit onto the same image (two transfer writes to one image).
  Barrier(f, cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {width, height, 1};
  f.vkCmdCopyBufferToImage(cb, band, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

Presenter::~Presenter() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!d) return;
  {
    std::lock_guard queue_lock(device_.queue_mutex());
    f.vkDeviceWaitIdle(d);
  }
  DestroySwapchain();
  for (uint32_t i = 0; i < kFrames; ++i) {
    if (fences_[i]) f.vkDestroyFence(d, fences_[i], nullptr);
    if (acquired_[i]) f.vkDestroySemaphore(d, acquired_[i], nullptr);
  }
  if (pool_) f.vkDestroyCommandPool(d, pool_, nullptr);
  DestroyImage(device_, frame_);
  DestroyBuffer(device_, staging_);
  for (OverlaySlot& o : overlay_) DestroyBuffer(device_, o.buffer);
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
  {
    std::lock_guard queue_lock(device_.queue_mutex());
    f.vkDeviceWaitIdle(d);
  }
  if (!pool_) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_.queue_family();
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFrames;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (f.vkCreateCommandPool(d, &pool_info, nullptr, &pool_) != VK_SUCCESS) return Fail("command pool");
    alloc.commandPool = pool_;
    if (f.vkAllocateCommandBuffers(d, &alloc, cbs_) != VK_SUCCESS) return Fail("presentation objects");
    for (uint32_t i = 0; i < kFrames; ++i) {
      if (f.vkCreateSemaphore(d, &semaphore, nullptr, &acquired_[i]) != VK_SUCCESS ||
          f.vkCreateFence(d, &fence, nullptr, &fences_[i]) != VK_SUCCESS) {
        return Fail("presentation objects");
      }
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
  info.minImageCount = std::max(caps.minImageCount, 3u);  // the renderer's thread does not wait for each present
  if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent_;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available (see PreferPresentMode)
  if (wanted_ != VK_PRESENT_MODE_FIFO_KHR) {
    uint32_t mode_count = 0;
    f.vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    f.vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical(), device_.surface(), &mode_count, modes.data());
    info.presentMode = ChoosePresentMode(wanted_, modes);
  }
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR next = VK_NULL_HANDLE;
  if (const VkResult made = f.vkCreateSwapchainKHR(d, &info, nullptr, &next); made != VK_SUCCESS) {
    return Fail("vkCreateSwapchainKHR failed", made);
  }
  DestroySwapchain();
  swapchain_ = next;
  format_ = chosen.format;
  ++overlay_version_;  // the overlay rasterized again, in this swapchain's byte order
  mode_ = info.presentMode;
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

  const uint32_t slot = slot_;
  // This slot's last present has run; a CPU frame rewrites the shared frame image and staging: all of them.
  if (rgba) {
    f.vkWaitForFences(d, kFrames, fences_, VK_TRUE, UINT64_MAX);
  } else {
    f.vkWaitForFences(d, 1, &fences_[slot], VK_TRUE, UINT64_MAX);
  }
  const VkCommandBuffer cb = cbs_[slot];
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
  const VkResult acquired = f.vkAcquireNextImageKHR(d, swapchain_, UINT64_MAX, acquired_[slot], VK_NULL_HANDLE, &index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return Fail("swapchain out of date (resize)");
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return Fail("vkAcquireNextImageKHR failed", acquired);
  f.vkResetFences(d, 1, &fences_[slot]);

  f.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  f.vkBeginCommandBuffer(cb, &begin);
  if (rgba) {
    Barrier(f, cb, frame_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy upload{};
    upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    upload.imageExtent = {width, height, 1};
    f.vkCmdCopyBufferToImage(cb, staging_.buffer, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    Barrier(f, cb, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
  const VkImage target = images_[index];
  Barrier(f, cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearColorValue black{{0, 0, 0, 1}};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  f.vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
  {
    // The frame written over the cleared image after the clear (two transfer writes to one image).
    VkMemoryBarrier2 after_clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    after_clear.srcStageMask = after_clear.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    after_clear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after_clear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &after_clear;
    f.vkCmdPipelineBarrier2(cb, &dependency);
  }
  // Fit the frame in the window with its aspect ratio (black bars on the long side).
  const float scale = std::min(float(extent_.width) / width, float(extent_.height) / height);
  const int32_t w = std::max(1, int32_t(width * scale)), h = std::max(1, int32_t(height * scale));
  const int32_t x = (int32_t(extent_.width) - w) / 2, y = (int32_t(extent_.height) - h) / 2;
  if (blit_source) {
    (*blit_source)(cb, target, VkOffset3D{x, y, 0}, VkOffset3D{x + w, y + h, 1});
  } else {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    f.vkCmdBlitImage(cb, frame_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_LINEAR);
  }
  DrawOverlay(cb, target, slot);
  Barrier(f, cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  f.vkEndCommandBuffer(cb);

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &acquired_[slot];
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &rendered_[index];
  std::unique_lock queue_lock(device_.queue_mutex());  // the submit and the present, as one use of the queue
  if (const VkResult submitted = f.vkQueueSubmit(device_.queue(), 1, &submit, fences_[slot]); submitted != VK_SUCCESS) {
    return Fail("vkQueueSubmit failed", submitted);
  }
  slot_ = (slot + 1) % kFrames;
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &rendered_[index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &index;
  const VkResult presented = f.vkQueuePresentKHR(device_.queue(), &present);
  queue_lock.unlock();
  if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
    return presented == VK_ERROR_OUT_OF_DATE_KHR ? Fail("swapchain out of date (resize)")
                                                 : Fail("vkQueuePresentKHR failed", presented);
  }
  ++presented_;
  return true;
}

std::string Presenter::Describe() const {
  if (!swapchain_) return extent_.width && extent_.height ? "no swapchain: " + error_ : "no swapchain (window minimized)";
  const char* mode = mode_ == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                     : mode_ == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                     : mode_ == VK_PRESENT_MODE_FIFO_KHR      ? "fifo"
                                                              : "other";
  return std::to_string(extent_.width) + "x" + std::to_string(extent_.height) + ", format " +
         std::to_string(int(format_)) + ", " + mode + ", " + std::to_string(images_.size()) + " images";
}

}  // namespace any360::gfx::vk
