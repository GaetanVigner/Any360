#include "any360/gfx/vulkan/device.h"

#include <atomic>
#include <cstring>
#include <mutex>

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
  // Object names for validation messages (Device::Name): with the validation layer, asked for here or through the
  // loader's VK_INSTANCE_LAYERS, or with ANY360_GPU_NAMES=1.
  bool names = options.validation;
  if (const char* env = std::getenv("ANY360_GPU_NAMES"); env && *env == '1') names = true;
  if (const char* env = std::getenv("VK_INSTANCE_LAYERS"); env && std::strstr(env, "validation")) names = true;
  if (names) {
    names = false;
    if (auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            load_global("vkEnumerateInstanceExtensionProperties"))) {
      uint32_t count = 0;
      enumerate(nullptr, &count, nullptr);
      std::vector<VkExtensionProperties> available(count);
      enumerate(nullptr, &count, available.data());
      for (const VkExtensionProperties& e : available) {
        if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) names = true;
      }
    }
    if (names) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
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
  // Uniform blocks laid out as the translated shaders declare them (std430-like offsets), where the device has it.
  VkPhysicalDeviceVulkan13Features v13_supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features v12_supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13_supported};
  {
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12_supported};
    fn_.vkGetPhysicalDeviceFeatures2(physical_, &query);
  }
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
  v12.uniformBufferStandardLayout = v12_supported.uniformBufferStandardLayout;
  v12.scalarBlockLayout = v12_supported.scalarBlockLayout;  // the system constants' vec3 + float members
  v13.shaderDemoteToHelperInvocation = v13_supported.shaderDemoteToHelperInvocation;
  VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
  enabled.features.shaderClipDistance = supported.features.shaderClipDistance;
  enabled.features.shaderCullDistance = supported.features.shaderCullDistance;
  enabled.features.fragmentStoresAndAtomics = supported.features.fragmentStoresAndAtomics;
  enabled.features.vertexPipelineStoresAndAtomics = supported.features.vertexPipelineStoresAndAtomics;
  enabled.features.samplerAnisotropy = supported.features.samplerAnisotropy;
  enabled.features.occlusionQueryPrecise = supported.features.occlusionQueryPrecise;
  enabled.features.fillModeNonSolid = supported.features.fillModeNonSolid;  // wireframe (line polygon mode)
  fill_mode_non_solid_ = supported.features.fillModeNonSolid;
  occlusion_query_precise_ = supported.features.occlusionQueryPrecise;
  enabled.features.shaderStorageImageExtendedFormats = supported.features.shaderStorageImageExtendedFormats;
  storage_image_extended_formats_ = supported.features.shaderStorageImageExtendedFormats;
  enabled.features.fullDrawIndexUint32 = supported.features.fullDrawIndexUint32;
  enabled.features.independentBlend = supported.features.independentBlend;
  enabled.features.depthClamp = supported.features.depthClamp;
  enabled.features.imageCubeArray = supported.features.imageCubeArray;
  enabled.features.shaderStorageImageWriteWithoutFormat = supported.features.shaderStorageImageWriteWithoutFormat;
  enabled.features.sampleRateShading = supported.features.sampleRateShading;
  // Diagnostics: ANY360_GPU_ROBUST=1 makes out-of-bounds buffer accesses defined (a lost device that this cures was
  // a shader reading or writing past a buffer).
  if (const char* robust = std::getenv("ANY360_GPU_ROBUST"); robust && *robust == '1') {
    enabled.features.robustBufferAccess = supported.features.robustBufferAccess;
  }
  // A lost device's fault (address, vendor data) when the driver reports it (VK_EXT_device_fault).
  VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executables{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
  const char* stats_env = std::getenv("ANY360_PIPELINE_STATS");
  bool pipeline_stats = false;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  std::vector<const char*> device_extensions;
  if (surface_) device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  // Diagnostics: ANY360_GPU_CHECKPOINTS=1 marks every draw for the GPU, so a lost device says which it stopped in.
  const char* checkpoints_env = std::getenv("ANY360_GPU_CHECKPOINTS");
  bool checkpoints = false;
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
      if (checkpoints_env && *checkpoints_env == '1' &&
          std::strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) == 0) {
        device_extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        checkpoints = true;
      }
      if (stats_env && *stats_env &&
          std::strcmp(e.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0) {
        device_extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        executables.pipelineExecutableInfo = VK_TRUE;
        executables.pNext = enabled.pNext;
        enabled.pNext = &executables;
        pipeline_stats = true;
      }
      if (stats_env && *stats_env && std::getenv("ANY360_PIPELINE_ISA") &&
          std::strcmp(e.extensionName, VK_AMD_SHADER_INFO_EXTENSION_NAME) == 0) {
        device_extensions.push_back(VK_AMD_SHADER_INFO_EXTENSION_NAME);
      }
      if (std::strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0) {
        VkPhysicalDeviceFaultFeaturesEXT has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has};
        fn_.vkGetPhysicalDeviceFeatures2(physical_, &features);
        if (has.deviceFault) {
          device_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
          fault.deviceFault = VK_TRUE;
          fault.deviceFaultVendorBinary = VK_FALSE;
          fault.pNext = enabled.pNext;
          enabled.pNext = &fault;
          device_fault_ = true;
        }
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
  if (pipeline_stats) {
    get_executables_ = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        fn_.vkGetDeviceProcAddr(device_, "vkGetPipelineExecutablePropertiesKHR"));
    get_executable_statistics_ = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        fn_.vkGetDeviceProcAddr(device_, "vkGetPipelineExecutableStatisticsKHR"));
    get_executable_representations_ = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
        fn_.vkGetDeviceProcAddr(device_, "vkGetPipelineExecutableInternalRepresentationsKHR"));
    if (!get_executable_statistics_) get_executables_ = nullptr;
    get_shader_info_amd_ = reinterpret_cast<PFN_vkGetShaderInfoAMD>(fn_.vkGetDeviceProcAddr(device_, "vkGetShaderInfoAMD"));
  }
  if (names) set_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(get(instance_, "vkSetDebugUtilsObjectNameEXT"));
  if (device_fault_) {
    get_fault_ = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(fn_.vkGetDeviceProcAddr(device_, "vkGetDeviceFaultInfoEXT"));
  }
  if (checkpoints) {
    set_checkpoint_ = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(fn_.vkGetDeviceProcAddr(device_, "vkCmdSetCheckpointNV"));
    get_checkpoints_ =
        reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(fn_.vkGetDeviceProcAddr(device_, "vkGetQueueCheckpointDataNV"));
    if (!set_checkpoint_ || !get_checkpoints_) set_checkpoint_ = nullptr, get_checkpoints_ = nullptr;
  }
  return true;
}

