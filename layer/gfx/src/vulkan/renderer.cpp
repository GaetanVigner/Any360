#include "any360/gfx/vulkan/renderer.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace any360::gfx::vk {

namespace rg = rex::graphics;
using rg::SpirvShaderTranslator;

namespace {
// Adds the time from construction to destruction to a counter.
struct Timer {
  uint64_t& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit Timer(uint64_t& t) : total(t) {}
  ~Timer() { total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
};
// Register file indices the renderer reads.
constexpr uint32_t kRbDepthInfo = 0x2002, kPaScScreenScissorTl = 0x200E, kPaScScreenScissorBr = 0x200F,
                   kPaScWindowOffset = 0x2080, kPaScWindowScissorTl = 0x2081, kPaScWindowScissorBr = 0x2082,
                   kRbColorMask = 0x2104, kRbBlendRed = 0x2105, kRbStencilRefMaskBf = 0x210C,
                   kRbStencilRefMask = 0x210D, kPaClVportXScale = 0x210F, kRbDepthControl = 0x2200,
                   kRbColorControl = 0x2202, kPaSuScModeCntl = 0x2205, kPaClVteCntl = 0x2206, kPaSuVtxCntl = 0x2302,
                   kRbModeControl = 0x2208, kPaSuPolyOffsetFrontScale = 0x2380, kRbSurfaceInfo = 0x2000,
                   kSqProgramCntl = 0x2180, kSqContextMisc = 0x2181, kVgtMaxVtxIndx = 0x2100, kVgtMinVtxIndx = 0x2101,
                   kVgtIndxOffset = 0x2102, kVgtMultiPrimIbResetIndx = 0x2103;
constexpr uint32_t kRbColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
constexpr uint32_t kRbBlendControl[4] = {0x2201, 0x2209, 0x220A, 0x220B};
// Bytes each constant buffer can hold (the most a shader can address).
constexpr uint32_t kFloatBufferBytes = 256 * 16, kBoolLoopBytes = (8 + 32) * 4, kFetchBytes = 32 * 6 * 4;
// RB_BLENDCONTROL with 1 * source + 0 * destination for colour and alpha: no blending.
constexpr uint32_t kNoBlending = 0x00010001;
// The polygon offset unit of a 24-bit normalized depth buffer (D3D10 rules, as the reference uses).
constexpr float kPolygonOffsetUnorm24 = float((1u << 24) - 1), kPolygonOffsetFloat24 = float(1u << (21 + 3));

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

float AsFloat(uint32_t bits) { return std::bit_cast<float>(bits); }

// The topology of a Xenos primitive type the host draws as is; MAX_ENUM for those that need expansion.
VkPrimitiveTopology Topology(uint32_t primitive_type) {
  switch (primitive_type) {
    case 0x01: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 0x03: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 0x04: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 0x06: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  }
}

bool IsPolygonal(VkPrimitiveTopology topology) {
  return topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
         topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
}

// Xenos blend factors (5 bits) and operations (3 bits); unknown values read as zero / add, as the reference does.
VkBlendFactor BlendFactor(uint32_t factor) {
  static const VkBlendFactor kMap[17] = {
      VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,           VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_SRC_COLOR,     VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
  return factor < 17 ? kMap[factor] : VK_BLEND_FACTOR_ZERO;
}
VkBlendOp BlendOp(uint32_t op) {
  static const VkBlendOp kMap[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                    VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
  return kMap[op & 7];
}
// Xenos compare functions and stencil operations number the same as Vulkan's.
VkStencilOpState StencilState(uint32_t func, uint32_t fail, uint32_t pass, uint32_t depth_fail) {
  VkStencilOpState s{};
  s.failOp = VkStencilOp(fail);
  s.passOp = VkStencilOp(pass);
  s.depthFailOp = VkStencilOp(depth_fail);
  s.compareOp = VkCompareOp(func);
  return s;
}
}  // namespace

struct VulkanRenderer::TranslatedShader {
  std::unique_ptr<rg::SpirvShader> shader;
  std::map<uint64_t, VkShaderModule> modules;  // by modification (VK_NULL_HANDLE: translation failed)
};

VulkanRenderer::VulkanRenderer(Device& device, uint8_t* physical, uint32_t mirror_bytes)
    : device_(device), physical_(physical), mirror_bytes_(mirror_bytes) {
  // Every translator feature on: what desktop Vulkan 1.3 drivers and RADV offer (to be read from the device).
  translator_ = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(true), true, true, false);
}

VulkanRenderer::~VulkanRenderer() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!d) return;
  if (batch_) Flush();
  f.vkDeviceWaitIdle(d);
  if (batch_fence_) f.vkDestroyFence(d, batch_fence_, nullptr);
  if (gamma_pipeline_) f.vkDestroyPipeline(d, gamma_pipeline_, nullptr);
  if (gamma_pipeline_layout_) f.vkDestroyPipelineLayout(d, gamma_pipeline_layout_, nullptr);
  if (gamma_pool_) f.vkDestroyDescriptorPool(d, gamma_pool_, nullptr);
  if (depth_pipeline_) f.vkDestroyPipeline(d, depth_pipeline_, nullptr);
  if (depth_pipeline_layout_) f.vkDestroyPipelineLayout(d, depth_pipeline_layout_, nullptr);
  if (depth_pool_) f.vkDestroyDescriptorPool(d, depth_pool_, nullptr);
  if (depth_layout_) f.vkDestroyDescriptorSetLayout(d, depth_layout_, nullptr);
  if (gamma_layout_) f.vkDestroyDescriptorSetLayout(d, gamma_layout_, nullptr);
  DestroyBuffer(device_, gamma_lut_buffer_);
  DestroyImage(device_, gamma_image_);
  if (batch_pool_) f.vkDestroyCommandPool(d, batch_pool_, nullptr);
  for (auto& [key, pipeline] : pipelines_) f.vkDestroyPipeline(d, pipeline, nullptr);
  for (auto& [key, shader] : shaders_) {
    for (auto& [modification, module] : shader->modules) {
      if (module) f.vkDestroyShaderModule(d, module, nullptr);
    }
  }
  if (pool_) f.vkDestroyDescriptorPool(d, pool_, nullptr);
  if (draw_pool_) f.vkDestroyDescriptorPool(d, draw_pool_, nullptr);
  for (auto& [key, layout] : pipeline_layouts_) f.vkDestroyPipelineLayout(d, layout, nullptr);
  for (auto& [key, layout] : texture_layouts_) f.vkDestroyDescriptorSetLayout(d, layout, nullptr);
  for (VkDescriptorSetLayout l : {shared_layout_, constants_layout_, empty_layout_}) {
    if (l) f.vkDestroyDescriptorSetLayout(d, l, nullptr);
  }
  textures_.reset();
  DestroyBuffer(device_, shared_memory_);
  DestroyBuffer(device_, constants_);
  DestroyBuffer(device_, indices_);
  for (auto& [key, surface] : surfaces_) {
    if (surface->depth_view) f.vkDestroyImageView(d, surface->depth_view, nullptr);
    DestroyImage(device_, surface->image);
  }
  for (auto& [base, r] : resolved_) DestroyImage(device_, r.image);
}

bool VulkanRenderer::Fail(const std::string& why) {
  last_error_ = why;
  ++failures_[why];
  ++stats_.failed;
  return false;
}

bool VulkanRenderer::Initialize() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  constant_alignment_ = std::max<uint32_t>(16, uint32_t(device_.properties().limits.minUniformBufferOffsetAlignment));

  // The translated shaders' layout (as the reference Vulkan backend builds it): shared memory, the five constant
  // buffers as dynamic uniform buffers, vertex and pixel textures (none yet).
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (f.vkCreateDescriptorSetLayout(d, &info, nullptr, &empty_layout_) != VK_SUCCESS) return Fail("empty set layout");
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr};
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  info.pBindings = constant_bindings;
  if (f.vkCreateDescriptorSetLayout(d, &info, nullptr, &constants_layout_) != VK_SUCCESS) return Fail("constants set layout");
  const uint32_t shared_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(device_.properties().limits.maxStorageBufferRange);
  if (shared_count_log2 != 0) return Fail("devices with a storage buffer range under 512 MB are not handled yet");
  VkDescriptorSetLayoutBinding shared_binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr};
  info.bindingCount = 1;
  info.pBindings = &shared_binding;
  if (f.vkCreateDescriptorSetLayout(d, &info, nullptr, &shared_layout_) != VK_SUCCESS) return Fail("shared set layout");
  textures_ = std::make_unique<TextureCache>(device_, physical_, mirror_bytes_);
  textures_->SetBeforeMemoryRead([this](uint32_t begin, uint32_t end) {
    for (auto& [base, r] : resolved_) {
      if (!r.stale && !r.pending) continue;
      const uint32_t element = r.image.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
      const uint64_t bytes = uint64_t(r.pitch) * ((r.y + r.height + 31) & ~31u) * element;
      if (base < end && begin < base + bytes) {
        if (r.stale) {
          WriteBackResolved(base, r);
        } else {
          Flush();  // its write is recorded in the batch
        }
      }
    }
  });
  textures_->SetRecorder([this] {
    VkCommandBuffer cb = Record();
    EndRendering();  // transfers and barriers are recorded outside rendering
    return cb;
  });
  // Texture sets: allocated per draw, the pool reset once the draw has run.
  VkDescriptorPoolSize draw_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDrawDescriptors}, {VK_DESCRIPTOR_TYPE_SAMPLER, kDrawDescriptors}};
  VkDescriptorPoolCreateInfo draw_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  draw_pool_info.maxSets = kDrawSets;
  draw_pool_info.poolSizeCount = 2;
  draw_pool_info.pPoolSizes = draw_sizes;
  if (f.vkCreateDescriptorPool(d, &draw_pool_info, nullptr, &draw_pool_) != VK_SUCCESS) return Fail("texture descriptor pool");

  // Guest memory for the shaders: imported in place when the device can (VK_EXT_external_memory_host, an aligned
  // allocation), so nothing is copied; otherwise a mirror copied before each draw.
  shared_memory_imported_ = ImportGuestMemory();
  if (!shared_memory_imported_ && !CreateBuffer(device_, mirror_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, shared_memory_)) {
    return Fail("guest memory buffer");
  }
  // One draw's constants, indices.
  // The batch's rings: constants and indices of every draw until the next flush.
  if (!CreateBuffer(device_, 32u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, constants_) ||
      !CreateBuffer(device_, 16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, indices_)) {
    return Fail("buffers");
  }

  // Descriptor sets, written once: dynamic offsets select each draw's constants.
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 3;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &pool_) != VK_SUCCESS) return Fail("descriptor pool");
  VkDescriptorSetLayout to_allocate[] = {shared_layout_, constants_layout_, empty_layout_};
  VkDescriptorSet allocated[3];
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 3;
  alloc.pSetLayouts = to_allocate;
  if (f.vkAllocateDescriptorSets(d, &alloc, allocated) != VK_SUCCESS) return Fail("descriptor sets");
  shared_set_ = allocated[0];
  constants_set_ = allocated[1];
  empty_set_ = allocated[2];
  VkDescriptorBufferInfo shared_info{shared_memory_.buffer, 0, VK_WHOLE_SIZE};
  const uint32_t ranges[SpirvShaderTranslator::kConstantBufferCount] = {
      uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)), kFloatBufferBytes, kFloatBufferBytes, kBoolLoopBytes, kFetchBytes};
  VkDescriptorBufferInfo constant_infos[SpirvShaderTranslator::kConstantBufferCount];
  VkWriteDescriptorSet writes[1 + SpirvShaderTranslator::kConstantBufferCount] = {};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shared_set_, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
               nullptr, &shared_info, nullptr};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_infos[i] = {constants_.buffer, 0, ranges[i]};
    writes[1 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, constants_set_, i, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &constant_infos[i], nullptr};
  }
  f.vkUpdateDescriptorSets(d, 1 + SpirvShaderTranslator::kConstantBufferCount, writes, 0, nullptr);

  // The batch: one command buffer and its fence.
  VkCommandPoolCreateInfo batch_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  batch_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  batch_pool.queueFamilyIndex = device_.queue_family();
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (f.vkCreateCommandPool(d, &batch_pool, nullptr, &batch_pool_) != VK_SUCCESS ||
      f.vkCreateFence(d, &fence, nullptr, &batch_fence_) != VK_SUCCESS) {
    return Fail("batch objects");
  }
  VkCommandBufferAllocateInfo batch_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  batch_alloc.commandPool = batch_pool_;
  batch_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  batch_alloc.commandBufferCount = 1;
  if (f.vkAllocateCommandBuffers(d, &batch_alloc, &batch_) != VK_SUCCESS) return Fail("batch command buffer");
  // Depth/stencil surfaces: D24S8 where the device has it (NVIDIA), else D32S8 (AMD).
  depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkFormatProperties props;
  f.vkGetPhysicalDeviceFormatProperties(device_.physical(), VK_FORMAT_D24_UNORM_S8_UINT, &props);
  if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
  return true;
}

