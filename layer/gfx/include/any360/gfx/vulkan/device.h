// The Vulkan device of any360::gfx: the loader opened at run time (vulkan-1.dll, libvulkan.so.1, libMoltenVK /
// libvulkan.1.dylib), so nothing links against a Vulkan SDK; an instance, one physical device and a device with a
// graphics queue, Vulkan 1.3 (dynamic rendering, synchronization2) as the Steam Deck's RADV and every current
// desktop driver offer. Headless: a window's swapchain is the platform layer's.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
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
  VkSurfaceKHR surface() const { return surface_; }
  // VK_EXT_external_memory_host: the alignment a host allocation must have to be imported (0: not available).
  VkDeviceSize host_pointer_alignment() const { return host_pointer_alignment_; }
  uint32_t queue_family() const { return queue_family_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  const std::string& error() const { return error_; }

  // A memory type with the bits of `type_bits` and all of `flags`, or UINT32_MAX.
  uint32_t MemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;

  // Records commands into a one-time command buffer, submits it and waits.
  template <typename Record>
  bool Submit(Record&& record) {
    VkCommandBuffer cb = BeginOneTime();
    if (!cb) return false;
    record(cb);
    return EndOneTime(cb);
  }

 private:
  VkCommandBuffer BeginOneTime();
  bool EndOneTime(VkCommandBuffer cb);

  void* library_ = nullptr;
  Functions fn_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkDeviceSize host_pointer_alignment_ = 0;
  uint32_t queue_family_ = 0;
  VkCommandPool pool_ = VK_NULL_HANDLE;
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
bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out);
void DestroyBuffer(const Device& d, Buffer& b);

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
};
bool CreateImage2D(const Device& d, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, Image& out);
void DestroyImage(const Device& d, Image& i);

}  // namespace any360::gfx::vk
