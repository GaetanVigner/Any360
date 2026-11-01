#include "any360/gfx/vulkan/renderer.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace any360::gfx::vk {

namespace rg = rex::graphics;
using rg::SpirvShaderTranslator;

namespace {
// Register file indices the renderer reads.
constexpr uint32_t kRbDepthInfo = 0x2002, kPaScScreenScissorTl = 0x200E, kPaScScreenScissorBr = 0x200F,
                   kPaScWindowOffset = 0x2080, kPaScWindowScissorTl = 0x2081, kPaScWindowScissorBr = 0x2082,
                   kRbColorMask = 0x2104, kRbBlendRed = 0x2105, kRbStencilRefMaskBf = 0x210C,
                   kRbStencilRefMask = 0x210D, kPaClVportXScale = 0x210F, kRbDepthControl = 0x2200,
                   kRbColorControl = 0x2202, kPaSuScModeCntl = 0x2205, kPaClVteCntl = 0x2206,
                   kRbModeControl = 0x2208, kPaSuPolyOffsetFrontScale = 0x2380, kRbSurfaceInfo = 0x2000,
                   kSqProgramCntl = 0x2180, kSqContextMisc = 0x2181;
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
  f.vkDeviceWaitIdle(d);
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
  for (auto& [key, surface] : surfaces_) DestroyImage(device_, surface->image);
}

