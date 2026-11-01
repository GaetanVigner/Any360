#include "any360/gfx/vulkan/renderer.h"

#include <algorithm>
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
constexpr uint32_t kPaClVteCntl = 0x2206, kPaClVportXScale = 0x210F, kRbColorControl = 0x2202;
// Bytes each constant buffer can hold (the most a shader can address).
constexpr uint32_t kFloatBufferBytes = 256 * 16, kBoolLoopBytes = (8 + 32) * 4, kFetchBytes = 32 * 6 * 4;

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

float AsFloat(uint32_t bits) { return std::bit_cast<float>(bits); }

// The topology of a Xenos primitive type the host draws as is; 0x7FFFFFFF for those that need expansion.
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
}  // namespace

struct VulkanRenderer::TranslatedShader {
  std::unique_ptr<rg::SpirvShader> shader;
  uint64_t modification = 0;
  VkShaderModule module = VK_NULL_HANDLE;
};

VulkanRenderer::VulkanRenderer(Device& device, const uint8_t* physical, uint32_t mirror_bytes)
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
    if (shader->module) f.vkDestroyShaderModule(d, shader->module, nullptr);
  }
  if (pool_) f.vkDestroyDescriptorPool(d, pool_, nullptr);
  if (pipeline_layout_) f.vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
  for (VkDescriptorSetLayout l : {shared_layout_, constants_layout_, empty_layout_}) {
    if (l) f.vkDestroyDescriptorSetLayout(d, l, nullptr);
  }
  DestroyBuffer(device_, shared_memory_);
  DestroyBuffer(device_, constants_);
  DestroyBuffer(device_, indices_);
  DestroyImage(device_, target_);
}

bool VulkanRenderer::Fail(const std::string& why) {
  last_error_ = why;
  ++stats_.failed;
  return false;
}