void VulkanRenderer::ReadsGuestMemory(uint32_t address, uint32_t bytes) {
  if (shared_memory_imported_ || !bytes || address >= mirror_bytes_) return;
  batch_ranges_.emplace_back(address, uint32_t(std::min<uint64_t>(uint64_t(address) + bytes, mirror_bytes_)));
}

bool VulkanRenderer::ImportGuestMemory() {
  const Functions& f = device_.fn();
  const VkDeviceSize alignment = device_.host_pointer_alignment();
  if (!alignment || !f.vkGetMemoryHostPointerPropertiesEXT || reinterpret_cast<uintptr_t>(physical_) % alignment ||
      mirror_bytes_ % alignment) {
    return false;
  }
  VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
  info.size = mirror_bytes_;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (f.vkCreateBuffer(device_.device(), &info, nullptr, &shared_memory_.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  f.vkGetBufferMemoryRequirements(device_.device(), shared_memory_.buffer, &req);
  VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
  if (f.vkGetMemoryHostPointerPropertiesEXT(device_.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, physical_,
                                            &host) != VK_SUCCESS) {
    DestroyBuffer(device_, shared_memory_);
    return false;
  }
  const uint32_t types = host.memoryTypeBits & req.memoryTypeBits;
  VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  import.pHostPointer = physical_;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
  alloc.allocationSize = mirror_bytes_;
  alloc.memoryTypeIndex = types ? uint32_t(std::countr_zero(types)) : UINT32_MAX;
  if (alloc.memoryTypeIndex == UINT32_MAX ||
      f.vkAllocateMemory(device_.device(), &alloc, nullptr, &shared_memory_.memory) != VK_SUCCESS ||
      f.vkBindBufferMemory(device_.device(), shared_memory_.buffer, shared_memory_.memory, 0) != VK_SUCCESS) {
    DestroyBuffer(device_, shared_memory_);
    return false;
  }
  shared_memory_.size = mirror_bytes_;
  return true;
}

VkFormat ColorTargetFormat(uint32_t color_format) {
  switch (color_format) {
    case 0: return VK_FORMAT_R8G8B8A8_UNORM;            // k_8_8_8_8
    case 1: return VK_FORMAT_R8G8B8A8_UNORM;            // k_8_8_8_8_GAMMA (gamma applied in the shader)
    case 2: case 10: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;  // k_2_10_10_10 (and AS_10_10_10_10), as the reference
    case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;   // k_2_10_10_10_FLOAT (and AS_16_16_16_16)
    case 4: return VK_FORMAT_R16G16_SNORM;              // k_16_16: -32...32 scaled to -1...1 by the exponent bias
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;        // k_16_16_16_16: likewise
    case 6: return VK_FORMAT_R16G16_SFLOAT;             // k_16_16_FLOAT
    case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;       // k_16_16_16_16_FLOAT
    case 14: return VK_FORMAT_R32_SFLOAT;               // k_32_FLOAT
    case 15: return VK_FORMAT_R32G32_SFLOAT;            // k_32_32_FLOAT
    default: return VK_FORMAT_UNDEFINED;
  }
}

namespace {
bool Is64bpp(uint32_t color_format) { return color_format == 5 || color_format == 7 || color_format == 15; }
}  // namespace

VulkanRenderer::Surface* VulkanRenderer::GetSurface(const SurfaceKey& key) {
  auto& slot = surfaces_[key];
  if (slot) return slot->image.image ? slot.get() : nullptr;
  slot = std::make_unique<Surface>();
  slot->key = key;
  const VkFormat format = key.depth ? depth_format_ : ColorTargetFormat(key.format);
  if (format == VK_FORMAT_UNDEFINED || key.pitch == 0 || key.base >= 2048) return nullptr;
  // The rows the surface can span from its base to the end of EDRAM (2048 tiles of 80x16 samples at 32 bpp).
  const uint32_t tiles_per_row = (key.pitch + 79) / 80 * (!key.depth && Is64bpp(key.format) ? 2 : 1);
  const uint32_t height = std::min<uint32_t>(4096, (2048 - key.base) / tiles_per_row * 16);
  if (!height) return nullptr;
  // Colour surfaces are also sampled: the gamma pass reads the presented frame from its surface.
  // Depth surfaces too: depth resolves read them on the GPU.
  const VkImageUsageFlags usage = (key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageAspectFlags aspect =
      key.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  if (!CreateImage2D(device_, key.pitch, height, format, usage, aspect, slot->image)) {
    DestroyImage(device_, slot->image);
    return nullptr;
  }
  ++stats_.render_targets;
  return slot.get();
}

void VulkanRenderer::Transition(VkCommandBuffer cb, Image& image, VkImageLayout& layout, VkImageLayout to,
                                VkImageAspectFlags aspect) {
  if (layout == to) return;
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout = layout;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {aspect, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  device_.fn().vkCmdPipelineBarrier2(cb, &dependency);
  layout = to;
}

bool VulkanRenderer::ClearColor(const SurfaceKey& key, const float rgba[4]) {
  Flush();  // what was drawn before comes first
  Surface* s = GetSurface(key);
  if (!s || key.depth) return false;
  const Functions& f = device_.fn();
  return device_.Submit([&](VkCommandBuffer cb) {
    Transition(cb, s->image, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue color;
    std::memcpy(color.float32, rgba, sizeof(color.float32));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(cb, s->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
  });
}

bool VulkanRenderer::ClearDepthStencil(const SurfaceKey& key, float depth, uint32_t stencil) {
  Flush();  // what was drawn before comes first
  Surface* s = GetSurface(key);
  if (!s || !key.depth) return false;
  const Functions& f = device_.fn();
  return device_.Submit([&](VkCommandBuffer cb) {
    const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Transition(cb, s->image, s->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects);
    const VkClearDepthStencilValue value{depth, stencil};
    VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
    f.vkCmdClearDepthStencilImage(cb, s->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
  });
}

bool VulkanRenderer::ReadBack(const SurfaceKey& key, uint32_t width, uint32_t height, std::vector<uint8_t>& bytes) {
  Flush();  // what was drawn before comes first
  Surface* s = GetSurface(key);
  if (!s || key.depth || width > s->image.width || height > s->image.height) return false;
  const Functions& f = device_.fn();
  // Every host colour format is 4 bytes a texel but the four-component 16-bit and two-component 32-bit ones.
  const VkFormat format = ColorTargetFormat(key.format);
  const uint32_t texel_bytes = format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16B16A16_SNORM ||
                                       format == VK_FORMAT_R32G32_SFLOAT
                                   ? 8
                                   : 4;
  Buffer readback;
  const VkDeviceSize size = VkDeviceSize(width) * height * texel_bytes;
  if (!CreateBuffer(device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback)) return false;
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    Transition(cb, s->image, s->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    f.vkCmdCopyImageToBuffer(cb, s->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
  });
  if (ok) {
    bytes.resize(size_t(size));
    std::memcpy(bytes.data(), readback.mapped, size_t(size));
  }
  DestroyBuffer(device_, readback);
  return ok;
}

VulkanRenderer::TranslatedShader* VulkanRenderer::GetShader(const gfx::Shader& source) {
  auto& slot = shaders_[{source.type, source.hash}];
  if (slot) return slot.get();
  slot = std::make_unique<TranslatedShader>();
  const rg::xenos::ShaderType type = source.type ? rg::xenos::ShaderType::kPixel : rg::xenos::ShaderType::kVertex;
  slot->shader = std::make_unique<rg::SpirvShader>(type, source.hash, source.microcode.data(), source.microcode.size(),
                                                   std::endian::native);
  rex::string::StringBuffer disassembly;
  slot->shader->AnalyzeUcode(disassembly);
  return slot.get();
}

VkShaderModule VulkanRenderer::GetModule(TranslatedShader& shader, uint64_t modification) {
  Timer shader_timer(stats_.ns_shaders);
  auto it = shader.modules.find(modification);
  if (it != shader.modules.end()) return it->second;
  VkShaderModule& module = shader.modules[modification];
  rg::Shader::Translation& translation = *shader.shader->GetOrCreateTranslation(modification);
  if (!translator_->TranslateAnalyzedShader(translation) || !translation.is_valid()) return module = VK_NULL_HANDLE;
  const std::vector<uint8_t>& binary = translation.translated_binary();
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = binary.size();
  info.pCode = reinterpret_cast<const uint32_t*>(binary.data());
  if (device_.fn().vkCreateShaderModule(device_.device(), &info, nullptr, &module) != VK_SUCCESS) return module = VK_NULL_HANDLE;
  ++stats_.translations;
  return module;
}

VkDescriptorSetLayout VulkanRenderer::TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers) {
  if (!textures && !samplers) return empty_layout_;
  VkDescriptorSetLayout& layout = texture_layouts_[{uint32_t(vertex), textures, samplers}];
  if (layout) return layout;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  const VkShaderStageFlags stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < textures; ++i) bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
  for (uint32_t i = 0; i < samplers; ++i) bindings.push_back({textures + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage, nullptr});
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  if (device_.fn().vkCreateDescriptorSetLayout(device_.device(), &info, nullptr, &layout) != VK_SUCCESS) layout = VK_NULL_HANDLE;
  return layout;
}

VkPipelineLayout VulkanRenderer::PipelineLayout(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures,
                                                uint32_t ps_samplers) {
  VkPipelineLayout& layout = pipeline_layouts_[{vs_textures, vs_samplers, ps_textures, ps_samplers}];
  if (layout) return layout;
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = TextureSetLayout(true, vs_textures, vs_samplers);
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = TextureSetLayout(false, ps_textures, ps_samplers);
  VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  info.pSetLayouts = sets;
  if (device_.fn().vkCreatePipelineLayout(device_.device(), &info, nullptr, &layout) != VK_SUCCESS) layout = VK_NULL_HANDLE;
  return layout;
}

VkPipeline VulkanRenderer::GetPipeline(const PipelineKey& key) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = VkShaderModule(key.vs_module);
  stages[0].pName = "main";
  if (key.ps_module) {
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = VkShaderModule(key.ps_module);
    stages[1].pName = "main";
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(key.topology);
  assembly.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VkPolygonMode(key.polygon_mode);
  raster.cullMode = key.cull_mode;
  raster.frontFace = key.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = VK_TRUE;  // the bias itself is dynamic
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  // Depth and stencil from RB_DEPTHCONTROL (masks and references dynamic).
  const uint32_t dc = key.depth_control;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = (dc >> 1) & 1;
  depth_stencil.depthWriteEnable = ((dc >> 1) & 1) && ((dc >> 2) & 1);
  depth_stencil.depthCompareOp = VkCompareOp((dc >> 4) & 7);
  depth_stencil.stencilTestEnable = dc & 1;
  depth_stencil.front = StencilState((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7);
  depth_stencil.back = ((dc >> 7) & 1) ? StencilState((dc >> 20) & 7, (dc >> 23) & 7, (dc >> 26) & 7, (dc >> 29) & 7)
                                       : depth_stencil.front;
  // Colour targets 0-3: blending (none when it is 1 * source + 0 * destination) and write mask of each.
  VkPipelineColorBlendAttachmentState blend_attachments[4] = {};
  VkFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = VkFormat(key.color_formats[i]);
    VkPipelineColorBlendAttachmentState& a = blend_attachments[i];
    a.colorWriteMask = (key.color_masks >> (4 * i)) & 0xF;
    const uint32_t bc = key.blend_controls[i];
    if (bc != kNoBlending && color_formats[i] != VK_FORMAT_UNDEFINED) {
      a.blendEnable = VK_TRUE;
      a.srcColorBlendFactor = BlendFactor(bc & 0x1F);
      a.colorBlendOp = BlendOp((bc >> 5) & 7);
      a.dstColorBlendFactor = BlendFactor((bc >> 8) & 0x1F);
      a.srcAlphaBlendFactor = BlendFactor((bc >> 16) & 0x1F);
      a.alphaBlendOp = BlendOp((bc >> 21) & 7);
      a.dstAlphaBlendFactor = BlendFactor((bc >> 24) & 0x1F);
    }
  }
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 4;
  blend.pAttachments = blend_attachments;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                           VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  // Unbound locations are VK_FORMAT_UNDEFINED: the shader's writes to them go nowhere.
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 4;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
  info.stageCount = key.ps_module ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = VkPipelineLayout(key.layout);
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (device_.fn().vkCreateGraphicsPipelines(device_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  ++stats_.pipelines;
  pipelines_[key] = pipeline;
  return pipeline;
}

void VulkanRenderer::Draw(const DrawCall& draw) {
  Timer draw_timer(stats_.ns_draws);
  const Functions& f = device_.fn();
  const uint32_t* regs = draw.registers;
  // EDRAM mode: 4 colour and depth, 5 depth only; anything else leaves the targets alone.
  const uint32_t edram_mode = regs[kRbModeControl] & 7;
  if (edram_mode != 4 && edram_mode != 5) {
    ++stats_.skipped;
    return;
  }
  if (!draw.vertex_shader || draw.vertex_shader->microcode.empty()) {
    Fail("no vertex shader");
    return;
  }
  // Rectangle lists are drawn as two-triangle strips whose fourth corner the vertex shader makes, quad lists as
  // triangle lists (a, b, c, a, c, d), as the reference does without geometry shaders.
  const bool rectangle_list = draw.primitive_type == 0x08, quad_list = draw.primitive_type == 0x0D;
  const VkPrimitiveTopology topology = rectangle_list ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                       : quad_list    ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                                      : Topology(draw.primitive_type);
  const uint32_t primitives = rectangle_list ? draw.index_count / 3 : quad_list ? draw.index_count / 4 : 0;
  const uint32_t host_index_count = rectangle_list ? (primitives ? 5 * primitives - 1 : 0)
                                    : quad_list    ? 6 * primitives
                                                   : draw.index_count;
  if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
    Fail("primitive type " + std::to_string(draw.primitive_type) + " needs expansion (not yet)");
    return;
  }
  if (draw.source == DrawSource::kImmediate) {
    Fail("immediate indices");
    return;
  }
  const bool polygonal = IsPolygonal(topology);
  // Primitive reset (PA_SU_SC_MODE_CNTL multi_prim_ib_ena, VGT_MULTI_PRIM_IB_RESET_INDX): strips of DMA indices are
  // cut at the reset index, as the reference does (16-bit indices only when the reset index fits in 16 bits).
  const uint32_t reset_index = regs[kVgtMultiPrimIbResetIndx];
  const bool strip = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                     topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  const bool restart = rectangle_list || (draw.source == DrawSource::kDma && strip && ((regs[kPaSuScModeCntl] >> 21) & 1) &&
                                          (draw.index_32 || reset_index <= UINT16_MAX));

  TranslatedShader* vs = GetShader(*draw.vertex_shader);
  const bool wants_ps = edram_mode == 4 && draw.pixel_shader && !draw.pixel_shader->microcode.empty();
  TranslatedShader* ps = wants_ps ? GetShader(*draw.pixel_shader) : nullptr;
  if (!vs || (wants_ps && !ps)) {
    Fail("shader analysis");
    return;
  }
  // The vertex buffers the vertex shader fetches from (vertex fetch constants: address in dword 0, size in words in
  // dword 1, and as far again as VGT_INDX_OFFSET moves the indices), for the copy into the mirror when guest memory
  // is not read in place.
  if (!shared_memory_imported_) {
    const uint64_t index_offset = regs[kVgtIndxOffset] & 0xFFFFFF;
    for (const auto& binding : vs->shader->vertex_bindings()) {
      const uint32_t* fetch = regs + reg::kFetchConstants + 2 * binding.fetch_constant;
      const uint64_t bytes = uint64_t((fetch[1] >> 2) & 0xFFFFFF) * 4 + index_offset * binding.stride_words * 4;
      ReadsGuestMemory(fetch[0] & 0x1FFFFFFC, uint32_t(std::min<uint64_t>(bytes, mirror_bytes_)));
    }
  }
  // The translations for this draw: the interpolators the vertex shader writes and the pixel shader reads
  // (SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC), and the register counts dynamic addressing may reach.
  rg::reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = regs[kSqProgramCntl];
  rg::reg::SQ_CONTEXT_MISC context_misc;
  context_misc.value = regs[kSqContextMisc];
  uint32_t interpolators = 0;
  // Pixel parameter generation (SQ_PROGRAM_CNTL gen_index_pix): the pixel shader reads, in one of its interpolators,
  // the pixel position, whether the primitive is back-facing, a line or a point, and the point coordinates. The
  // translator makes it from the fragment coordinates and the primitive flags of the system constants; points would
  // need their coordinates from a point sprite expansion.
  uint32_t param_gen = UINT32_MAX;
  if (ps) {
    interpolators = vs->shader->writes_interpolators() & ps->shader->GetInterpolatorInputMask(program_cntl, context_misc, param_gen);
    if (param_gen != UINT32_MAX && topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) {
      Fail("point sprites with pixel parameter generation (not yet)");
      return;
    }
  }
  SpirvShaderTranslator::Modification vs_modification(translator_->GetDefaultVertexShaderModification(
      vs->shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rectangle_list ? rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip : rg::Shader::HostVertexShaderType::kVertex));
  vs_modification.vertex.interpolator_mask = interpolators;
  const VkShaderModule vs_module = GetModule(*vs, vs_modification.value);
  VkShaderModule ps_module = VK_NULL_HANDLE;
  if (ps) {
    SpirvShaderTranslator::Modification ps_modification(
        translator_->GetDefaultPixelShaderModification(ps->shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    ps_modification.pixel.interpolator_mask = interpolators;
    if (param_gen < 16) {
      ps_modification.pixel.param_gen_enable = 1;
      ps_modification.pixel.param_gen_interpolator = param_gen;
      ps_modification.pixel.param_gen_point = 0;
    }
    ps_module = GetModule(*ps, ps_modification.value);
  }
  if (!vs_module || (ps && !ps_module)) {
    Fail("shader translation");
    return;
  }

  // Textures and samplers of each stage, in the order the translated shader binds them.
  struct StageTextures {
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
  } stage_textures[2];
  uint32_t swizzled_signs[8] = {};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    TranslatedShader* shader = stage ? ps : vs;
    if (!shader) continue;
    for (const auto& binding : shader->shader->GetTextureBindingsAfterTranslation()) {
      // The translator declares an unsigned and a signed binding for a fetch and picks one per component by the
      // fetch constant's signs at run time: the signed binding gets the texture's signed view when a component is
      // signed (sign 1), else the unsigned view (never sampled then).
      const uint32_t* fetch = regs + reg::kFetchConstants + 6 * binding.fetch_constant;
      const uint32_t fetch_signs = (fetch[0] >> 2) & 0xFF;
      const bool signed_view = binding.is_signed && ((fetch_signs & 0x55) & ~(fetch_signs >> 1)) != 0;
      std::string error;
      const TextureCache::ViewType view_type =
          binding.dimension == rg::xenos::FetchOpDimension::kCube           ? TextureCache::ViewType::kCube
          : binding.dimension == rg::xenos::FetchOpDimension::k3DOrStacked ? TextureCache::ViewType::k3D
                                                                           : TextureCache::ViewType::k2DArray;
      VkImageView view;
      {
        Timer texture_timer(stats_.ns_textures);
        view = textures_->View(fetch, view_type, error, signed_view);
      }
      if (!view) {
        Fail("texture " + std::to_string(binding.fetch_constant) + ": " + error);
        return;
      }
      stage_textures[stage].views.push_back(view);
      // The signs the shader applies after the swizzle (unsigned is 0), a byte per fetch constant.
      const TextureFetch t = TextureFetch::Parse(fetch);
      uint32_t signs = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t component = (t.swizzle >> (3 * i)) & 7;
        if (component < 4) signs |= ((t.signs >> (2 * component)) & 3) << (2 * i);
      }
      swizzled_signs[binding.fetch_constant / 4] |= signs << (8 * (binding.fetch_constant % 4));
    }
    for (const auto& binding : shader->shader->GetSamplerBindingsAfterTranslation()) {
      const VkSampler sampler = textures_->Sampler(regs + reg::kFetchConstants + 6 * binding.fetch_constant,
                                                   uint32_t(binding.mag_filter), uint32_t(binding.min_filter),
                                                   uint32_t(binding.mip_filter), uint32_t(binding.aniso_filter));
      if (!sampler) {
        Fail("sampler");
        return;
      }
      stage_textures[stage].samplers.push_back(sampler);
    }
  }
  stats_.texture_uploads = textures_->uploads();
  stats_.texture_gpu_copies = textures_->gpu_copies();
  stats_.ns_texture_hash = textures_->ns_hash();
  stats_.ns_texture_load = textures_->ns_load();
  const VkPipelineLayout pipeline_layout =
      PipelineLayout(uint32_t(stage_textures[0].views.size()), uint32_t(stage_textures[0].samplers.size()),
                     uint32_t(stage_textures[1].views.size()), uint32_t(stage_textures[1].samplers.size()));
  if (!pipeline_layout) {
    Fail("pipeline layout");
    return;
  }

  // The surfaces: colour targets the pixel shader writes and RB_COLOR_MASK enables, the depth/stencil target when
  // the depth or stencil test is on; all at the pitch of RB_SURFACE_INFO.
  // An MSAA surface is drawn as its samples: EDRAM holds a 4x surface of pitch P like a 1x surface of pitch 2P
  // with twice the rows (2x: the same pitch, twice the rows), so the title's MSAA draws (its fast clears among them)
  // reach the same host image as its 1x draws to that EDRAM. Coordinates are scaled to samples; resolves pick or
  // average a pixel's samples.
  const uint32_t surface_info = regs[kRbSurfaceInfo];
  const uint32_t msaa = (surface_info >> 16) & 3;
  if (msaa > 2) {
    Fail("MSAA mode 3");
    return;
  }
  const int32_t sample_x = msaa == 2 ? 2 : 1, sample_y = msaa ? 2 : 1;
  const uint32_t pitch = (surface_info & 0x3FFF) * uint32_t(sample_x);
  uint32_t depth_control = regs[kRbDepthControl];
  // Depth tested always without writes does nothing (the reference drops it the same way).
  if ((depth_control & 2) && !(depth_control & 4) && ((depth_control >> 4) & 7) == 7) depth_control &= ~2u;
  if (!polygonal) depth_control &= ~(1u << 7);  // back-face stencil only for polygons
  Surface* colors[4] = {};
  uint32_t color_infos[4] = {};
  Surface* depth_surface = nullptr;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  const uint32_t color_mask_register = regs[kRbColorMask];
  for (uint32_t i = 0; i < 4; ++i) {
    color_infos[i] = regs[kRbColorInfo[i]];
    if (!ps || !ps->shader->writes_color_target(i) || !((color_mask_register >> (4 * i)) & 0xF)) continue;
    colors[i] = GetSurface({color_infos[i] & 0xFFF, pitch, (color_infos[i] >> 16) & 0xF, 0});
    if (!colors[i]) {
      Fail("colour target " + std::to_string(i) + " format " + std::to_string((color_infos[i] >> 16) & 0xF));
      return;
    }
    width = std::min(width, colors[i]->image.width);
    height = std::min(height, colors[i]->image.height);
  }
  if (depth_control & 3) {
    const uint32_t depth_info = regs[kRbDepthInfo];
    depth_surface = GetSurface({depth_info & 0xFFF, pitch, (depth_info >> 16) & 1, 1});
    if (!depth_surface) {
      Fail("depth target");
      return;
    }
    width = std::min(width, depth_surface->image.width);
    height = std::min(height, depth_surface->image.height);
  } else {
    depth_control = 0;
  }
  if (width == UINT32_MAX) {
    ++stats_.skipped;  // nothing bound: no target this draw could change
    return;
  }

  // The scissor: the window scissor (moved by the window offset unless it says not to) within the screen scissor
  // and the targets.
  const uint32_t window_offset = regs[kPaScWindowOffset];
  const int32_t offset_x = int32_t(window_offset << 17) >> 17, offset_y = int32_t(window_offset << 1) >> 17;
  const uint32_t window_tl = regs[kPaScWindowScissorTl], window_br = regs[kPaScWindowScissorBr];
  const bool offset_scissor = !(window_tl >> 31);
  int32_t left = int32_t(window_tl & 0x3FFF) + (offset_scissor ? offset_x : 0);
  int32_t top = int32_t((window_tl >> 16) & 0x3FFF) + (offset_scissor ? offset_y : 0);
  int32_t right = int32_t(window_br & 0x3FFF) + (offset_scissor ? offset_x : 0);
  int32_t bottom = int32_t((window_br >> 16) & 0x3FFF) + (offset_scissor ? offset_y : 0);
  left = std::max({left, int32_t(regs[kPaScScreenScissorTl] & 0x7FFF), 0}) * sample_x;
  top = std::max({top, int32_t((regs[kPaScScreenScissorTl] >> 16) & 0x7FFF), 0}) * sample_y;
  right = std::min(std::min(right, int32_t(regs[kPaScScreenScissorBr] & 0x7FFF)) * sample_x, int32_t(width));
  bottom = std::min(std::min(bottom, int32_t((regs[kPaScScreenScissorBr] >> 16) & 0x7FFF)) * sample_y, int32_t(height));
  if (right <= left || bottom <= top) {
    ++stats_.skipped;
    return;
  }

  // The pipeline state from the registers.
  const uint32_t mode_cntl = regs[kPaSuScModeCntl];
  PipelineKey key{};
  key.vs_module = uint64_t(vs_module);
  key.layout = uint64_t(pipeline_layout);
  key.ps_module = uint64_t(ps_module);
  key.topology = topology;
  key.primitive_restart = restart ? 1 : 0;
  key.polygon_mode = VK_POLYGON_MODE_FILL;
  if (polygonal) {
    key.cull_mode = (mode_cntl & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (mode_cntl & 2 ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_clockwise = (mode_cntl >> 2) & 1;
    // Dual mode with a point or line fill on a face that is drawn: wireframe (as the reference does for both).
    if (((mode_cntl >> 3) & 3) == 1) {
      uint32_t type = 2;
      if (!(mode_cntl & 1)) type = std::min(type, (mode_cntl >> 5) & 7);
      if (!(mode_cntl & 2)) type = std::min(type, (mode_cntl >> 8) & 7);
      if (type < 2) key.polygon_mode = VK_POLYGON_MODE_LINE;
    }
  }
  key.depth_control = depth_control;
  key.depth_format = depth_surface ? depth_format_ : VK_FORMAT_UNDEFINED;
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED;
    key.blend_controls[i] = colors[i] ? (regs[kRbBlendControl[i]] & 0x1FFF1FFF) : kNoBlending;
    if (colors[i]) key.color_masks |= color_mask_register & (0xFu << (4 * i));
  }
  VkPipeline pipeline;
  {
    Timer pipeline_timer(stats_.ns_pipelines);
    pipeline = GetPipeline(key);
  }
  if (!pipeline) {
    Fail("pipeline creation");
    return;
  }

  // Constants, each at an aligned offset of the batch's ring: system, VS floats, PS floats, bool/loop, fetch. A draw
  // takes at most this much, and every range the descriptors read (up to 4 KB) must stay inside the buffer.
  const uint32_t most = uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)) + 2 * kFloatBufferBytes + kBoolLoopBytes +
                        kFetchBytes + 5 * constant_alignment_ + kFloatBufferBytes;
  // Everything this draw takes from the batch, checked up front: a flush once its constants are written would let
  // later draws overwrite them.
  const uint32_t index_need = rectangle_list || quad_list       ? host_index_count * 4 + 4
                              : draw.source == DrawSource::kDma ? draw.index_bytes + 4
                                                                : 0;
  const uint32_t images_need = uint32_t(stage_textures[0].views.size() + stage_textures[1].views.size());
  const uint32_t samplers_need = uint32_t(stage_textures[0].samplers.size() + stage_textures[1].samplers.size());
  if (constant_head_ + most > constants_.size || index_head_ + index_need > indices_.size || draw_sets_ + 2 > kDrawSets ||
      draw_images_ + images_need > kDrawDescriptors || draw_samplers_ + samplers_need > kDrawDescriptors) {
    Flush();
  }
  draw_images_ += images_need;
  draw_samplers_ += samplers_need;
  uint8_t* base = static_cast<uint8_t*>(constants_.mapped);
  uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
  uint32_t& at = constant_head_;
  auto reserve = [&](uint32_t index, uint32_t bytes) {
    offsets[index] = at;
    std::memset(base + at, 0, bytes);
    at = AlignUp(at + bytes, constant_alignment_);
    return base + offsets[index];
  };
  // System constants: the guest viewport transform (PA_CL_VTE_CNTL, PA_CL_VPORT_*) folded into the NDC scale and
  // offset for a host viewport covering the target.
  auto* system = reinterpret_cast<SpirvShaderTranslator::SystemConstants*>(
      reserve(SpirvShaderTranslator::kConstantBufferSystem, sizeof(SpirvShaderTranslator::SystemConstants)));
  const uint32_t vte = regs[kPaClVteCntl];
  if (vte & (1u << 8)) system->flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (vte & (1u << 9)) system->flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (vte & (1u << 10)) system->flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  // The primitive class, for pixel parameter generation (back-facing only means something for polygons).
  if (polygonal) system->flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) {
    system->flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  const float size[2] = {float(width), float(height)};
  // The window offset moves vertices too when PA_SU_SC_MODE_CNTL asks for it.
  // D3D's pixel centres (PA_SU_VTX_CNTL pix_center 0) are at integer window coordinates, the host's at .5: the
  // reference moves the window coordinates by half a pixel then.
  const float half_pixel = (regs[kPaSuVtxCntl] & 1) ? 0.0f : 0.5f;
  const float window_shift[2] = {(mode_cntl >> 16) & 1 ? float(offset_x) : 0.0f, (mode_cntl >> 16) & 1 ? float(offset_y) : 0.0f};
  for (int i = 0; i < 3; ++i) {
    // Guest window coordinate = position * scale + offset (pixels for x and y, depth for z).
    const float scale = (vte & (1u << (2 * i))) ? AsFloat(regs[kPaClVportXScale + 2 * i]) : 1.0f;
    const float offset = (vte & (1u << (2 * i + 1))) ? AsFloat(regs[kPaClVportXScale + 2 * i + 1]) : 0.0f;
    if (i < 2) {
      const float samples = float(i ? sample_y : sample_x);  // guest pixels to surface samples
      system->ndc_scale[i] = scale * samples * 2.0f / size[i];
      system->ndc_offset[i] = (offset + window_shift[i] + half_pixel) * samples * 2.0f / size[i] - 1.0f;
    } else {
      system->ndc_scale[i] = scale;
      system->ndc_offset[i] = offset;
    }
  }
  // Index offset and clamp (VGT_INDX_OFFSET, VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX), applied by the vertex shader.
  system->vertex_base_index = int32_t(regs[kVgtIndxOffset]);
  system->vertex_index_min = regs[kVgtMinVtxIndx];
  system->vertex_index_max = regs[kVgtMaxVtxIndx];
  // A rectangle list's vertex shader loads its guest indices itself (from the strip vertex it runs for).
  if (rectangle_list && draw.source == DrawSource::kDma) {
    system->flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (draw.index_32) system->flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    system->vertex_index_load_address = draw.index_base;
    ReadsGuestMemory(draw.index_base, draw.index_count * (draw.index_32 ? 4 : 2));
    // 16-bit indices swap within 16 bits (as the reference normalizes 32-bit swap modes for them).
    Endian endian = draw.index_endian;
    if (!draw.index_32) endian = endian == Endian::k8in32 ? Endian::k8in16 : endian == Endian::k16in32 ? Endian::kNone : endian;
    system->vertex_index_endian = decltype(system->vertex_index_endian)(uint32_t(endian));
  }
  std::memcpy(system->texture_swizzled_signs, swizzled_signs, sizeof(swizzled_signs));
  // Alpha test (RB_COLORCONTROL): the compare function's less / equal / greater bits are the pass flags; a disabled
  // test is "always" (with none of them set, every pixel would be discarded).
  const uint32_t color_control = regs[kRbColorControl];
  const uint32_t alpha_func = (color_control & (1u << 3)) ? (color_control & 7) : 7;
  system->flags |= alpha_func << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Per colour target: the exponent bias of RB_COLOR_INFO as a 2^bias multiplier (the fixed-point 16-bit formats'
  // -32...32 range scaled into the host's -1...1 by 2^-5 more, as the reference does), and the gamma conversion
  // of k_8_8_8_8_GAMMA targets.
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t bias = int32_t(color_infos[i] << 6) >> 26;  // bits 20-25, signed
    const uint32_t format = (color_infos[i] >> 16) & 0xF;
    if (format == 4 || format == 5) bias -= 5;
    system->color_exp_bias[i] = std::bit_cast<float>(uint32_t(int32_t(0x3F800000) + bias * (1 << 23)));
    if (colors[i] && format == 1) system->flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
  }
  // Float constants, packed as each shader uses them (all 256 when it addresses them dynamically).
  auto pack_floats = [&](uint32_t index, const TranslatedShader* shader, uint32_t first_register) {
    // Only the constants the shader uses take room (the descriptor's 4 KB range reads past them harmlessly).
    const auto& map = shader ? shader->shader->constant_register_map() : vs->shader->constant_register_map();
    const uint32_t count = !shader ? 1 : map.float_dynamic_addressing ? 256 : std::max(1u, map.float_count);
    uint8_t* out = reserve(index, 16 * count);
    if (!shader) return;
    uint32_t packed = 0;
    for (uint32_t c = 0; c < 256; ++c) {
      if (!map.float_dynamic_addressing && !(map.float_bitmap[c / 64] & (uint64_t(1) << (c % 64)))) continue;
      std::memcpy(out + 16 * packed++, regs + first_register + 4 * c, 16);
    }
  };
  pack_floats(SpirvShaderTranslator::kConstantBufferFloatVertex, vs, 0x4000);
  pack_floats(SpirvShaderTranslator::kConstantBufferFloatPixel, ps, 0x4000 + 256 * 4);
  std::memcpy(reserve(SpirvShaderTranslator::kConstantBufferBoolLoop, kBoolLoopBytes), regs + 0x4900, kBoolLoopBytes);
  std::memcpy(reserve(SpirvShaderTranslator::kConstantBufferFetch, kFetchBytes), regs + 0x4800, kFetchBytes);

  // Dynamic state: polygon offset (scaled to the depth format's unit), blend constants, stencil masks and refs.
  float bias_scale = 0.0f, bias_offset = 0.0f;
  const float* poly_offset = reinterpret_cast<const float*>(regs + kPaSuPolyOffsetFrontScale);
  if (polygonal) {
    if (((mode_cntl >> 11) & 1) && !(mode_cntl & 1)) bias_scale = poly_offset[0], bias_offset = poly_offset[1];
    if (((mode_cntl >> 12) & 1) && !(mode_cntl & 2) && !bias_scale && !bias_offset) {
      bias_scale = poly_offset[2], bias_offset = poly_offset[3];
    }
  } else if ((mode_cntl >> 13) & 1) {
    bias_scale = poly_offset[0], bias_offset = poly_offset[1];
  }
  bias_offset *= ((regs[kRbDepthInfo] >> 16) & 1) ? kPolygonOffsetFloat24 : kPolygonOffsetUnorm24;
  bias_scale *= 1.0f / 16.0f;  // guest slope unit: 1/16 of a pixel
  const float* blend_constants = reinterpret_cast<const float*>(regs + kRbBlendRed);
  const uint32_t stencil_front = regs[kRbStencilRefMask];
  const uint32_t stencil_back = (depth_control >> 7) & 1 ? regs[kRbStencilRefMaskBf] : stencil_front;

  // The indices (endian-swapped to host order) into the batch's ring.
  VkIndexType index_type = VK_INDEX_TYPE_NONE_KHR;
  VkDeviceSize index_offset = 0;
  if (rectangle_list || quad_list) {
    if (draw.source == DrawSource::kDma && uint64_t(draw.index_base) + draw.index_bytes > mirror_bytes_) {
      Fail("index buffer outside the mirrored memory");
      return;
    }
    index_type = VK_INDEX_TYPE_UINT32;
    index_offset = index_head_;
    index_head_ = AlignUp(index_head_ + host_index_count * 4, 4);
    uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(indices_.mapped) + index_offset);
    if (rectangle_list) {
      // Four strip vertices a rectangle, a restart between rectangles.
      for (uint32_t r = 0; r < primitives; ++r) {
        if (r) *dst++ = UINT32_MAX;
        for (uint32_t i = 0; i < 4; ++i) *dst++ = 4 * r + i;
      }
    } else {
      auto guest_index = [&](uint32_t i) -> uint32_t {
        if (draw.source != DrawSource::kDma) return i;
        const uint8_t* src = physical_ + draw.index_base;
        if (draw.index_32) {
          uint32_t v;
          std::memcpy(&v, src + 4 * i, 4);
          return GpuSwap(v, draw.index_endian);
        }
        uint16_t v;
        std::memcpy(&v, src + 2 * i, 2);
        if (draw.index_endian == Endian::k8in16 || draw.index_endian == Endian::k8in32) v = uint16_t(v << 8 | v >> 8);
        return v;
      };
      for (uint32_t q = 0; q < primitives; ++q) {
        const uint32_t a = guest_index(4 * q), b = guest_index(4 * q + 1), cc = guest_index(4 * q + 2), d = guest_index(4 * q + 3);
        const uint32_t six[6] = {a, b, cc, a, cc, d};
        std::memcpy(dst, six, sizeof(six));
        dst += 6;
      }
    }
  } else if (draw.source == DrawSource::kDma) {
    if (uint64_t(draw.index_base) + draw.index_bytes > mirror_bytes_ || draw.index_bytes > indices_.size) {
      Fail("index buffer outside the mirrored memory");
      return;
    }
    index_offset = index_head_;
    index_head_ = AlignUp(index_head_ + draw.index_bytes, 4);
    const uint8_t* src = physical_ + draw.index_base;
    uint8_t* dst = static_cast<uint8_t*>(indices_.mapped) + index_offset;
    // With primitive reset, the guest's reset index becomes the host's (all ones).
    if (draw.index_32) {
      index_type = VK_INDEX_TYPE_UINT32;
      for (uint32_t i = 0; i < draw.index_bytes / 4; ++i) {
        uint32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        v = GpuSwap(v, draw.index_endian);
        if (restart && v == reset_index) v = UINT32_MAX;
        std::memcpy(dst + 4 * i, &v, 4);
      }
    } else {
      index_type = VK_INDEX_TYPE_UINT16;
      for (uint32_t i = 0; i < draw.index_bytes / 2; ++i) {
        uint16_t v;
        std::memcpy(&v, src + 2 * i, 2);
        if (draw.index_endian == Endian::k8in16 || draw.index_endian == Endian::k8in32) v = uint16_t(v << 8 | v >> 8);
        if (restart && v == reset_index) v = UINT16_MAX;
        std::memcpy(dst + 2 * i, &v, 2);
      }
    }
  }

  // The texture sets of both stages: sampled images, then samplers (empty sets for a stage without textures).
  VkDescriptorSet texture_sets[2] = {empty_set_, empty_set_};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const StageTextures& st = stage_textures[stage];
    if (st.views.empty() && st.samplers.empty()) continue;
    VkDescriptorSetLayout layout = TextureSetLayout(stage == 0, uint32_t(st.views.size()), uint32_t(st.samplers.size()));
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = draw_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    if (f.vkAllocateDescriptorSets(device_.device(), &alloc, &texture_sets[stage]) != VK_SUCCESS) {
      Fail("texture descriptor set");
      return;
    }
    ++draw_sets_;
    std::vector<VkDescriptorImageInfo> infos(st.views.size() + st.samplers.size());
    std::vector<VkWriteDescriptorSet> writes(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
      const bool is_image = i < st.views.size();
      infos[i] = is_image ? VkDescriptorImageInfo{VK_NULL_HANDLE, st.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                          : VkDescriptorImageInfo{st.samplers[i - st.views.size()], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, texture_sets[stage], uint32_t(i), 0, 1,
                   is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, &infos[i], nullptr, nullptr};
    }
    f.vkUpdateDescriptorSets(device_.device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  }

  {
    Timer submit_timer(stats_.ns_submit);
    const VkCommandBuffer cb = Record();
    // The render pass: kept open while this draw's targets are the open pass's; otherwise ended, the targets moved
    // to attachment layouts (outside any pass) and a new one begun.
    const std::array<uint64_t, 7> key = {
        colors[0] ? uint64_t(colors[0]->image.view) : 0, colors[1] ? uint64_t(colors[1]->image.view) : 0,
        colors[2] ? uint64_t(colors[2]->image.view) : 0, colors[3] ? uint64_t(colors[3]->image.view) : 0,
        depth_surface ? uint64_t(depth_surface->image.view) : 0, width, height};
    bool layouts_ready = true;
    for (uint32_t i = 0; i < 4; ++i) layouts_ready &= !colors[i] || colors[i]->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    layouts_ready &= !depth_surface || depth_surface->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (rendering_open_ && (key != rendering_key_ || !layouts_ready)) EndRendering();
    if (!rendering_open_) {
      VkRenderingAttachmentInfo color[4];
      for (uint32_t i = 0; i < 4; ++i) {
        color[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!colors[i]) continue;  // no view: the location is unused
        Transition(cb, colors[i]->image, colors[i]->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        color[i].imageView = colors[i]->image.view;
        color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if (depth_surface) {
        Transition(cb, depth_surface->image, depth_surface->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth.imageView = depth_surface->image.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea = {{0, 0}, {width, height}};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = 4;
      rendering.pColorAttachments = color;
      rendering.pDepthAttachment = depth_surface ? &depth : nullptr;
      rendering.pStencilAttachment = depth_surface ? &depth : nullptr;
      f.vkCmdBeginRendering(cb, &rendering);
      rendering_open_ = true;
      rendering_key_ = key;
    }
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount];
    sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_set_;
    sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_set_;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = texture_sets[0];
    sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = texture_sets[1];
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, SpirvShaderTranslator::kDescriptorSetCount,
                              sets, SpirvShaderTranslator::kConstantBufferCount, offsets);
    const VkViewport viewport{0, 0, size[0], size[1], 0, 1};
    const VkRect2D scissor{{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
    f.vkCmdSetViewport(cb, 0, 1, &viewport);
    f.vkCmdSetScissor(cb, 0, 1, &scissor);
    f.vkCmdSetDepthBias(cb, bias_offset, 0.0f, bias_scale);
    f.vkCmdSetBlendConstants(cb, blend_constants);
    f.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (stencil_front >> 8) & 0xFF);
    f.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (stencil_back >> 8) & 0xFF);
    f.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (stencil_front >> 16) & 0xFF);
    f.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (stencil_back >> 16) & 0xFF);
    f.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_front & 0xFF);
    f.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, stencil_back & 0xFF);
    if (index_type != VK_INDEX_TYPE_NONE_KHR) {
      f.vkCmdBindIndexBuffer(cb, indices_.buffer, index_offset, index_type);
      f.vkCmdDrawIndexed(cb, host_index_count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(cb, host_index_count, 1, 0, 0);
    }
  }
  ++stats_.draws;
}

VkCommandBuffer VulkanRenderer::Record() {
  if (!batch_open_) {
    const Functions& f = device_.fn();
    f.vkResetCommandBuffer(batch_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(batch_, &begin);
    batch_open_ = true;
  }
  return batch_;
}

void VulkanRenderer::EndRendering() {
  if (!rendering_open_) return;
  device_.fn().vkCmdEndRendering(batch_);
  rendering_open_ = false;
}

void VulkanRenderer::Flush() {
  if (!batch_open_) return;
  Timer submit_timer(stats_.ns_submit);
  const Functions& f = device_.fn();
  EndRendering();
  f.vkEndCommandBuffer(batch_);
  // Guest memory the batch's shaders fetch from: read in place, or copied once here (the draws run now, so they
  // see memory as it is now either way).
  if (!shared_memory_imported_) {
    // Only what the batch reads: copying all of guest memory at every submission (several a frame: each resolve
    // submits) costs far more than the draws.
    Timer memory_timer(stats_.ns_memory);
    std::sort(batch_ranges_.begin(), batch_ranges_.end());
    uint32_t copied_to = 0;
    for (auto [begin, end] : batch_ranges_) {
      begin = std::max(begin, copied_to);
      if (begin >= end) continue;
      std::memcpy(static_cast<uint8_t*>(shared_memory_.mapped) + begin, physical_ + begin, end - begin);
      copied_to = end;
    }
    batch_ranges_.clear();
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &batch_;
  f.vkResetFences(device_.device(), 1, &batch_fence_);
  if (f.vkQueueSubmit(device_.queue(), 1, &submit, batch_fence_) != VK_SUCCESS) {
    Fail("batch submission");
  } else {
    f.vkWaitForFences(device_.device(), 1, &batch_fence_, VK_TRUE, UINT64_MAX);
  }
  batch_open_ = false;
  FinishWriteBacks();
  // Everything the batch used is free again.
  if (textures_) textures_->BatchDone();
  if (depth_pool_ && depth_sets_) {
    f.vkResetDescriptorPool(device_.device(), depth_pool_, 0);
    depth_sets_ = 0;
  }
  constant_head_ = index_head_ = 0;
  draw_sets_ = draw_images_ = draw_samplers_ = 0;
  f.vkResetDescriptorPool(device_.device(), draw_pool_, 0);
  ++stats_.flushes;
}

namespace {
// Resolve registers.
constexpr uint32_t kRbCopyControl = 0x2318, kRbCopyDestBase = 0x2319, kRbCopyDestPitch = 0x231A, kRbCopyDestInfo = 0x231B,
                   kRbDepthClear = 0x231D, kRbColorClear = 0x231E, kRbColorClearLo = 0x231F;

uint16_t FloatToHalf(float f) {
  // Round-to-nearest-even IEEE half (finite values; the resolve path carries what the target holds).
  const uint32_t b = std::bit_cast<uint32_t>(f);
  const uint32_t sign = (b >> 16) & 0x8000;
  int32_t exponent = int32_t((b >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = b & 0x7FFFFF;
  if (exponent <= 0) return uint16_t(sign);
  if (exponent >= 31) return uint16_t(sign | 0x7C00);
  uint32_t half = sign | uint32_t(exponent) << 10 | mantissa >> 13;
  const uint32_t rest = mantissa & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;
  return uint16_t(half);
}
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  if (!exponent) return std::bit_cast<float>(sign) + (sign ? -1.0f : 1.0f) * float(mantissa) / 16777216.0f;
  if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000 | mantissa << 13);
  return std::bit_cast<float>(sign | (exponent - 15 + 127) << 23 | mantissa << 13);
}
// Endian128 of RB_COPY_DEST_INFO on a 32- or 64-bit element (as little-endian bytes in `bytes`).
void SwapElement(uint8_t* bytes, uint32_t size, uint32_t endian) {
  if (endian >= 1 && endian <= 3) {
    for (uint32_t i = 0; i < size; i += 4) {
      uint32_t v;
      std::memcpy(&v, bytes + i, 4);
      v = GpuSwap(v, Endian(endian));
      std::memcpy(bytes + i, &v, 4);
    }
  } else if (endian == 4 && size == 8) {  // 8 in 64
    std::reverse(bytes, bytes + 8);
  }
}
}  // namespace

bool VulkanRenderer::ResolveDepth(Surface& surface, bool float24, int32_t x0, int32_t y0, uint32_t width, uint32_t height,
                                  uint32_t sample_x, uint32_t sample_y, uint32_t sample, uint32_t dest_base,
                                  uint32_t dest_pitch, uint32_t dest_endian, uint32_t dest_format) {
  // The EDRAM depth word as the title reads it back: depth (24-bit unorm, or float24 for D24FS8) << 8 | stencil,
  // into a 32-bit destination format (k_8_8_8_8, k_24_8, k_24_8_FLOAT, k_32_FLOAT), endian-swapped and tiled.
  if (dest_format != 6 && dest_format != 22 && dest_format != 23 && dest_format != 36) {
    return Fail("depth resolve to format " + std::to_string(dest_format) + " (not yet)");
  }
  const Functions& f = device_.fn();
  // The samples of the rectangle are read back; the sample's position in its pixel: bit 0 down, bit 1 right.
  const uint32_t samples_wide = width * sample_x, samples_high = height * sample_y;
  const uint32_t offset_x = sample >> 1, offset_y = sample & 1;
  const VkDeviceSize depth_bytes = VkDeviceSize(samples_wide) * samples_high * 4;
  Buffer readback;
  if (!CreateBuffer(device_, depth_bytes + VkDeviceSize(samples_wide) * samples_high, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true, readback)) {
    return Fail("resolve readback buffer");
  }
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    Transition(cb, surface.image, surface.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkBufferImageCopy regions[2]{};
    regions[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    regions[0].imageOffset = {x0 * int32_t(sample_x), y0 * int32_t(sample_y), 0};
    regions[0].imageExtent = {samples_wide, samples_high, 1};
    regions[1] = regions[0];
    regions[1].bufferOffset = depth_bytes;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    f.vkCmdCopyImageToBuffer(cb, surface.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 2, regions);
  });
  if (ok) {
    const uint8_t* depth = static_cast<const uint8_t*>(readback.mapped);
    const uint8_t* stencil = depth + depth_bytes;
    // D24_UNORM_S8_UINT reads back as 32-bit words with the depth in the low 24 bits; D32_SFLOAT_S8_UINT as floats.
    const bool unorm24 = surface.image.format == VK_FORMAT_D24_UNORM_S8_UINT;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t i = size_t(y * sample_y + offset_y) * samples_wide + x * sample_x + offset_x;
        uint32_t raw;
        std::memcpy(&raw, depth + i * 4, 4);
        const float value = unorm24 ? float(raw & 0xFFFFFF) / float(0xFFFFFF) : std::bit_cast<float>(raw);
        const uint32_t guest = float24 ? FloatTo20e4(value)
                                       : (unorm24 ? raw & 0xFFFFFF
                                                  : uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * float(0xFFFFFF))));
        const uint32_t word = GpuSwap(guest << 8 | stencil[i], Endian(dest_endian <= 3 ? dest_endian : 0));
        const uint32_t address =
            dest_base + uint32_t(TiledOffset2D(int32_t(uint32_t(x0) + x), int32_t(uint32_t(y0) + y), dest_pitch, 2));
        if (address + 4 <= mirror_bytes_) std::memcpy(physical_ + address, &word, 4);
      }
    }
  }
  DestroyBuffer(device_, readback);
  return ok ? true : Fail("depth resolve copy");
}

bool VulkanRenderer::ResolveOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height,
                                  uint32_t sample_x, uint32_t sample_y, uint32_t dest_base, uint32_t dest_pitch,
                                  uint32_t dest_endian, VkFormat format) {
  const Functions& f = device_.fn();
  const uint32_t pitch = (dest_pitch + 31) & ~31u, rows = (uint32_t(y0) + height + 31) & ~31u;
  Resolved& r = resolved_[dest_base];
  if (!r.image.image || r.image.format != format || r.image.width < pitch || r.image.height < rows) {
    if (r.image.image) {
      Flush();  // the batch may still read the old image
      DestroyImage(device_, r.image);
    }
    if (!CreateImage2D(device_, pitch, std::max(rows, 32u), format,
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, r.image)) {
      resolved_.erase(dest_base);
      return Fail("resolve image");
    }
    r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  // After the draws in the batch: the surface's samples over the rectangle into the destination's texels, one sample
  // copied as it is, or the samples averaged (a linear blit to half the size averages each 2x2 or 1x2 block).
  EndRendering();
  VkCommandBuffer cb = Record();
  Transition(cb, surface.image, surface.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageBlit region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {x0 * int32_t(sample_x), y0 * int32_t(sample_y), 0};
  region.srcOffsets[1] = {(x0 + int32_t(width)) * int32_t(sample_x), (y0 + int32_t(height)) * int32_t(sample_y), 1};
  region.dstOffsets[0] = {x0, y0, 0};
  region.dstOffsets[1] = {x0 + int32_t(width), y0 + int32_t(height), 1};
  f.vkCmdBlitImage(cb, surface.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   sample_x * sample_y > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  r.pitch = pitch, r.endian = dest_endian, r.x = uint32_t(x0), r.y = uint32_t(y0), r.width = width, r.height = height;
  r.stale = true;
  if (textures_) {
    textures_->SetGpuSource({r.image.image, r.image.format, dest_base, pitch, dest_endian, r.image.width, r.image.height,
                             ++resolve_generation_});
  }
  return true;
}

void VulkanRenderer::Copy(const uint32_t* regs) {
  Timer resolve_timer(stats_.ns_resolves);
  const Functions& f = device_.fn();
  const uint32_t copy_control = regs[kRbCopyControl];
  const uint32_t source = copy_control & 7, copy_command = (copy_control >> 20) & 3;
  const bool clear_color = (copy_control >> 8) & 1, clear_depth = (copy_control >> 9) & 1;
  if (copy_command > 1) {
    Fail("resolve copy command " + std::to_string(copy_command) + " (not yet)");
    return;
  }
  // An MSAA source is drawn as its samples (see Draw): the rectangle is in pixels, the surface in samples.
  const uint32_t surface_info = regs[kRbSurfaceInfo];
  const uint32_t msaa = (surface_info >> 16) & 3;
  if (msaa > 2) {
    Fail("MSAA mode 3");
    return;
  }
  const uint32_t sample_x = msaa == 2 ? 2 : 1, sample_y = msaa ? 2 : 1;
  const uint32_t pitch = surface_info & 0x3FFF;

  // The rectangle: the copy draw's three vertices (vertex fetch constant 0, float2 window coordinates), with the
  // D3D half-pixel offset, in 24.8 fixed point, within the scissor, aligned to 8 pixels (as the reference does).
  const uint32_t fetch0 = regs[reg::kFetchConstants], fetch1 = regs[reg::kFetchConstants + 1];
  if ((fetch0 & 3) != 3 || ((fetch1 >> 2) & 0xFFFFFF) != 6) {
    Fail("resolve vertex buffer format");
    return;
  }
  const uint32_t vertex_address = fetch0 & 0x1FFFFFFC;
  if (vertex_address + 24 > mirror_bytes_) {
    Fail("resolve vertices outside the mirrored memory");
    return;
  }
  const float half_pixel = ((regs[kPaSuVtxCntl] & 1) == 0) ? 0.5f : 0.0f;  // pix_center: D3D zero
  int32_t fixed[6];
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t bits;
    std::memcpy(&bits, physical_ + vertex_address + 4 * i, 4);
    const float value = std::bit_cast<float>(GpuSwap(bits, Endian(fetch1 & 3))) + half_pixel;
    fixed[i] = int32_t(std::lround(value * 256.0f));
  }
  int32_t x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8, y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  int32_t x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8, y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  const uint32_t window_offset = regs[kPaScWindowOffset];
  if ((regs[kPaSuScModeCntl] >> 16) & 1) {
    const int32_t ox = int32_t(window_offset << 17) >> 17, oy = int32_t(window_offset << 1) >> 17;
    x0 += ox, x1 += ox, y0 += oy, y1 += oy;
  }
  // The scissor without the window offset applied to it (as the reference reads it for resolves).
  const uint32_t window_tl = regs[kPaScWindowScissorTl], window_br = regs[kPaScWindowScissorBr];
  const int32_t sl = std::max(int32_t(window_tl & 0x3FFF), int32_t(regs[kPaScScreenScissorTl] & 0x7FFF));
  const int32_t st = std::max(int32_t((window_tl >> 16) & 0x3FFF), int32_t((regs[kPaScScreenScissorTl] >> 16) & 0x7FFF));
  const int32_t sr = std::min(int32_t(window_br & 0x3FFF), int32_t(regs[kPaScScreenScissorBr] & 0x7FFF));
  const int32_t sb = std::min(int32_t((window_br >> 16) & 0x3FFF), int32_t((regs[kPaScScreenScissorBr] >> 16) & 0x7FFF));
  x0 = std::clamp(x0, sl, std::max(sl, sr)), x1 = std::clamp(x1, sl, std::max(sl, sr));
  y0 = std::clamp(y0, st, std::max(st, sb)), y1 = std::clamp(y1, st, std::max(st, sb));
  x0 &= ~7, y0 &= ~7, x1 = (x1 + 7) & ~7, y1 = (y1 + 7) & ~7;
  x1 = std::min(x1, int32_t(pitch & ~7u));
  if (x0 >= x1 || y0 >= y1) {
    Fail("resolve region empty");
    return;
  }

  // The source surface and the targets the resolve clears.
  const bool is_depth = source >= 4;
  const uint32_t color_info = is_depth ? 0 : regs[kRbColorInfo[source]];
  const uint32_t depth_info = regs[kRbDepthInfo];
  const SurfaceKey source_key = is_depth ? SurfaceKey{depth_info & 0xFFF, pitch * sample_x, (depth_info >> 16) & 1, 1}
                                         : SurfaceKey{color_info & 0xFFF, pitch * sample_x, (color_info >> 16) & 0xF, 0};
  Surface* surface = GetSurface(source_key);
  if (!surface) {
    Fail("resolve source surface");
    return;
  }
  const uint32_t width = uint32_t(std::min<int32_t>(x1, int32_t(surface->image.width / sample_x)) - x0);
  const uint32_t height = uint32_t(std::min<int32_t>(y1, int32_t(surface->image.height / sample_y)) - y0);
  // The samples the copy takes of each pixel (RB_COPY_CONTROL copy_sample_select: 0-3 one sample, 4 samples 0 and 1,
  // 5 samples 2 and 3, 6 all four), made valid for the sample count as the reference does: depth is never averaged.
  // Samples are numbered top-left, bottom-left, top-right, bottom-right.
  uint32_t sample_select = (copy_control >> 4) & 7;
  if (msaa == 2) {
    sample_select = std::min(sample_select, 6u);
    if (is_depth) sample_select = sample_select == 4 || sample_select == 6 ? 0 : sample_select == 5 ? 2 : sample_select;
  } else if (msaa == 1) {
    sample_select = sample_select == 2 ? 0 : sample_select == 3 ? 1 : std::min(sample_select, 4u);
    if (is_depth && sample_select == 4) sample_select = 0;
  } else {
    sample_select = 0;
  }
  const uint32_t first_sample = sample_select < 4 ? sample_select : sample_select == 5 ? 2 : 0;
  const uint32_t sample_count = sample_select < 4 ? 1 : sample_select == 6 ? 4 : 2;

  // The copy: the region read back, converted, tiled into guest memory (destination texels at the source's
  // coordinates, at RB_COPY_DEST_BASE with RB_COPY_DEST_PITCH).
  const uint32_t dest_info = regs[kRbCopyDestInfo];
  const uint32_t dest_endian = dest_info & 7, dest_format = (dest_info >> 7) & 0x3F, dest_swap = (dest_info >> 24) & 1;
  const int32_t dest_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21, signed
  const uint32_t dest_base = regs[kRbCopyDestBase] & 0x1FFFFFFF, dest_pitch = regs[kRbCopyDestPitch] & 0x3FFF;
  if ((dest_info >> 3) & 1) {
    Fail("resolve to a 3D / array destination (not yet)");
    return;
  }
  // A colour resolve a texture can read as it is (no exponent bias, no red/blue swap, the target's own format, one
  // sample or all averaged) stays on the GPU, recorded in the batch after the draws it follows; only a small one is
  // also read back into guest memory for the CPU. Any other is read back (the batch runs first).
  const VkFormat host_format = surface->image.format;
  VkFormat gpu_format = VK_FORMAT_UNDEFINED;
  if (!is_depth && !dest_bias && (!msaa || sample_count == sample_x * sample_y)) {
    if (dest_format == 6 && host_format == VK_FORMAT_R8G8B8A8_UNORM) {
      gpu_format = dest_swap ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    } else if (!dest_swap && dest_format == 32 && host_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
      gpu_format = host_format;
    } else if (!dest_swap && dest_format == 31 && host_format == VK_FORMAT_R16G16_SFLOAT) {
      gpu_format = host_format;  // k_16_16_FLOAT
    }
  }
  // A depth resolve: one sample a pixel, the depth value for the depth texture formats, on the GPU too.
  const bool depth_on_gpu = is_depth && (dest_format == 6 || dest_format == 22 || dest_format == 23 || dest_format == 36);
  const bool on_gpu = gpu_format != VK_FORMAT_UNDEFINED || depth_on_gpu;
  {
    char path[192];
    std::snprintf(path, sizeof(path), "%s %s target format %u (host %d) to format %u%s%s%s, %ux%u",
                  on_gpu ? "GPU" : "CPU", is_depth ? "depth" : "colour", source_key.format, int(host_format), dest_format,
                  dest_bias ? ", exponent bias" : "", dest_swap ? ", red/blue swap" : "",
                  !msaa ? "" : sample_count == sample_x * sample_y ? ", samples averaged" : ", one sample", width, height);
    ++resolve_paths_[path];
  }
  if (depth_on_gpu && !ResolveDepthOnGpu(*surface, x0, y0, width, height, sample_x, sample_y, first_sample, dest_base,
                                         dest_pitch, dest_endian)) {
    return;
  }
  if (gpu_format != VK_FORMAT_UNDEFINED &&
      !ResolveOnGpu(*surface, x0, y0, width, height, sample_x, sample_y, dest_base, dest_pitch, dest_endian, gpu_format)) {
    return;
  }
  // Read back as well: what the CPU may read (small resolves) and front buffers (presented from guest memory), when
  // the CPU path has the format.
  const bool cpu_has_format = is_depth || (dest_format == 6 && host_format == VK_FORMAT_R8G8B8A8_UNORM) ||
                              (dest_format == 32 && host_format == VK_FORMAT_R16G16B16A16_SFLOAT);
  const bool read_back =
      !on_gpu || (cpu_has_format && (uint64_t(width) * height * (dest_format == 32 ? 8 : 4) <= kReadBackMaxBytes ||
                                     (front_buffers_.count(dest_base) && !front_on_gpu_.count(dest_base))));
  if (read_back && on_gpu) {
    // Its write into guest memory recorded in the batch, done when the batch has run (no wait here).
    QueueWriteBack(dest_base, resolved_[dest_base]);
  } else if (read_back) {
    Flush();  // the draws before the resolve must be in the surface
  }
  if (!on_gpu && textures_) textures_->DropGpuSources(dest_base, dest_base + 1);
  if (on_gpu) {
    // On the GPU (and queued for guest memory when read back).
  } else if (is_depth) {
    if (!ResolveDepth(*surface, source_key.format == 1, x0, y0, width, height, sample_x, sample_y, first_sample, dest_base,
                      dest_pitch, dest_endian, dest_format)) {
      return;
    }
  } else {
    const VkFormat host = surface->image.format;
    uint32_t dest_bytes = 0, source_bytes = 0;
    if (dest_format == 6 && host == VK_FORMAT_R8G8B8A8_UNORM) {
      dest_bytes = 4, source_bytes = 4;  // k_8_8_8_8 from an 8_8_8_8 target
    } else if (dest_format == 32 && host == VK_FORMAT_R16G16B16A16_SFLOAT) {
      dest_bytes = 8, source_bytes = 8;  // k_16_16_16_16_FLOAT from a 16_16_16_16_FLOAT target
    } else {
      Fail("resolve from host format " + std::to_string(host) + " to format " + std::to_string(dest_format) + " (not yet)");
      return;
    }
    const uint32_t samples_wide = width * sample_x, samples_high = height * sample_y;
    Buffer readback;
    if (!CreateBuffer(device_, VkDeviceSize(samples_wide) * samples_high * source_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      true, readback)) {
      Fail("resolve readback buffer");
      return;
    }
    bool ok = device_.Submit([&](VkCommandBuffer cb) {
      Transition(cb, surface->image, surface->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {x0 * int32_t(sample_x), y0 * int32_t(sample_y), 0};
      region.imageExtent = {samples_wide, samples_high, 1};
      f.vkCmdCopyImageToBuffer(cb, surface->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });
    std::vector<uint8_t> resolved;
    if (ok && sample_x * sample_y > 1) {
      // One texel a pixel: the selected sample, or the average of the selected samples (8-bit components rounded to
      // nearest, half floats averaged as floats).
      const uint8_t* samples = static_cast<const uint8_t*>(readback.mapped);
      resolved.resize(size_t(width) * height * source_bytes);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* taken[4];
          for (uint32_t s = 0; s < sample_count; ++s) {
            const uint32_t sample = first_sample + s;
            taken[s] = samples +
                       (size_t(y * sample_y + (sample & 1)) * samples_wide + x * sample_x + (sample >> 1)) * source_bytes;
          }
          uint8_t* out = resolved.data() + (size_t(y) * width + x) * source_bytes;
          if (source_bytes == 4) {
            for (uint32_t c = 0; c < 4; ++c) {
              uint32_t sum = 0;
              for (uint32_t s = 0; s < sample_count; ++s) sum += taken[s][c];
              out[c] = uint8_t((sum + sample_count / 2) / sample_count);
            }
          } else {
            for (uint32_t c = 0; c < 4; ++c) {
              float sum = 0.0f;
              for (uint32_t s = 0; s < sample_count; ++s) {
                uint16_t half;
                std::memcpy(&half, taken[s] + 2 * c, 2);
                sum += HalfToFloat(half);
              }
              const uint16_t half = FloatToHalf(sum / float(sample_count));
              std::memcpy(out + 2 * c, &half, 2);
            }
          }
        }
      }
    }
    if (ok) {
      const uint8_t* src = resolved.empty() ? static_cast<const uint8_t*>(readback.mapped) : resolved.data();
      const float scale = std::ldexp(1.0f, dest_bias);
      const uint32_t bpp_log2 = dest_bytes == 8 ? 3 : 2;
      if (dest_bytes == 4 && !dest_bias && !dest_swap) {
        // The common case (a 32-bit target copied as it is): in the Xenos tiling, 4 texels starting at a multiple of
        // 4 in x are contiguous, so rows go 16 bytes at a time; the rows are spread over threads.
        auto rows = [&](uint32_t first, uint32_t last) {
          for (uint32_t y = first; y < last; ++y) {
            const uint32_t ty = uint32_t(y0) + y;
            for (uint32_t x = 0; x + 4 <= width; x += 4) {
              const uint32_t address = dest_base + uint32_t(TiledOffset2D(int32_t(uint32_t(x0) + x), int32_t(ty), dest_pitch, 2));
              if (address + 16 > mirror_bytes_) continue;
              uint32_t run[4];
              std::memcpy(run, src + (size_t(y) * width + x) * 4, 16);
              for (uint32_t& v : run) v = GpuSwap(v, Endian(dest_endian <= 3 ? dest_endian : 0));
              std::memcpy(physical_ + address, run, 16);
            }
          }
        };
        const uint32_t threads = std::clamp<uint32_t>(std::thread::hardware_concurrency(), 1, 8);
        std::vector<std::thread> workers;
        const uint32_t step = (height + threads - 1) / threads;
        for (uint32_t t = 1; t < threads && t * step < height; ++t) workers.emplace_back(rows, t * step, std::min(height, (t + 1) * step));
        rows(0, std::min(height, step));
        for (std::thread& w : workers) w.join();
      } else for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          uint8_t element[8];
          const uint8_t* texel = src + (size_t(y) * width + x) * source_bytes;
          if (dest_bytes == 4) {
            uint8_t c[4];
            for (int i = 0; i < 4; ++i) {
              c[i] = dest_bias ? uint8_t(std::clamp(std::lround(texel[i] * scale), 0L, 255L)) : texel[i];
            }
            if (dest_swap) std::swap(c[0], c[2]);
            std::memcpy(element, c, 4);  // component 0 in the low byte, as the GPU writes
          } else {
            uint16_t c[4];
            std::memcpy(c, texel, 8);
            if (dest_bias) {
              for (uint16_t& h : c) h = FloatToHalf(HalfToFloat(h) * scale);
            }
            if (dest_swap) std::swap(c[0], c[2]);
            std::memcpy(element, c, 8);
          }
          SwapElement(element, dest_bytes, dest_endian);
          const uint32_t tx = uint32_t(x0) + x, ty = uint32_t(y0) + y;
          const uint32_t address = dest_base + uint32_t(TiledOffset2D(int32_t(tx), int32_t(ty), dest_pitch, bpp_log2));
          if (address + dest_bytes <= mirror_bytes_) std::memcpy(physical_ + address, element, dest_bytes);
        }
      }
    }
    DestroyBuffer(device_, readback);
    if (!ok) {
      Fail("resolve copy");
      return;
    }
  }

  // The clears: the colour source and the depth target over the same rectangle, with the values RB_COLOR_CLEAR /
  // RB_DEPTH_CLEAR give in the targets' formats (as the reference decodes them).
  if (clear_color || clear_depth) {
    Surface* color_target = clear_color ? surface : nullptr;
    Surface* depth_target =
        clear_depth ? GetSurface({depth_info & 0xFFF, pitch * sample_x, (depth_info >> 16) & 1, 1}) : nullptr;
    VkClearValue color_value{}, depth_value{};
    if (color_target) {
      const uint64_t clear = regs[kRbColorClear] | uint64_t(regs[kRbColorClearLo]) << 32;
      switch (source_key.format) {
        case 0: case 1:  // 8_8_8_8 (gamma alike)
          for (uint32_t j = 0; j < 4; ++j) color_value.color.float32[j] = float((clear >> (8 * j)) & 0xFF) / 255.0f;
          break;
        case 6: case 7:  // 16-bit float
          for (uint32_t j = 0; j < 4; ++j) color_value.color.float32[j] = HalfToFloat(uint16_t(clear >> (16 * j)));
          break;
        case 14:
          color_value.color.float32[0] = std::bit_cast<float>(uint32_t(clear));
          break;
        default:
          Fail("resolve clear of target format " + std::to_string(source_key.format) + " (not yet)");
          return;
      }
    }
    if (depth_target) {
      const uint32_t clear = regs[kRbDepthClear];
      // D24FS8 holds depth as float24 (20e4: 4 exponent bits, bias 15, 20 mantissa bits); the host depth buffer
      // holds the same value in its own format.
      const float depth = ((depth_info >> 16) & 1) ? Float20e4ToFloat(clear >> 8) : float(clear >> 8) / float(0xFFFFFF);
      depth_value.depthStencil = {depth, clear & 0xFF};
    }
    {
      // In the batch, after whatever it copies from the surface. A render pass over the rectangle that clears on
      // load and stores: clears exactly the rectangle.
      EndRendering();
      VkCommandBuffer cb = Record();
      VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if (color_target) {
        Transition(cb, color_target->image, color_target->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        color.imageView = color_target->image.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = color_value;
      }
      if (depth_target) {
        Transition(cb, depth_target->image, depth_target->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth.imageView = depth_target->image.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = depth_value;
      }
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea = {{x0 * int32_t(sample_x), y0 * int32_t(sample_y)}, {width * sample_x, height * sample_y}};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = color_target ? 1 : 0;
      rendering.pColorAttachments = color_target ? &color : nullptr;
      rendering.pDepthAttachment = depth_target ? &depth : nullptr;
      rendering.pStencilAttachment = depth_target ? &depth : nullptr;
      f.vkCmdBeginRendering(cb, &rendering);
      f.vkCmdEndRendering(cb);
    }
  }
  // Textures over the destination are hashed again before their next use (the tiled rows the rectangle reaches).
  if (textures_) {
    const uint32_t element = is_depth ? 4 : (dest_format == 32 ? 8 : 4);
    const uint64_t rows = (uint64_t(y0) + height + 31) & ~uint64_t(31);
    textures_->MemoryChanged(dest_base, uint32_t(std::min<uint64_t>(dest_base + rows * ((dest_pitch + 31) & ~31u) * element,
                                                                    mirror_bytes_)));
  }
  last_resolve_ = {surface, dest_base, dest_endian, uint32_t(x0), uint32_t(y0), width, height,
                   !is_depth && !msaa && dest_format == 6 && !dest_bias && !dest_swap};
  ++stats_.resolves;
  resolve_formats_[dest_base] = is_depth ? 0x100 | dest_format : dest_format;
  if (is_depth) ++stats_.resolves_depth;
  if (on_gpu) ++stats_.resolves_on_gpu;
  if (read_back) ++stats_.resolves_read_back;
}

void VulkanRenderer::Invalidate(uint32_t base, uint32_t size) {
  if (textures_) textures_->MemoryChanged(base & 0x1FFFFFFF, uint32_t(std::min<uint64_t>(uint64_t(base & 0x1FFFFFFF) + size, mirror_bytes_)));
}

bool VulkanRenderer::WriteBackResolved(uint32_t base, Resolved& r) {
  if (!QueueWriteBack(base, r)) return false;
  Flush();
  return true;
}

bool VulkanRenderer::QueueWriteBack(uint32_t base, Resolved& r) {
  // The image holds the destination's bytes as a texture reads them: copied into a buffer in the batch, then tiled
  // into guest memory with its endianness once the batch has run (FinishWriteBacks).
  r.stale = false;
  ++stats_.resolve_write_backs;
  PendingWriteBack w{};
  w.resolved = &r;
  w.base = base, w.x = r.x, w.y = r.y, w.width = r.width, w.height = r.height, w.pitch = r.pitch, w.endian = r.endian;
  w.depth = r.image.format == VK_FORMAT_R32_SFLOAT;
  w.float24 = r.float24;
  w.element = r.image.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
  if (!r.width || !CreateBuffer(device_, VkDeviceSize(r.width) * r.height * w.element, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                w.buffer)) {
    return false;
  }
  EndRendering();
  VkCommandBuffer cb = Record();
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(r.x), int32_t(r.y), 0};
  region.imageExtent = {r.width, r.height, 1};
  device_.fn().vkCmdCopyImageToBuffer(cb, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, w.buffer.buffer, 1, &region);
  r.pending = true;
  pending_write_backs_.push_back(w);
  return true;
}

void VulkanRenderer::FinishWriteBacks() {
  for (PendingWriteBack& w : pending_write_backs_) {
    const uint8_t* src = static_cast<const uint8_t*>(w.buffer.mapped);
    const uint32_t element = w.element;
    const bool depth = w.depth;
    for (uint32_t y = 0; y < w.height; ++y) {
      for (uint32_t x = 0; x < w.width; ++x) {
        uint8_t bytes[8];
        std::memcpy(bytes, src + (size_t(y) * w.width + x) * element, element);
        if (depth) {
          // The guest's depth word (depth << 8, stencil not kept: 0), as the CPU depth resolve writes it.
          float value;
          std::memcpy(&value, bytes, 4);
          const uint32_t guest = w.float24 ? FloatTo20e4(value)
                                           : uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * float(0xFFFFFF)));
          const uint32_t word = GpuSwap(guest << 8, Endian(w.endian <= 3 ? w.endian : 0));
          std::memcpy(bytes, &word, 4);
        } else {
          SwapElement(bytes, element, w.endian);
        }
        const uint32_t address =
            w.base + uint32_t(TiledOffset2D(int32_t(w.x + x), int32_t(w.y + y), w.pitch, element == 8 ? 3 : 2));
        if (address + element <= mirror_bytes_) std::memcpy(physical_ + address, bytes, element);
      }
    }
    if (textures_) textures_->MemoryChanged(w.base, w.base + w.pitch * ((w.y + w.height + 31) & ~31u) * element);
    w.resolved->pending = false;
    DestroyBuffer(device_, w.buffer);
  }
  pending_write_backs_.clear();
}