bool VulkanRenderer::Fail(const std::string& why) {
  last_error_ = why;
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
  // Texture sets: allocated per draw, the pool reset once the draw has run.
  VkDescriptorPoolSize draw_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64}, {VK_DESCRIPTOR_TYPE_SAMPLER, 64}};
  VkDescriptorPoolCreateInfo draw_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  draw_pool_info.maxSets = 2;
  draw_pool_info.poolSizeCount = 2;
  draw_pool_info.pPoolSizes = draw_sizes;
  if (f.vkCreateDescriptorPool(d, &draw_pool_info, nullptr, &draw_pool_) != VK_SUCCESS) return Fail("texture descriptor pool");

  // Buffers: the mirrored guest memory, one draw's constants, indices.
  if (!CreateBuffer(device_, mirror_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, shared_memory_) ||
      !CreateBuffer(device_, 64 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, constants_) ||
      !CreateBuffer(device_, 4 * 1024 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, indices_)) {
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

  // Depth/stencil surfaces: D24S8 where the device has it (NVIDIA), else D32S8 (AMD).
  depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkFormatProperties props;
  f.vkGetPhysicalDeviceFormatProperties(device_.physical(), VK_FORMAT_D24_UNORM_S8_UINT, &props);
  if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
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
  const VkImageUsageFlags usage = (key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
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
  const VkPrimitiveTopology topology = Topology(draw.primitive_type);
  if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
    Fail("primitive type " + std::to_string(draw.primitive_type) + " needs expansion (not yet)");
    return;
  }
  if (draw.source == DrawSource::kImmediate) {
    Fail("immediate indices");
    return;
  }
  const bool polygonal = IsPolygonal(topology);

  TranslatedShader* vs = GetShader(*draw.vertex_shader);
  const bool wants_ps = edram_mode == 4 && draw.pixel_shader && !draw.pixel_shader->microcode.empty();
  TranslatedShader* ps = wants_ps ? GetShader(*draw.pixel_shader) : nullptr;
  if (!vs || (wants_ps && !ps)) {
    Fail("shader analysis");
    return;
  }
  // The translations for this draw: the interpolators the vertex shader writes and the pixel shader reads
  // (SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC), and the register counts dynamic addressing may reach.
  rg::reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = regs[kSqProgramCntl];
  rg::reg::SQ_CONTEXT_MISC context_misc;
  context_misc.value = regs[kSqContextMisc];
  uint32_t interpolators = 0;
  if (ps) {
    uint32_t param_gen = UINT32_MAX;
    interpolators = vs->shader->writes_interpolators() & ps->shader->GetInterpolatorInputMask(program_cntl, context_misc, param_gen);
    if (param_gen != UINT32_MAX) {
      Fail("pixel shader parameter generation (not yet)");
      return;
    }
  }
  SpirvShaderTranslator::Modification vs_modification(translator_->GetDefaultVertexShaderModification(
      vs->shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg), rg::Shader::HostVertexShaderType::kVertex));
  vs_modification.vertex.interpolator_mask = interpolators;
  const VkShaderModule vs_module = GetModule(*vs, vs_modification.value);
  VkShaderModule ps_module = VK_NULL_HANDLE;
  if (ps) {
    SpirvShaderTranslator::Modification ps_modification(
        translator_->GetDefaultPixelShaderModification(ps->shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    ps_modification.pixel.interpolator_mask = interpolators;
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
      // The translator declares an unsigned and a signed binding for a fetch and picks one by the fetch constant's
      // signs at run time; until signed views exist, a texture with no signed component binds its unsigned view to
      // both (the shader never samples the signed one then).
      const uint32_t* fetch = regs + reg::kFetchConstants + 6 * binding.fetch_constant;
      if (binding.is_signed) {
        const uint32_t signs = (fetch[0] >> 2) & 0xFF;
        if (((signs & 0x55) & ~(signs >> 1)) != 0) {  // a component of sign 1 (signed)
          Fail("signed textures (not yet)");
          return;
        }
      }
      std::string error;
      const VkImageView view = textures_->View(fetch, error);
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
  const VkPipelineLayout pipeline_layout =
      PipelineLayout(uint32_t(stage_textures[0].views.size()), uint32_t(stage_textures[0].samplers.size()),
                     uint32_t(stage_textures[1].views.size()), uint32_t(stage_textures[1].samplers.size()));
  if (!pipeline_layout) {
    Fail("pipeline layout");
    return;
  }

  // The surfaces: colour targets the pixel shader writes and RB_COLOR_MASK enables, the depth/stencil target when
  // the depth or stencil test is on; all at the pitch of RB_SURFACE_INFO.
  const uint32_t surface_info = regs[kRbSurfaceInfo];
  if ((surface_info >> 16) & 3) {
    Fail("MSAA surfaces (not yet)");
    return;
  }
  const uint32_t pitch = surface_info & 0x3FFF;
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
  left = std::max({left, int32_t(regs[kPaScScreenScissorTl] & 0x7FFF), 0});
  top = std::max({top, int32_t((regs[kPaScScreenScissorTl] >> 16) & 0x7FFF), 0});
  right = std::min({right, int32_t(regs[kPaScScreenScissorBr] & 0x7FFF), int32_t(width)});
  bottom = std::min({bottom, int32_t((regs[kPaScScreenScissorBr] >> 16) & 0x7FFF), int32_t(height)});
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
  const VkPipeline pipeline = GetPipeline(key);
  if (!pipeline) {
    Fail("pipeline creation");
    return;
  }

  // Constants, each at an aligned offset of one buffer: system, VS floats, PS floats, bool/loop, fetch.
  uint8_t* base = static_cast<uint8_t*>(constants_.mapped);
  uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
  uint32_t at = 0;
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
  const float size[2] = {float(width), float(height)};
  // The window offset moves vertices too when PA_SU_SC_MODE_CNTL asks for it.
  const float window_shift[2] = {(mode_cntl >> 16) & 1 ? float(offset_x) : 0.0f, (mode_cntl >> 16) & 1 ? float(offset_y) : 0.0f};
  for (int i = 0; i < 3; ++i) {
    // Guest window coordinate = position * scale + offset (pixels for x and y, depth for z).
    const float scale = (vte & (1u << (2 * i))) ? AsFloat(regs[kPaClVportXScale + 2 * i]) : 1.0f;
    const float offset = (vte & (1u << (2 * i + 1))) ? AsFloat(regs[kPaClVportXScale + 2 * i + 1]) : 0.0f;
    if (i < 2) {
      system->ndc_scale[i] = scale * 2.0f / size[i];
      system->ndc_offset[i] = (offset + window_shift[i]) * 2.0f / size[i] - 1.0f;
    } else {
      system->ndc_scale[i] = scale;
      system->ndc_offset[i] = offset;
    }
  }
  system->vertex_index_max = UINT32_MAX;
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
    uint8_t* out = reserve(index, kFloatBufferBytes);
    if (!shader) return;
    const auto& map = shader->shader->constant_register_map();
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

  // Guest memory the shaders fetch from, and the indices (endian-swapped to host order).
  std::memcpy(shared_memory_.mapped, physical_, mirror_bytes_);
  VkIndexType index_type = VK_INDEX_TYPE_NONE_KHR;
  if (draw.source == DrawSource::kDma) {
    if (uint64_t(draw.index_base) + draw.index_bytes > mirror_bytes_ || draw.index_bytes > indices_.size) {
      Fail("index buffer outside the mirrored memory");
      return;
    }
    const uint8_t* src = physical_ + draw.index_base;
    uint8_t* dst = static_cast<uint8_t*>(indices_.mapped);
    if (draw.index_32) {
      index_type = VK_INDEX_TYPE_UINT32;
      for (uint32_t i = 0; i < draw.index_bytes / 4; ++i) {
        uint32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        v = GpuSwap(v, draw.index_endian);
        std::memcpy(dst + 4 * i, &v, 4);
      }
    } else {
      index_type = VK_INDEX_TYPE_UINT16;
      for (uint32_t i = 0; i < draw.index_bytes / 2; ++i) {
        uint16_t v;
        std::memcpy(&v, src + 2 * i, 2);
        if (draw.index_endian == Endian::k8in16 || draw.index_endian == Endian::k8in32) v = uint16_t(v << 8 | v >> 8);
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
      f.vkResetDescriptorPool(device_.device(), draw_pool_, 0);
      Fail("texture descriptor set");
      return;
    }
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

  const bool submitted = device_.Submit([&](VkCommandBuffer cb) {
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
      f.vkCmdBindIndexBuffer(cb, indices_.buffer, 0, index_type);
      f.vkCmdDrawIndexed(cb, draw.index_count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(cb, draw.index_count, 1, 0, 0);
    }
    f.vkCmdEndRendering(cb);
  });
  f.vkResetDescriptorPool(device_.device(), draw_pool_, 0);  // the draw has run (Submit waits)
  if (!submitted) {
    Fail("submission");
    return;
  }
  ++stats_.draws;
}

namespace {
// Resolve registers.
constexpr uint32_t kRbCopyControl = 0x2318, kRbCopyDestBase = 0x2319, kRbCopyDestPitch = 0x231A, kRbCopyDestInfo = 0x231B,
                   kRbDepthClear = 0x231D, kRbColorClear = 0x231E, kRbColorClearLo = 0x231F, kPaSuVtxCntl = 0x2302;

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

void VulkanRenderer::Copy(const uint32_t* regs) {
  const Functions& f = device_.fn();
  const uint32_t copy_control = regs[kRbCopyControl];
  const uint32_t source = copy_control & 7, copy_command = (copy_control >> 20) & 3;
  const bool clear_color = (copy_control >> 8) & 1, clear_depth = (copy_control >> 9) & 1;
  if (copy_command > 1) {
    Fail("resolve copy command " + std::to_string(copy_command) + " (not yet)");
    return;
  }
  const uint32_t surface_info = regs[kRbSurfaceInfo];
  if ((surface_info >> 16) & 3) {
    Fail("MSAA resolve (not yet)");
    return;
  }
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
  const SurfaceKey source_key = is_depth ? SurfaceKey{depth_info & 0xFFF, pitch, (depth_info >> 16) & 1, 1}
                                         : SurfaceKey{color_info & 0xFFF, pitch, (color_info >> 16) & 0xF, 0};
  Surface* surface = GetSurface(source_key);
  if (!surface) {
    Fail("resolve source surface");
    return;
  }
  const uint32_t width = uint32_t(std::min<int32_t>(x1, int32_t(surface->image.width)) - x0);
  const uint32_t height = uint32_t(std::min<int32_t>(y1, int32_t(surface->image.height)) - y0);

  // The copy: the region read back, converted, tiled into guest memory (destination texels at the source's
  // coordinates, at RB_COPY_DEST_BASE with RB_COPY_DEST_PITCH).
  const uint32_t dest_info = regs[kRbCopyDestInfo];
  const uint32_t dest_endian = dest_info & 7, dest_format = (dest_info >> 7) & 0x3F, dest_swap = (dest_info >> 24) & 1;
  const int32_t dest_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21, signed
  const uint32_t dest_base = regs[kRbCopyDestBase] & 0x1FFFFFFF, dest_pitch = regs[kRbCopyDestPitch] & 0x3FFF;
  if (is_depth) {
    Fail("depth resolve (not yet)");
    return;
  }
  if ((dest_info >> 3) & 1) {
    Fail("resolve to a 3D / array destination (not yet)");
    return;
  }
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
  Buffer readback;
  if (!CreateBuffer(device_, VkDeviceSize(width) * height * source_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback)) {
    Fail("resolve readback buffer");
    return;
  }
  bool ok = device_.Submit([&](VkCommandBuffer cb) {
    Transition(cb, surface->image, surface->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {width, height, 1};
    f.vkCmdCopyImageToBuffer(cb, surface->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
  });
  if (ok) {
    const uint8_t* src = static_cast<const uint8_t*>(readback.mapped);
    const float scale = std::ldexp(1.0f, dest_bias);
    const uint32_t bpp_log2 = dest_bytes == 8 ? 3 : 2;
    for (uint32_t y = 0; y < height; ++y) {
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

  // The clears: the colour source and the depth target over the same rectangle, with the values RB_COLOR_CLEAR /
  // RB_DEPTH_CLEAR give in the targets' formats (as the reference decodes them).
  if (clear_color || clear_depth) {
    Surface* color_target = clear_color ? surface : nullptr;
    Surface* depth_target = clear_depth ? GetSurface({depth_info & 0xFFF, pitch, (depth_info >> 16) & 1, 1}) : nullptr;
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
      if ((depth_info >> 16) & 1) {
        Fail("float24 depth clear (not yet)");
        return;
      }
      depth_value.depthStencil = {float(clear >> 8) / float(0xFFFFFF), clear & 0xFF};
    }
    ok = device_.Submit([&](VkCommandBuffer cb) {
      // A render pass over the rectangle that clears on load and stores: clears exactly the rectangle.
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
      rendering.renderArea = {{x0, y0}, {width, height}};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = color_target ? 1 : 0;
      rendering.pColorAttachments = color_target ? &color : nullptr;
      rendering.pDepthAttachment = depth_target ? &depth : nullptr;
      rendering.pStencilAttachment = depth_target ? &depth : nullptr;
      f.vkCmdBeginRendering(cb, &rendering);
      f.vkCmdEndRendering(cb);
    });
    if (!ok) {
      Fail("resolve clear");
      return;
    }
  }
  ++stats_.resolves;
}

}  // namespace any360::gfx::vk