std::vector<std::pair<VkPipelineStageFlags, uint64_t>> Device::QueueCheckpoints() const {
  std::vector<std::pair<VkPipelineStageFlags, uint64_t>> passed;
  if (!get_checkpoints_) return passed;
  uint32_t count = 0;
  get_checkpoints_(queue_, &count, nullptr);
  std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
  get_checkpoints_(queue_, &count, data.data());
  for (uint32_t i = 0; i < count; ++i) passed.push_back({data[i].stage, uint64_t(uintptr_t(data[i].pCheckpointMarker))});
  return passed;
}

std::string Device::DescribeFault() const {
  if (!get_fault_) return "no fault information (VK_EXT_device_fault unavailable)";
  VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
  if (get_fault_(device_, &counts, nullptr) != VK_SUCCESS) return "no fault information";
  std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
  std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
  counts.vendorBinarySize = 0;
  VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
  info.pAddressInfos = addresses.data();
  info.pVendorInfos = vendor.data();
  if (get_fault_(device_, &counts, &info) != VK_SUCCESS) return "no fault information";
  std::string text = std::string("fault: ") + info.description;
  static const char* const kTypes[] = {"none", "read invalid", "write invalid", "execute invalid",
                                       "instruction pointer unknown", "instruction pointer invalid",
                                       "instruction pointer fault"};
  for (const VkDeviceFaultAddressInfoEXT& a : addresses) {
    char line[128];
    std::snprintf(line, sizeof(line), "; %s at 0x%llX (+-0x%llX)", a.addressType < 7 ? kTypes[a.addressType] : "?",
                  (unsigned long long)a.reportedAddress, (unsigned long long)a.addressPrecision);
    text += line;
  }
  for (const VkDeviceFaultVendorInfoEXT& v : vendor) {
    char line[320];
    std::snprintf(line, sizeof(line), "; %s (code 0x%llX, data 0x%llX)", v.description,
                  (unsigned long long)v.vendorFaultCode, (unsigned long long)v.vendorFaultData);
    text += line;
  }
  return text;
}