bool VulkanRenderer::ReadImageRGBA8(Image& image, VkImageLayout& layout, uint32_t x, uint32_t y, uint32_t width,
                                    uint32_t height, std::vector<uint32_t>& pixels) {
  Flush();
  Buffer readback;
  if (!CreateBuffer(device_, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback)) return false;
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    Transition(cb, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    region.imageExtent = {width, height, 1};
    device_.fn().vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
  });
  if (ok) {
    pixels.resize(size_t(width) * height);
    std::memcpy(pixels.data(), readback.mapped, pixels.size() * 4);
    if (image.format == VK_FORMAT_B8G8R8A8_UNORM) {
      for (uint32_t& p : pixels) p = (p & 0xFF00FF00u) | (p & 0xFF) << 16 | ((p >> 16) & 0xFF);
    }
  }
  DestroyBuffer(device_, readback);
  return ok;
}

void VulkanRenderer::Swap(uint32_t, uint32_t width, uint32_t height, const uint32_t* regs) {
  Flush();  // a frame boundary: nothing stays pending across it
  if (textures_) textures_->NewFrame();  // the CPU may have written any texture since
  Timer swap_timer(stats_.ns_swaps);
  // The front buffer is a texture in guest memory, usually the last resolve's destination. When it is exactly that
  // (same address, k_8_8_8_8 read with the endianness it was written with, the colour components unswizzled), the
  // frame is that resolve's source surface on the GPU and nothing is read back.
  Frame next;
  const TextureFetch fetch = TextureFetch::Parse(regs + reg::kFetchConstants);
  if (front_buffers_.insert(fetch.base & 0x1FFFFFFF).second) {
    // A front buffer seen for the first time: a resolve there before now stayed on the GPU only.
    const auto it = resolved_.find(fetch.base & 0x1FFFFFFF);
    if (it != resolved_.end()) WriteBackResolved(it->first, it->second);
  }
  // A front buffer that is a resolve kept on the GPU, read as the resolve's image shows it (an 8-bit resolve, its
  // red/blue swap undone by the fetch swizzle or absent): presented from that image.
  const uint32_t front = fetch.base & 0x1FFFFFFF;
  const auto front_resolve = resolved_.find(front);
  if (front_resolve != resolved_.end() && fetch.type == 2 && fetch.format == 6) {
    Resolved& r = front_resolve->second;
    const uint32_t swizzle = fetch.swizzle & 0x1FF;
    const bool as_shown = (r.image.format == VK_FORMAT_R8G8B8A8_UNORM && swizzle == (0 | 1 << 3 | 2 << 6)) ||
                          (r.image.format == VK_FORMAT_B8G8R8A8_UNORM && swizzle == (2 | 1 << 3 | 0 << 6));
    if (as_shown && fetch.endian == r.endian && r.x == 0 && r.y == 0) {
      next.width = std::min({width ? width : fetch.width, fetch.width, r.width});
      next.height = std::min({height ? height : fetch.height, fetch.height, r.height});
      next.on_gpu = true;
      next.number = frame_.number + 1;
      frame_ = std::move(next);
      frame_surface_ = nullptr;
      frame_resolved_ = &r;
      frame_x_ = frame_y_ = 0;
      front_on_gpu_.insert(front);
      return;
    }
  }
  front_on_gpu_.erase(front);
  frame_resolved_ = nullptr;
  const LastResolve& lr = last_resolve_;
  if (lr.surface && lr.plain && fetch.type == 2 && fetch.format == 6 && (fetch.base & 0x1FFFFFFF) == lr.dest_base &&
      fetch.endian == lr.endian && (fetch.swizzle & 0x1FF) == (0 | 1 << 3 | 2 << 6) && lr.x == 0 && lr.y == 0) {
    next.width = std::min({width ? width : fetch.width, fetch.width, lr.width});
    next.height = std::min({height ? height : fetch.height, fetch.height, lr.height});
    next.on_gpu = true;
    next.number = frame_.number + 1;
    frame_ = std::move(next);
    frame_surface_ = lr.surface;
    frame_x_ = lr.x, frame_y_ = lr.y;
    return;
  }
  frame_surface_ = nullptr;
  std::string error;
  std::vector<uint32_t> texels;
  uint32_t tw = 0, th = 0;
  if (!textures_ || !textures_->ReadRGBA8(regs + reg::kFetchConstants, tw, th, texels, error)) {
    Fail("swap: " + error);
    return;
  }
  next.width = std::min(width ? width : tw, tw);
  next.height = std::min(height ? height : th, th);
  next.pixels.resize(size_t(next.width) * next.height);
  for (uint32_t y = 0; y < next.height; ++y) {
    std::memcpy(&next.pixels[size_t(y) * next.width], &texels[size_t(y) * tw], size_t(next.width) * 4);
  }
  // Through the gamma ramp, as presented.
  for (uint32_t& px : next.pixels) {
    px = (px & 0xFF000000u) | gamma_lut_[0][px & 0xFF] | uint32_t(gamma_lut_[1][(px >> 8) & 0xFF]) << 8 |
         uint32_t(gamma_lut_[2][(px >> 16) & 0xFF]) << 16;
  }
  next.number = frame_.number + 1;
  frame_ = std::move(next);
}