bool VulkanRenderer::Initialize(uint32_t width, uint32_t height) {
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
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = empty_layout_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = empty_layout_;
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  layout_info.pSetLayouts = sets;
  if (f.vkCreatePipelineLayout(d, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) return Fail("pipeline layout");

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

  if (!CreateImage2D(device_, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, target_)) {
    return Fail("render target");
  }
  return true;
}

namespace {
void Transition(const Functions& f, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
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

bool VulkanRenderer::Clear(const float clear[4]) {
  const Functions& f = device_.fn();
  return device_.Submit([&](VkCommandBuffer cb) {
    Transition(f, cb, target_.image, target_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue color;
    std::memcpy(color.float32, clear, sizeof(color.float32));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(cb, target_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    Transition(f, cb, target_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    target_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  });
}

bool VulkanRenderer::ReadBack(std::vector<uint32_t>& pixels) {
  const Functions& f = device_.fn();
  Buffer readback;
  const VkDeviceSize bytes = VkDeviceSize(target_.width) * target_.height * 4;
  if (!CreateBuffer(device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback)) return false;
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    Transition(f, cb, target_.image, target_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {target_.width, target_.height, 1};
    f.vkCmdCopyImageToBuffer(cb, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    Transition(f, cb, target_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    target_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  });
  if (ok) {
    pixels.resize(size_t(target_.width) * target_.height);
    std::memcpy(pixels.data(), readback.mapped, bytes);
  }
  DestroyBuffer(device_, readback);
  return ok;
}

VulkanRenderer::TranslatedShader* VulkanRenderer::GetShader(const gfx::Shader& source) {
  auto& slot = shaders_[{source.type, source.hash}];
  if (slot) return slot->module ? slot.get() : nullptr;
  slot = std::make_unique<TranslatedShader>();
  const rg::xenos::ShaderType type = source.type ? rg::xenos::ShaderType::kPixel : rg::xenos::ShaderType::kVertex;
  slot->shader = std::make_unique<rg::SpirvShader>(type, source.hash, source.microcode.data(), source.microcode.size(),
                                                   std::endian::native);
  rex::string::StringBuffer disassembly;
  slot->shader->AnalyzeUcode(disassembly);
  // Default modification: no interpolator, clip plane or render target specifics yet.
  slot->modification =
      type == rg::xenos::ShaderType::kVertex
          ? translator_->GetDefaultVertexShaderModification(slot->shader->GetDynamicAddressableRegisterCount(0),
                                                            rg::Shader::HostVertexShaderType::kVertex)
          : translator_->GetDefaultPixelShaderModification(slot->shader->GetDynamicAddressableRegisterCount(0));
  rg::Shader::Translation& translation = *slot->shader->GetOrCreateTranslation(slot->modification);
  if (!translator_->TranslateAnalyzedShader(translation) || !translation.is_valid()) return nullptr;
  const std::vector<uint8_t>& binary = translation.translated_binary();
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = binary.size();
  info.pCode = reinterpret_cast<const uint32_t*>(binary.data());
  if (device_.fn().vkCreateShaderModule(device_.device(), &info, nullptr, &slot->module) != VK_SUCCESS) return nullptr;
  ++stats_.translations;
  return slot.get();
}

VkPipeline VulkanRenderer::GetPipeline(const TranslatedShader& vs, const TranslatedShader* ps, VkPrimitiveTopology topology) {
  const auto key = std::make_tuple(vs.shader->ucode_data_hash(), ps ? ps->shader->ucode_data_hash() : 0, uint32_t(topology));
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs.module;
  stages[0].pName = "main";
  if (ps) {
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps->module;
    stages[1].pName = "main";
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = topology;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  const VkFormat format = target_.format;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
  info.stageCount = ps ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout_;
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
  TranslatedShader* vs = GetShader(*draw.vertex_shader);
  TranslatedShader* ps = draw.edram_mode == 5 || !draw.pixel_shader || draw.pixel_shader->microcode.empty()
                             ? nullptr
                             : GetShader(*draw.pixel_shader);
  if (!vs || (draw.edram_mode != 5 && draw.pixel_shader && !draw.pixel_shader->microcode.empty() && !ps)) {
    Fail("shader translation");
    return;
  }
  const VkPipeline pipeline = GetPipeline(*vs, ps, topology);
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
  const float size[2] = {float(target_.width), float(target_.height)};
  for (int i = 0; i < 3; ++i) {
    // Guest window coordinate = position * scale + offset (pixels for x and y, depth for z).
    const float scale = (vte & (1u << (2 * i))) ? AsFloat(regs[kPaClVportXScale + 2 * i]) : 1.0f;
    const float offset = (vte & (1u << (2 * i + 1))) ? AsFloat(regs[kPaClVportXScale + 2 * i + 1]) : 0.0f;
    if (i < 2) {
      system->ndc_scale[i] = scale * 2.0f / size[i];
      system->ndc_offset[i] = offset * 2.0f / size[i] - 1.0f;
    } else {
      system->ndc_scale[i] = scale;
      system->ndc_offset[i] = offset;
    }
  }
  system->vertex_index_max = UINT32_MAX;
  // Alpha test (RB_COLORCONTROL): the compare function's less / equal / greater bits are the pass flags; a disabled
  // test is "always" (with none of them set, every pixel would be discarded).
  const uint32_t color_control = regs[kRbColorControl];
  const uint32_t alpha_func = (color_control & (1u << 3)) ? (color_control & 7) : 7;
  system->flags |= alpha_func << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // The colour exponent bias is a multiplier: 2^bias, 1 for none.
  for (float& bias : system->color_exp_bias) bias = 1.0f;
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

  const bool submitted = device_.Submit([&](VkCommandBuffer cb) {
    if (target_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
      Transition(f, cb, target_.image, target_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      target_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {target_.width, target_.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    f.vkCmdBeginRendering(cb, &rendering);
    f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount];
    sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_set_;
    sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_set_;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = empty_set_;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = empty_set_;
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, SpirvShaderTranslator::kDescriptorSetCount,
                              sets, SpirvShaderTranslator::kConstantBufferCount, offsets);
    const VkViewport viewport{0, 0, size[0], size[1], 0, 1};
    const VkRect2D scissor{{0, 0}, {target_.width, target_.height}};
    f.vkCmdSetViewport(cb, 0, 1, &viewport);
    f.vkCmdSetScissor(cb, 0, 1, &scissor);
    if (index_type != VK_INDEX_TYPE_NONE_KHR) {
      f.vkCmdBindIndexBuffer(cb, indices_.buffer, 0, index_type);
      f.vkCmdDrawIndexed(cb, draw.index_count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(cb, draw.index_count, 1, 0, 0);
    }
    f.vkCmdEndRendering(cb);
  });
  if (!submitted) {
    Fail("submission");
    return;
  }
  ++stats_.draws;
}

}  // namespace any360::gfx::vk