void Device::Destroy() {
  if (device_) {
    {
      std::lock_guard queue_lock(queue_mutex_);
      fn_.vkDeviceWaitIdle(device_);
    }
    for (const std::unique_ptr<OneTime>& one : one_times_) {
      if (one->fence) fn_.vkDestroyFence(device_, one->fence, nullptr);
      if (one->pool) fn_.vkDestroyCommandPool(device_, one->pool, nullptr);
    }
    one_times_.clear();
    idle_one_times_.clear();
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
}

uint32_t Device::MemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  return UINT32_MAX;
}

Device::OneTime* Device::BeginOneTime() {
  OneTime* one = nullptr;
  {
    std::lock_guard lock(one_time_mutex_);
    if (!idle_one_times_.empty()) {
      one = idle_one_times_.back();
      idle_one_times_.pop_back();
    }
  }
  if (one) {
    fn_.vkResetCommandPool(device_, one->pool, 0);
  } else {
    auto made = std::make_unique<OneTime>();
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool made_ok = fn_.vkCreateCommandPool(device_, &pool_info, nullptr, &made->pool) == VK_SUCCESS;
    if (made_ok) {
      VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      info.commandPool = made->pool;
      info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      info.commandBufferCount = 1;
      made_ok = fn_.vkAllocateCommandBuffers(device_, &info, &made->cb) == VK_SUCCESS &&
                fn_.vkCreateFence(device_, &fence_info, nullptr, &made->fence) == VK_SUCCESS;
    }
    if (!made_ok) {
      if (made->fence) fn_.vkDestroyFence(device_, made->fence, nullptr);
      if (made->pool) fn_.vkDestroyCommandPool(device_, made->pool, nullptr);
      return nullptr;
    }
    one = made.get();
    std::lock_guard lock(one_time_mutex_);
    one_times_.push_back(std::move(made));
  }
  VkCommandBuffer cb = one->cb;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  fn_.vkBeginCommandBuffer(cb, &begin);
  // After everything submitted before it (a renderer's batches may still be running).
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  fn_.vkCmdPipelineBarrier2(cb, &dependency);
  return one;
}

