// The Vulkan device of any360::gfx: the loader opened at run time (vulkan-1.dll, libvulkan.so.1, libMoltenVK /
// libvulkan.1.dylib), so nothing links against a Vulkan SDK; an instance, one physical device and a device with a
// graphics queue, Vulkan 1.3 (dynamic rendering, synchronization2) as the Steam Deck's RADV and every current
// desktop driver offer. Headless: a window's swapchain is the platform layer's.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace any360::gfx::vk {

// The entry points the renderer calls, loaded once the instance and the device exist.
struct Functions {
#define ANY360_VK_FN(name) PFN_##name name = nullptr;
#include "functions.inc"
#undef ANY360_VK_FN
};

struct DeviceOptions {
  bool validation = false;     // VK_LAYER_KHRONOS_validation when installed
  std::string prefer;          // a substring of the device name to choose ("Radeon", "NVIDIA"); else discrete first
  // Presenting to a window: the instance extensions the platform layer needs (VK_KHR_surface and the window
  // system's), and the surface it makes once the instance exists. The device then has a queue that presents to it
  // and VK_KHR_swapchain.
  std::vector<const char*> instance_extensions;
  std::function<VkSurfaceKHR(VkInstance)> make_surface;
};

class Device {
 public:
  Device() = default;
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // False with error() set when no loader, no 1.3 device or no graphics queue.
  bool Create(const DeviceOptions& options = {});
  void Destroy();

  const Functions& fn() const { return fn_; }
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  // Held around every use of the queue (vkQueueSubmit, vkQueuePresentKHR, vkQueueWaitIdle, vkDeviceWaitIdle): the
  // renderer's submitter thread, the presenter and one-time submits use it from different threads, and Vulkan wants
  // a queue used by one thread at a time (two at once lost the device now and then at a title's start, when the
  // window's resize waited for the device while a frame's batches were being submitted).
  std::mutex& queue_mutex() const { return queue_mutex_; }
  VkSurfaceKHR surface() const { return surface_; }
  // VK_EXT_external_memory_host: the alignment a host allocation must have to be imported (0: not available).
  VkDeviceSize host_pointer_alignment() const { return host_pointer_alignment_; }
  uint32_t queue_family() const { return queue_family_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  bool occlusion_query_precise() const { return occlusion_query_precise_; }  // exact sample counts
  // After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), or why it cannot say.
  std::string DescribeFault() const;
  // Diagnostics (ANY360_GPU_CHECKPOINTS=1, VK_NV_device_diagnostic_checkpoints): a marker the GPU passes in `cb`, and
  // after a lost device the last marker each pipeline stage passed (stage flags, marker).
  bool checkpoints() const { return set_checkpoint_ != nullptr; }
  void SetCheckpoint(VkCommandBuffer cb, uint64_t marker) const {
    if (set_checkpoint_) set_checkpoint_(cb, reinterpret_cast<const void*>(uintptr_t(marker)));
  }
  std::vector<std::pair<VkPipelineStageFlags, uint64_t>> QueueCheckpoints() const;
  const std::string& error() const { return error_; }

  // A memory type with the bits of `type_bits` and all of `flags`, or UINT32_MAX.
  uint32_t MemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;

  // Records commands into a one-time command buffer, submits it and waits until it and everything submitted before
  // it has run. Callable from several threads at once: each submit in flight records into a command pool of its own.
  template <typename Record>
  bool Submit(Record&& record) {
    OneTime* one = BeginOneTime();
    if (!one) return false;
    record(one->cb);
    return EndOneTime(one);
  }

 private:
  // A command pool, its command buffer and the fence its submit signals. Vulkan wants a pool, and every command
  // buffer recorded from it, used by one thread at a time: one shared pool, allocated from, recorded and freed by a
  // renderer's read-back and another thread's one-time submit at once, crashed in the driver now and then; and a
  // lock around a shared pool let a thread submitting in a loop starve the other (std::mutex is not fair). So each
  // submit takes an idle one (or makes one) and gives it back, and holds the queue only for vkQueueSubmit.
  struct OneTime {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
  };
  OneTime* BeginOneTime();
  bool EndOneTime(OneTime* one);

  void* library_ = nullptr;
  Functions fn_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  mutable std::mutex queue_mutex_;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkDeviceSize host_pointer_alignment_ = 0;
  uint32_t queue_family_ = 0;
  bool occlusion_query_precise_ = false;
  bool device_fault_ = false;
  PFN_vkGetDeviceFaultInfoEXT get_fault_ = nullptr;
  PFN_vkCmdSetCheckpointNV set_checkpoint_ = nullptr;
  PFN_vkGetQueueCheckpointDataNV get_checkpoints_ = nullptr;
  std::mutex one_time_mutex_;                         // guards the two lists below (not held while recording)
  std::vector<std::unique_ptr<OneTime>> one_times_;  // every one made, destroyed with the device
  std::vector<OneTime*> idle_one_times_;              // those no submit is using
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_{};
  std::string error_;
};

// A buffer or image with its own memory (tests and readbacks; the renderer suballocates).
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
};
// `where`: the caller, for the allocation log (SetAllocationLog).
bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out,
                  std::source_location where = std::source_location::current());
void DestroyBuffer(const Device& d, Buffer& b);

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
};
// `alias`: another format of the same size views of the image may take (VK_FORMAT_UNDEFINED: none).
bool CreateImage2D(const Device& d, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, Image& out, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                   VkFormat alias = VK_FORMAT_UNDEFINED, std::source_location where = std::source_location::current());
void DestroyImage(const Device& d, Image& i);

// Buffers and images created since the start, and their bytes: a renderer that creates some every frame pays the
// driver and the kernel twice (the allocation, then its release on another thread).
struct GpuObjectCounts {
  uint64_t buffers = 0, images = 0, bytes = 0;
};
GpuObjectCounts CreatedGpuObjects();
// Each creation from now on logged with its size, format and caller (null: none). For finding what a renderer
// creates in a steady scene.
void SetAllocationLog(std::function<void(const std::string&)> log);

}  // namespace any360::gfx::vk