std::vector<uint32_t> BuildGammaShader();  // gamma_shader.cpp
std::vector<uint32_t> BuildDepthCopyShader();  // depth_shader.cpp

bool VulkanRenderer::DepthPass() {
  if (depth_pipeline_) return true;
  const Functions& f = device_.fn();
  const VkDevice d = device_.device();
  VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 2;
  layout_info.pBindings = bindings;
  if (f.vkCreateDescriptorSetLayout(d, &layout_info, nullptr, &depth_layout_) != VK_SUCCESS) return Fail("depth copy set layout");
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &depth_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push;
  if (f.vkCreatePipelineLayout(d, &pipeline_layout_info, nullptr, &depth_pipeline_layout_) != VK_SUCCESS) {
    return Fail("depth copy pipeline layout");
  }
  const std::vector<uint32_t> code = BuildDepthCopyShader();
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = code.size() * 4;
  module_info.pCode = code.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (f.vkCreateShaderModule(d, &module_info, nullptr, &module) != VK_SUCCESS) return Fail("depth copy shader");
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = depth_pipeline_layout_;
  const VkResult created = f.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &depth_pipeline_);
  f.vkDestroyShaderModule(d, module, nullptr);
  if (created != VK_SUCCESS) return Fail("depth copy pipeline");
  const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDepthSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDepthSets}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kDepthSets;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &depth_pool_) != VK_SUCCESS) return Fail("depth copy pool");
  return true;
}