bool Device::EndOneTime(OneTime* one) {
  fn_.vkEndCommandBuffer(one->cb);
  fn_.vkResetFences(device_, 1, &one->fence);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &one->cb;
  bool ok;
  {
    std::lock_guard queue_lock(queue_mutex_);
    ok = fn_.vkQueueSubmit(queue_, 1, &submit, one->fence) == VK_SUCCESS;
  }
  // The fence of a vkQueueSubmit covers every command submitted before it too (and the barrier BeginOneTime records
  // orders them): once signalled, the queue has run everything up to this submit, as vkQueueWaitIdle did, without
  // holding the queue from other threads while it waits.
  if (ok && fn_.vkWaitForFences(device_, 1, &one->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    return false;  // the command buffer may still be pending (a lost device): not given back, freed with the device
  }
  std::lock_guard lock(one_time_mutex_);
  idle_one_times_.push_back(one);
  return ok;
}

namespace {
std::atomic<uint64_t> g_buffers{0}, g_images{0}, g_bytes{0};
std::mutex g_allocation_log_mutex;
std::function<void(const std::string&)> g_allocation_log;
std::atomic<bool> g_allocation_logging{false};

void Created(const char* what, uint64_t bytes, uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
             const std::source_location& where) {
  g_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (!g_allocation_logging.load(std::memory_order_relaxed)) return;
  std::string file = where.file_name();
  file = file.substr(file.find_last_of("/\\") + 1);
  char text[200];
  std::snprintf(text, sizeof text, "Any360: created %s %ux%u format %u usage 0x%X, %llu KB, at %s:%u", what, width,
                height, format, usage, static_cast<unsigned long long>(bytes >> 10), file.c_str(), unsigned(where.line()));
  std::lock_guard lock(g_allocation_log_mutex);
  if (g_allocation_log) g_allocation_log(text);
}
}  // namespace

GpuObjectCounts CreatedGpuObjects() {
  return {g_buffers.load(std::memory_order_relaxed), g_images.load(std::memory_order_relaxed),
          g_bytes.load(std::memory_order_relaxed)};
}

void SetAllocationLog(std::function<void(const std::string&)> log) {
  std::lock_guard lock(g_allocation_log_mutex);
  g_allocation_log = std::move(log);
  g_allocation_logging.store(bool(g_allocation_log), std::memory_order_relaxed);
}

bool CreateBuffer(const Device& d, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out,
                  std::source_location where) {
  g_buffers.fetch_add(1, std::memory_order_relaxed);
  Created("buffer", size, 0, 0, 0, usage, where);
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
  if (d.naming()) {
    std::string file = where.file_name();
    char name[160];
    std::snprintf(name, sizeof name, "buffer %llu B usage 0x%X at %s:%u", static_cast<unsigned long long>(size), usage,
                  file.substr(file.find_last_of("/\\") + 1).c_str(), unsigned(where.line()));
    d.Name(VK_OBJECT_TYPE_BUFFER, uint64_t(out.buffer), name);
  }
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
                   VkImageAspectFlags aspect, Image& out, VkSampleCountFlagBits samples, VkFormat alias,
                   std::source_location where) {
  g_images.fetch_add(1, std::memory_order_relaxed);
  const Functions& f = d.fn();
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  const VkFormat formats[2] = {format, alias};
  VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  if (alias != VK_FORMAT_UNDEFINED) {
    // Listed, so that the driver can keep the image compressed.
    info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    format_list.viewFormatCount = 2;
    format_list.pViewFormats = formats;
    info.pNext = &format_list;
  }
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
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
  Created("image", req.size, width, height, uint32_t(format), usage, where);
  if (d.naming()) {
    std::string file = where.file_name();
    char name[160];
    std::snprintf(name, sizeof name, "image %ux%u format %u samples %u usage 0x%X at %s:%u", width, height, unsigned(format),
                  unsigned(samples), usage, file.substr(file.find_last_of("/\\") + 1).c_str(), unsigned(where.line()));
    d.Name(VK_OBJECT_TYPE_IMAGE, uint64_t(out.image), name);
  }
  out.format = format;
  out.width = width;
  out.height = height;
  // A view only for an image that can have one (a transfer-only image, a resolve's staging, has none).
  constexpr VkImageUsageFlags kViewUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  if (!(usage & kViewUsage)) return true;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {aspect, 0, 1, 0, 1};
  return f.vkCreateImageView(d.device(), &view, nullptr, &out.view) == VK_SUCCESS;
}

void DestroyImage(const Device& d, Image& i) {
  if (i.view) d.fn().vkDestroyImageView(d.device(), i.view, nullptr);
  if (i.image) d.fn().vkDestroyImage(d.device(), i.image, nullptr);
  if (i.memory) d.fn().vkFreeMemory(d.device(), i.memory, nullptr);
  i = {};
}

}  // namespace any360::gfx::vk
