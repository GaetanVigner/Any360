#include "any360/gfx/vulkan/device.h"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace any360::gfx::vk {

namespace {

void* OpenLoader() {
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
  for (const char* name : {"libvulkan.1.dylib", "libMoltenVK.dylib"}) {
    if (void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) return lib;
  }
  return nullptr;
#else
  for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
    if (void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) return lib;
  }
  return nullptr;
#endif
}

void CloseLoader(void* library) {
  if (!library) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
  dlclose(library);
#endif
}

PFN_vkGetInstanceProcAddr LoaderEntry(void* library) {
#if defined(_WIN32)
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), "vkGetInstanceProcAddr")));
#else
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
}

}  // namespace

Device::~Device() { Destroy(); }

bool Device::Create(const DeviceOptions& options) {
  Destroy();
  library_ = OpenLoader();
  if (!library_) {
    error_ = "no Vulkan loader";
    return false;
  }
  const PFN_vkGetInstanceProcAddr get = LoaderEntry(library_);
  if (!get) {
    error_ = "the Vulkan loader has no vkGetInstanceProcAddr";
    return false;
  }
  auto load_global = [&](const char* name) { return get(VK_NULL_HANDLE, name); };
  fn_.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(load_global("vkCreateInstance"));
  fn_.vkEnumerateInstanceLayerProperties =
      reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(load_global("vkEnumerateInstanceLayerProperties"));
  fn_.vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(load_global("vkEnumerateInstanceVersion"));
  uint32_t version = VK_API_VERSION_1_0;
  if (fn_.vkEnumerateInstanceVersion) fn_.vkEnumerateInstanceVersion(&version);
  if (version < VK_API_VERSION_1_3) {
    error_ = "the Vulkan loader is older than 1.3";
    return false;
  }

  std::vector<const char*> layers;
  if (options.validation && fn_.vkEnumerateInstanceLayerProperties) {
    uint32_t count = 0;
    fn_.vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    fn_.vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const VkLayerProperties& layer : available) {
      if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
    }
  }
  std::vector<const char*> extensions = options.instance_extensions;
  VkInstanceCreateFlags instance_flags = 0;
#if defined(__APPLE__)
  // MoltenVK is a portability implementation: listed only when asked for.
  extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "any360";
  app.pEngineName = "any360::gfx";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.flags = instance_flags;
  instance_info.pApplicationInfo = &app;
  instance_info.enabledLayerCount = uint32_t(layers.size());
  instance_info.ppEnabledLayerNames = layers.data();
  instance_info.enabledExtensionCount = uint32_t(extensions.size());
  instance_info.ppEnabledExtensionNames = extensions.data();
  if (fn_.vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
    error_ = "vkCreateInstance failed";
    return false;
  }
#define ANY360_VK_FN(name)                                                       \
  if (!fn_.name) fn_.name = reinterpret_cast<PFN_##name>(get(instance_, #name));
#include "any360/gfx/vulkan/functions.inc"
#undef ANY360_VK_FN
  if (options.make_surface) {
    surface_ = options.make_surface(instance_);
    if (!surface_) {
      error_ = "the platform layer made no surface";
      return false;
    }
  }

  // The physical device: Vulkan 1.3 with a graphics queue (that presents to the surface, when there is one); the
  // preferred name, else a discrete GPU, else the first.
  uint32_t count = 0;
  fn_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  fn_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  int best_score = -1;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties props;
    fn_.vkGetPhysicalDeviceProperties(candidate, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;
    uint32_t family_count = 0;
    fn_.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    fn_.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
      VkBool32 presents = VK_TRUE;
      if (surface_) fn_.vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &presents);
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) {
        family = i;
        break;
      }
    }
    if (family == UINT32_MAX) continue;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13};
    fn_.vkGetPhysicalDeviceFeatures2(candidate, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) continue;
    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
    if (!options.prefer.empty() && std::strstr(props.deviceName, options.prefer.c_str())) score = 3;
    if (score > best_score) {
      best_score = score;
      physical_ = candidate;
      queue_family_ = family;
      properties_ = props;
    }
  }
  if (!physical_) {
    error_ = "no Vulkan 1.3 device with dynamic rendering, synchronization2 and a graphics queue";
    return false;
  }
  fn_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  v13.dynamicRendering = VK_TRUE;
  v13.synchronization2 = VK_TRUE;
  // What the translated shaders use where the device has it (every desktop driver and RADV do).
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  fn_.vkGetPhysicalDeviceFeatures2(physical_, &supported);
  VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13};
  enabled.features.shaderClipDistance = supported.features.shaderClipDistance;
  enabled.features.shaderCullDistance = supported.features.shaderCullDistance;
  enabled.features.fragmentStoresAndAtomics = supported.features.fragmentStoresAndAtomics;
  enabled.features.vertexPipelineStoresAndAtomics = supported.features.vertexPipelineStoresAndAtomics;
  enabled.features.samplerAnisotropy = supported.features.samplerAnisotropy;
  enabled.features.fullDrawIndexUint32 = supported.features.fullDrawIndexUint32;
  enabled.features.independentBlend = supported.features.independentBlend;
  enabled.features.depthClamp = supported.features.depthClamp;
  enabled.features.imageCubeArray = supported.features.imageCubeArray;
  enabled.features.shaderStorageImageWriteWithoutFormat = supported.features.shaderStorageImageWriteWithoutFormat;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  std::vector<const char*> device_extensions;
  if (surface_) device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  // Importing host memory: the renderer then reads guest memory in place instead of copying it.
  {
    uint32_t count = 0;
    fn_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    fn_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data());
    for (const VkExtensionProperties& e : available) {
      if (std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
        device_extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
        fn_.vkGetPhysicalDeviceProperties2(physical_, &props2);
        host_pointer_alignment_ = host.minImportedHostPointerAlignment;
      }
    }
  }
  device_info.enabledExtensionCount = uint32_t(device_extensions.size());
  device_info.ppEnabledExtensionNames = device_extensions.data();
  if (fn_.vkCreateDevice(physical_, &device_info, nullptr, &device_) != VK_SUCCESS) {
    error_ = "vkCreateDevice failed";
    return false;
  }
  // Device functions straight from the driver, past the loader's trampolines.