bool VulkanRenderer::ResolveDepthOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height,
                                       uint32_t sample_x, uint32_t sample_y, uint32_t sample, uint32_t dest_base,
                                       uint32_t dest_pitch, uint32_t dest_endian) {
  if (!DepthPass()) return false;
  const Functions& f = device_.fn();
  const VkDevice d = device_.device();
  if (!surface.depth_view) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = surface.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = surface.image.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (f.vkCreateImageView(d, &vi, nullptr, &surface.depth_view) != VK_SUCCESS) return Fail("depth resolve view");
  }
  const uint32_t pitch = (dest_pitch + 31) & ~31u, rows = (uint32_t(y0) + height + 31) & ~31u;
  Resolved& r = resolved_[dest_base];
  if (!r.image.image || r.image.format != VK_FORMAT_R32_SFLOAT || r.image.width < pitch || r.image.height < rows) {
    if (r.image.image) {
      Flush();  // the batch may still read the old image
      DestroyImage(device_, r.image);
    }
    if (!CreateImage2D(device_, pitch, std::max(rows, 32u), VK_FORMAT_R32_SFLOAT,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, r.image)) {
      resolved_.erase(dest_base);
      return Fail("depth resolve image");
    }
    r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  if (depth_sets_ >= kDepthSets) Flush();  // resets the pool
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = depth_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &depth_layout_;
  if (f.vkAllocateDescriptorSets(d, &alloc, &set) != VK_SUCCESS) return Fail("depth resolve set");
  ++depth_sets_;
  const VkDescriptorImageInfo source{VK_NULL_HANDLE, surface.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorImageInfo dest{VK_NULL_HANDLE, r.image.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  writes[0].dstSet = writes[1].dstSet = set;
  writes[0].dstBinding = 0, writes[1].dstBinding = 1;
  writes[0].descriptorCount = writes[1].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[0].pImageInfo = &source;
  writes[1].pImageInfo = &dest;
  f.vkUpdateDescriptorSets(d, 2, writes, 0, nullptr);

  // After the draws in the batch. The sample's position in its pixel: bit 0 down, bit 1 right.
  EndRendering();
  VkCommandBuffer cb = Record();
  Transition(cb, surface.image, surface.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_);
  f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_layout_, 0, 1, &set, 0, nullptr);
  const int32_t region[8] = {x0 * int32_t(sample_x) + int32_t(sample_x > 1 ? sample >> 1 : 0),
                             y0 * int32_t(sample_y) + int32_t(sample_y > 1 ? sample & 1 : 0),
                             x0, y0, int32_t(width), int32_t(height), int32_t(sample_x), int32_t(sample_y)};
  f.vkCmdPushConstants(cb, depth_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(region), region);
  f.vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  r.pitch = pitch, r.endian = dest_endian, r.x = uint32_t(x0), r.y = uint32_t(y0), r.width = width, r.height = height;
  r.stale = true;
  r.float24 = surface.key.format == 1;
  if (textures_) {
    textures_->SetGpuSource({r.image.image, VK_FORMAT_R32_SFLOAT, dest_base, pitch, dest_endian, r.image.width,
                             r.image.height, ++resolve_generation_, true});
  }
  return true;
}

bool VulkanRenderer::GammaPass() {
  if (gamma_pipeline_) return true;
  const Functions& f = device_.fn();
  const VkDevice d = device_.device();
  VkDescriptorSetLayoutBinding bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 3;
  layout_info.pBindings = bindings;
  if (f.vkCreateDescriptorSetLayout(d, &layout_info, nullptr, &gamma_layout_) != VK_SUCCESS) return Fail("gamma set layout");
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &gamma_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push;
  if (f.vkCreatePipelineLayout(d, &pipeline_layout_info, nullptr, &gamma_pipeline_layout_) != VK_SUCCESS) {
    return Fail("gamma pipeline layout");
  }
  const std::vector<uint32_t> code = BuildGammaShader();
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = code.size() * 4;
  module_info.pCode = code.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (f.vkCreateShaderModule(d, &module_info, nullptr, &module) != VK_SUCCESS) return Fail("gamma shader");
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = gamma_pipeline_layout_;
  const VkResult created = f.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &gamma_pipeline_);
  f.vkDestroyShaderModule(d, module, nullptr);
  if (created != VK_SUCCESS) return Fail("gamma pipeline");
  const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &gamma_pool_) != VK_SUCCESS) return Fail("gamma pool");
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = gamma_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &gamma_layout_;
  if (f.vkAllocateDescriptorSets(d, &alloc, &gamma_set_) != VK_SUCCESS) return Fail("gamma set");
  if (!CreateBuffer(device_, 3 * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, gamma_lut_buffer_)) return Fail("gamma LUT");
  return true;
}