#define ANY360_VK_FN(name)                                                                              \
  if (PFN_vkVoidFunction direct = fn_.vkGetDeviceProcAddr(device_, #name)) {                            \
    fn_.name = reinterpret_cast<PFN_##name>(direct);                                                    \
  }
#include "any360/gfx/vulkan/functions.inc"
#undef ANY360_VK_FN
  fn_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_;
  if (fn_.vkCreateCommandPool(device_, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
    error_ = "vkCreateCommandPool failed";
    return false;
  }
  return true;
}

void Device::Destroy() {
  if (device_) {
    fn_.vkDeviceWaitIdle(device_);
    if (pool_) fn_.vkDestroyCommandPool(device_, pool_, nullptr);
    fn_.vkDestroyDevice(device_, nullptr);
  }
  if (surface_ && instance_) fn_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
  surface_ = VK_NULL_HANDLE;
  if (instance_) fn_.vkDestroyInstance(instance_, nullptr);
  CloseLoader(library_);
  library_ = nullptr;
  fn_ = {};
  instance_ = VK_NULL_HANDLE;
  physical_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
}

uint32_t Device::MemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  return UINT32_MAX;
}

VkCommandBuffer Device::BeginOneTime() {
  VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  info.commandPool = pool_;
  info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (fn_.vkAllocateCommandBuffers(device_, &info, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  fn_.vkBeginCommandBuffer(cb, &begin);
  return cb;
}

bool Device::EndOneTime(VkCommandBuffer cb) {
  fn_.vkEndCommandBuffer(cb);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  const bool ok = fn_.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && fn_.vkQueueWaitIdle(queue_) == VK_SUCCESS;
  fn_.vkFreeCommandBuffers(device_, pool_, 1, &cb);
  return ok;
}

bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out) {
  const Functions& f = d.fn();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  if (f.vkCreateBuffer(d.device(), &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  f.vkGetBufferMemoryRequirements(d.device(), out.buffer, &req);
  const VkMemoryPropertyFlags flags = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = UINT32_MAX;
  // Readbacks (the CPU reads what the GPU wrote) want cached memory: reading write-combined memory is orders of
  // magnitude slower (a 1280x720 resolve read back took ~100 ms).
  if (host_visible && usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
    alloc.memoryTypeIndex = d.MemoryType(req.memoryTypeBits, flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  }
  if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = d.MemoryType(req.memoryTypeBits, flags);
  if (alloc.memoryTypeIndex == UINT32_MAX || f.vkAllocateMemory(d.device(), &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
  f.vkBindBufferMemory(d.device(), out.buffer, out.memory, 0);
  if (host_visible) f.vkMapMemory(d.device(), out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
  out.size = size;
  return true;
}

void DestroyBuffer(const Device& d, Buffer& b) {
  if (b.buffer) d.fn().vkDestroyBuffer(d.device(), b.buffer, nullptr);
  if (b.memory) d.fn().vkFreeMemory(d.device(), b.memory, nullptr);
  b = {};
}

bool CreateImage2D(const Device& d, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, Image& out) {
  const Functions& f = d.fn();
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (f.vkCreateImage(d.device(), &info, nullptr, &out.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  f.vkGetImageMemoryRequirements(d.device(), out.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = d.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX || f.vkAllocateMemory(d.device(), &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
  f.vkBindImageMemory(d.device(), out.image, out.memory, 0);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, 1, 0, 1};
  if (f.vkCreateImageView(d.device(), &view, nullptr, &out.view) != VK_SUCCESS) return false;
  out.format = format;
  out.width = width;
  out.height = height;
  return true;
}

void DestroyImage(const Device& d, Image& i) {
  if (i.view) d.fn().vkDestroyImageView(d.device(), i.view, nullptr);
  if (i.image) d.fn().vkDestroyImage(d.device(), i.image, nullptr);
  if (i.memory) d.fn().vkFreeMemory(d.device(), i.memory, nullptr);
  i = {};
}

}  // namespace any360::gfx::vk