void VulkanRenderer::RecordFrameBlit(VkCommandBuffer cb, VkImage target, VkOffset3D dst0, VkOffset3D dst1) {
  if (!frame_.on_gpu || (!frame_surface_ && !frame_resolved_) || !GammaPass()) return;
  Image& frame_image = frame_surface_ ? frame_surface_->image : frame_resolved_->image;
  VkImageLayout& frame_layout = frame_surface_ ? frame_surface_->layout : frame_resolved_->layout;
  const Functions& f = device_.fn();
  // The output image, as large as the frame.
  if (gamma_image_.width < frame_.width || gamma_image_.height < frame_.height) {
    f.vkDeviceWaitIdle(device_.device());
    DestroyImage(device_, gamma_image_);
    gamma_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!CreateImage2D(device_, frame_.width, frame_.height, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, gamma_image_)) {
      Fail("gamma image");
      return;
    }
  }
  // The ramp as 8-bit outputs (the shader's LUT), and the set (the frame's surface may change from frame to frame).
  uint32_t* lut = static_cast<uint32_t*>(gamma_lut_buffer_.mapped);
  for (uint32_t ch = 0; ch < 3; ++ch) {
    for (uint32_t i = 0; i < 256; ++i) lut[ch * 256 + i] = gamma_lut_[ch][i];
  }
  const VkDescriptorImageInfo source{VK_NULL_HANDLE, frame_image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorImageInfo dest{VK_NULL_HANDLE, gamma_image_.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo lut_info{gamma_lut_buffer_.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].dstSet = gamma_set_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = types[i];
  }
  writes[0].pImageInfo = &source;
  writes[1].pImageInfo = &dest;
  writes[2].pBufferInfo = &lut_info;
  f.vkUpdateDescriptorSets(device_.device(), 3, writes, 0, nullptr);

  Transition(cb, frame_image, frame_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Transition(cb, gamma_image_, gamma_image_layout_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline_);
  f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline_layout_, 0, 1, &gamma_set_, 0, nullptr);
  const int32_t region[4] = {int32_t(frame_x_), int32_t(frame_y_), int32_t(frame_.width), int32_t(frame_.height)};
  f.vkCmdPushConstants(cb, gamma_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(region), region);
  f.vkCmdDispatch(cb, (frame_.width + 7) / 8, (frame_.height + 7) / 8, 1);
  Transition(cb, gamma_image_, gamma_image_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(frame_.width), int32_t(frame_.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = dst0;
  blit.dstOffsets[1] = dst1;
  f.vkCmdBlitImage(cb, gamma_image_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
}

void VulkanRenderer::GammaRamp(const uint32_t* table, const uint32_t*) {
  // An 8-bit value through the 256-entry table: its 10-bit output scaled back to 8 bits (red from bit 20, green from
  // bit 10, blue from bit 0).
  for (uint32_t c = 0; c < 3; ++c) {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v10 = (table[i] >> (10 * (2 - c))) & 0x3FF;
      gamma_lut_[c][i] = uint8_t((v10 * 255 + 511) / 1023);
    }
  }
}

bool VulkanRenderer::ReadFrame(std::vector<uint32_t>& pixels) {
  if (!frame_.on_gpu) {
    pixels = frame_.pixels;
    return !pixels.empty();
  }
  if (frame_resolved_) {
    if (!ReadImageRGBA8(frame_resolved_->image, frame_resolved_->layout, 0, 0, frame_.width, frame_.height, pixels)) {
      return false;
    }
  } else {
    if (!frame_surface_) return false;
    std::vector<uint8_t> bytes;
    if (frame_x_ || frame_y_ || !ReadBack(frame_surface_->key, frame_.width, frame_.height, bytes)) return false;
    pixels.resize(size_t(frame_.width) * frame_.height);
    std::memcpy(pixels.data(), bytes.data(), pixels.size() * 4);
  }
  for (uint32_t& p : pixels) {
    p = (p & 0xFF000000u) | gamma_lut_[0][p & 0xFF] | uint32_t(gamma_lut_[1][(p >> 8) & 0xFF]) << 8 |
        uint32_t(gamma_lut_[2][(p >> 16) & 0xFF]) << 16;
  }
  return !pixels.empty();
}

}  // namespace any360::gfx::vk
