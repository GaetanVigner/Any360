#include "any360/gfx/vulkan/renderer.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <thread>

#include <immintrin.h>

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
  const bool on = Diagnosing();
  std::chrono::steady_clock::time_point start = on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  explicit Timer(uint64_t& t) : total(t) {}
  ~Timer() {
    if (on) total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  }
};
// Register file indices the renderer reads.
constexpr uint32_t kRbDepthInfo = 0x2002, kPaScScreenScissorTl = 0x200E, kPaScScreenScissorBr = 0x200F,
                   kPaScWindowOffset = 0x2080, kPaScWindowScissorTl = 0x2081, kPaScWindowScissorBr = 0x2082,
                   kRbColorMask = 0x2104, kRbBlendRed = 0x2105, kRbStencilRefMaskBf = 0x210C, kRbAlphaRef = 0x210E,
                   kPaClClipCntl = 0x2204, kPaClUcp0X = 0x2388, kPaSuPointSize = 0x2280, kPaSuPointMinmax = 0x2281,
                   kRbStencilRefMask = 0x210D, kPaClVportXScale = 0x210F, kRbDepthControl = 0x2200,
                   kRbColorControl = 0x2202, kPaSuScModeCntl = 0x2205, kPaClVteCntl = 0x2206, kPaSuVtxCntl = 0x2302,
                   kRbModeControl = 0x2208, kPaSuPolyOffsetFrontScale = 0x2380, kRbSurfaceInfo = 0x2000,
                   kSqProgramCntl = 0x2180, kSqContextMisc = 0x2181, kSqInterpolatorCntl = 0x2182, kVgtMaxVtxIndx = 0x2100, kVgtMinVtxIndx = 0x2101,
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
    case 0x0E: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;  // a quad strip: its quads are the strip's triangle pairs
    case 0x0F: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;    // a polygon (convex): a fan from its first vertex
    default: return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  }
}

// The bytes of a memory export element of a ColorFormat, as the translator packs them (0: one it does not write).
uint32_t MemExportElementBytes(uint32_t format) {
  using F = rg::xenos::ColorFormat;
  switch (F(format)) {
    case F::k_8: case F::k_8_A: case F::k_8_B: return 1;
    case F::k_1_5_5_5: case F::k_5_6_5: case F::k_6_5_5: case F::k_8_8: case F::k_4_4_4_4: case F::k_16:
    case F::k_16_FLOAT: return 2;
    case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_8_8_8_8_AS_16_16_16_16: case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16: case F::k_10_11_11: case F::k_10_11_11_AS_16_16_16_16: case F::k_11_11_10:
    case F::k_11_11_10_AS_16_16_16_16: case F::k_16_16: case F::k_16_16_FLOAT: case F::k_32_FLOAT: return 4;
    case F::k_16_16_16_16: case F::k_16_16_16_16_FLOAT: case F::k_32_32_FLOAT: return 8;
    case F::k_32_32_32_32_FLOAT: return 16;
    default: return 0;
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

VulkanRenderer::VulkanRenderer(Device& device, uint8_t* physical, uint32_t mirror_bytes, uint32_t resolution_scale)
    : device_(device), physical_(physical), mirror_bytes_(mirror_bytes), scale_(std::clamp(resolution_scale, 1u, 4u)) {
  // Every translator feature on (what desktop Vulkan 1.3 drivers and RADV offer), but the float controls, as the
  // device reports them: a shader declaring a mode the device lacks is invalid (NVIDIA has no 32-bit denormal flush).
  SpirvShaderTranslator::Features features(true);
  if (device_.fn().vkGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceVulkan12Properties v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v12};
    device_.fn().vkGetPhysicalDeviceProperties2(device_.physical(), &props);
    features.denorm_flush_to_zero_float32 = v12.shaderDenormFlushToZeroFloat32;
    features.signed_zero_inf_nan_preserve_float32 = v12.shaderSignedZeroInfNanPreserveFloat32;
    features.rounding_mode_rte_float32 = v12.shaderRoundingModeRTEFloat32;
  }
  translator_ = std::make_unique<SpirvShaderTranslator>(features, true, true, false, scale_, scale_);
  translator_->SetSamplesAsPixels(true);  // guest samples are pixels of the host targets
}

uint32_t CentroidInterpolators(uint32_t interpolators, uint32_t surface_info, uint32_t context_misc,
                               uint32_t interpolator_cntl) {
  // The interpolators sampled at their centroid, as the reference picks them: with guest MSAA, SQ_CONTEXT_MISC
  // sc_sample_cntl centroids only, or centroids and centres by SQ_INTERPOLATOR_CNTL's per-interpolator pattern (a set
  // bit a centre). Guest samples being host pixels, the decoration matters to the host's own MSAA.
  rg::reg::SQ_CONTEXT_MISC misc;
  misc.value = context_misc;
  return interpolators & ~rg::xenos::GetInterpolatorSamplingPattern(rg::xenos::MsaaSamples((surface_info >> 16) & 3),
                                                                    misc.sc_sample_cntl, interpolator_cntl >> 16);
}

VulkanRenderer::~VulkanRenderer() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!d) return;
  if (batches_[0].cb) {
    Flush();
    WaitIdle();
  }
  if (submitter_.joinable()) {
    {
      std::lock_guard lock(submit_mutex_);
      submit_stopping_ = true;
    }
    submit_cv_.notify_all();
    submitter_.join();
  }
  {
    std::lock_guard lock(record_mutex_);
    record_stopping_ = true;
  }
  record_cv_.notify_all();
  for (auto& w : workers_) {
    if (w->thread.joinable()) w->thread.join();
    for (VkCommandPool pool : w->pools) {
      if (pool) f.vkDestroyCommandPool(d, pool, nullptr);
    }
    for (auto& pools : w->set_pools) {
      for (VkDescriptorPool pool : pools) f.vkDestroyDescriptorPool(d, pool, nullptr);
    }
  }
  {
    std::lock_guard lock(done_mutex_);
    stopping_ = true;
  }
  work_cv_.notify_all();
  if (completer_.joinable()) completer_.join();
  {
    std::lock_guard lock(compile_mutex_);
    compile_stop_ = true;
    compile_queue_.clear();
  }
  compile_cv_.notify_all();
  for (std::thread& t : compilers_) t.join();
  TakeCompiledPipelines();
  SavePipelineCache();
  f.vkDeviceWaitIdle(d);
  for (Batch& b : batches_) {
    if (b.fence) f.vkDestroyFence(d, b.fence, nullptr);
    if (timestamps_ && &b == &batches_[0]) f.vkDestroyQueryPool(d, timestamps_, nullptr);
    if (query_pool_ && &b == &batches_[0]) f.vkDestroyQueryPool(d, query_pool_, nullptr);
    if (gpu_marks_ && &b == &batches_[0]) f.vkDestroyQueryPool(d, gpu_marks_, nullptr);
    if (b.draw_pool) f.vkDestroyDescriptorPool(d, b.draw_pool, nullptr);
    if (b.depth_pool) f.vkDestroyDescriptorPool(d, b.depth_pool, nullptr);
    DestroyBuffer(device_, b.constants);
    DestroyBuffer(device_, b.indices);
    DestroyBuffer(device_, b.guest_staging);
  }
  for (Buffer& b : free_readbacks_) DestroyBuffer(device_, b);
  free_readbacks_.clear();
  if (gamma_pipeline_) f.vkDestroyPipeline(d, gamma_pipeline_, nullptr);
  if (gamma_pipeline_layout_) f.vkDestroyPipelineLayout(d, gamma_pipeline_layout_, nullptr);
  if (gamma_pool_) f.vkDestroyDescriptorPool(d, gamma_pool_, nullptr);
  if (depth_pipeline_) f.vkDestroyPipeline(d, depth_pipeline_, nullptr);
  if (depth_pipeline_layout_) f.vkDestroyPipelineLayout(d, depth_pipeline_layout_, nullptr);
  if (depth_layout_) f.vkDestroyDescriptorSetLayout(d, depth_layout_, nullptr);
  if (gamma_layout_) f.vkDestroyDescriptorSetLayout(d, gamma_layout_, nullptr);
  for (GammaFrame& g : gamma_frames_) {
    DestroyBuffer(device_, g.lut);
    DestroyImage(device_, g.image);
  }
  if (batch_pool_) f.vkDestroyCommandPool(d, batch_pool_, nullptr);
  if (submit_pool_) f.vkDestroyCommandPool(d, submit_pool_, nullptr);
  for (auto& [module, pipeline] : memexport_pipelines_) {
    if (pipeline) f.vkDestroyPipeline(d, pipeline, nullptr);
  }
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline) f.vkDestroyPipeline(d, pipeline, nullptr);
  }
  if (pipeline_cache_) f.vkDestroyPipelineCache(d, pipeline_cache_, nullptr);
  for (auto& [key, shader] : shaders_) {
    for (auto& [modification, module] : shader->modules) {
      if (module) f.vkDestroyShaderModule(d, module, nullptr);
    }
  }
  if (pool_) f.vkDestroyDescriptorPool(d, pool_, nullptr);
  for (auto& [key, layout] : pipeline_layouts_) f.vkDestroyPipelineLayout(d, layout, nullptr);
  for (auto& [key, layout] : texture_layouts_) f.vkDestroyDescriptorSetLayout(d, layout, nullptr);
  for (VkDescriptorSetLayout l : {shared_layout_, constants_layout_, empty_layout_}) {
    if (l) f.vkDestroyDescriptorSetLayout(d, l, nullptr);
  }
  textures_.reset();
  DestroyBuffer(device_, shared_memory_);
  DestroyBuffer(device_, vram_guest_);
  DestroyBuffer(device_, memexport_readback_);
  for (auto& [key, surface] : surfaces_) {
    if (surface->depth_view) f.vkDestroyImageView(d, surface->depth_view, nullptr);
    DestroyImage(device_, surface->image);
    DestroyImage(device_, surface->ms);
  }
  for (auto& [base, r] : resolved_) DestroyImage(device_, r.image), DestroyImage(device_, r.small), DestroyImage(device_, r.spare),
                                        DestroyImage(device_, r.spare_small);
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
  // Compute too: a memory-exporting vertex shader without rasterization runs as a compute dispatch.
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  constant_alignment_ = std::max<uint32_t>(16, uint32_t(device_.properties().limits.minUniformBufferOffsetAlignment));

  // The translated shaders' layout (as the reference Vulkan backend builds it): shared memory, the five constant
  // buffers as dynamic uniform buffers, vertex and pixel textures (none yet).
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  if (f.vkCreateDescriptorSetLayout(d, &info, nullptr, &empty_layout_) != VK_SUCCESS) return Fail("empty set layout");
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stages, nullptr};
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
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
        if (late_resolve_reads_) {
          if (r.stale) QueueWriteBack(base, r);  // lands once its batch has run: the texture reads the frame before
          continue;
        }
        if (r.stale) {
          WriteBackResolved(base, r);
        } else {
          Flush();  // its write is recorded in a batch: done once that has run
          WaitIdle();
        }
      }
    }
  });
  textures_->SetRecorder([this] {
    VkCommandBuffer cb = Record();
    EndRendering();  // transfers and barriers are recorded outside rendering
    cb = Record();
    GpuMark(batch(), cb, kGpuTextureCopies);
    return cb;
  });
  // Texture sets: allocated per draw, each batch's pool reset once the batch has run.
  VkDescriptorPoolSize draw_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDrawDescriptors}, {VK_DESCRIPTOR_TYPE_SAMPLER, kDrawDescriptors}};
  VkDescriptorPoolCreateInfo draw_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  draw_pool_info.maxSets = kDrawSets;
  draw_pool_info.poolSizeCount = 2;
  draw_pool_info.pPoolSizes = draw_sizes;
  for (Batch& b : batches_) {
    if (f.vkCreateDescriptorPool(d, &draw_pool_info, nullptr, &b.draw_pool) != VK_SUCCESS) return Fail("texture descriptor pool");
  }

  // Guest memory for the shaders: a copy in video memory, its pages filled from each batch's staging buffer (copied
  // on the CPU at the flush) as draws fetch them. Not imported into the device (VK_EXT_external_memory_host): the
  // title's own code ran ~1.4x slower on imported memory (measured in game on the same route). Without video memory
  // for it: imported in place, or else a mirror copied before each draw.
  vram_guest_on_ = CreateBuffer(device_, mirror_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, vram_guest_);
  shared_memory_imported_ = !vram_guest_on_ && ImportGuestMemory();
  if (!shared_memory_imported_ && !vram_guest_on_ && !CreateBuffer(device_, mirror_bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, shared_memory_)) {
    return Fail("guest memory buffer");
  }
  // Each batch's rings: constants and indices of every draw until the next flush.
  for (Batch& b : batches_) {
    if (!CreateBuffer(device_, 32u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, b.constants) ||
        !CreateBuffer(device_, 16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, b.indices)) {
      return Fail("buffers");
    }
  }

  // Descriptor sets, written once: dynamic offsets select each draw's constants.
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount * kBatches}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 2 + kBatches;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &pool_) != VK_SUCCESS) return Fail("descriptor pool");
  VkDescriptorSetLayout to_allocate[2 + kBatches] = {shared_layout_, empty_layout_};
  for (uint32_t i = 0; i < kBatches; ++i) to_allocate[2 + i] = constants_layout_;
  VkDescriptorSet allocated[2 + kBatches];
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool_;
  alloc.descriptorSetCount = 2 + kBatches;
  alloc.pSetLayouts = to_allocate;
  if (f.vkAllocateDescriptorSets(d, &alloc, allocated) != VK_SUCCESS) return Fail("descriptor sets");
  shared_set_ = allocated[0];
  empty_set_ = allocated[1];
  for (uint32_t i = 0; i < kBatches; ++i) batches_[i].constants_set = allocated[2 + i];
  VkDescriptorBufferInfo shared_info{vram_guest_on_ ? vram_guest_.buffer : shared_memory_.buffer, 0, VK_WHOLE_SIZE};
  const uint32_t ranges[SpirvShaderTranslator::kConstantBufferCount] = {
      uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)), kFloatBufferBytes, kFloatBufferBytes, kBoolLoopBytes, kFetchBytes};
  const VkWriteDescriptorSet shared_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, shared_set_, 0, 0, 1,
                                          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &shared_info, nullptr};
  f.vkUpdateDescriptorSets(d, 1, &shared_write, 0, nullptr);
  for (Batch& b : batches_) {
    VkDescriptorBufferInfo constant_infos[SpirvShaderTranslator::kConstantBufferCount];
    VkWriteDescriptorSet writes[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      constant_infos[i] = {b.constants.buffer, 0, ranges[i]};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, b.constants_set, i, 0, 1,
                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &constant_infos[i], nullptr};
    }
    f.vkUpdateDescriptorSets(d, SpirvShaderTranslator::kConstantBufferCount, writes, 0, nullptr);
  }

  // The batches: a command buffer and a fence each; the completion thread that waits for them.
  VkCommandPoolCreateInfo batch_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  batch_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  batch_pool.queueFamilyIndex = device_.queue_family();
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (f.vkCreateCommandPool(d, &batch_pool, nullptr, &batch_pool_) != VK_SUCCESS) return Fail("batch objects");
  VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queries.queryCount = 4 * kBatches;  // a batch's start and end, its upload's start and end
  if (f.vkCreateQueryPool(d, &queries, nullptr, &timestamps_) != VK_SUCCESS) timestamps_ = VK_NULL_HANDLE;
  queries.queryCount = kGpuMarks * kBatches;
  if (f.vkCreateQueryPool(d, &queries, nullptr, &gpu_marks_) != VK_SUCCESS) gpu_marks_ = VK_NULL_HANDLE;
  VkQueryPoolCreateInfo occlusion{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  occlusion.queryType = VK_QUERY_TYPE_OCCLUSION;
  occlusion.queryCount = kQueriesPerBatch * kBatches;
  if (f.vkCreateQueryPool(d, &occlusion, nullptr, &query_pool_) != VK_SUCCESS) query_pool_ = VK_NULL_HANDLE;
  for (Batch& b : batches_) {
    if (f.vkCreateFence(d, &fence, nullptr, &b.fence) != VK_SUCCESS) return Fail("batch objects");
    VkCommandBufferAllocateInfo batch_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    batch_alloc.commandPool = batch_pool_;
    batch_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    batch_alloc.commandBufferCount = 1;
    if (f.vkAllocateCommandBuffers(d, &batch_alloc, &b.cb) != VK_SUCCESS ||
        f.vkAllocateCommandBuffers(d, &batch_alloc, &b.upload_cb) != VK_SUCCESS) {
      return Fail("batch command buffer");
    }
  }
  completer_ = std::thread([this] { Complete(); });
  // Depth/stencil surfaces: D24S8 where the device has it (NVIDIA), else D32S8 (AMD).
  depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkFormatProperties props;
  f.vkGetPhysicalDeviceFormatProperties(device_.physical(), VK_FORMAT_D24_UNORM_S8_UINT, &props);
  if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
  // Host MSAA: the most samples up to the asked count that colour, depth and stencil targets all have.
  {
    const VkPhysicalDeviceLimits& limits = device_.properties().limits;
    const VkSampleCountFlags counts =
        limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts & limits.framebufferStencilSampleCounts;
    uint32_t n = std::bit_floor(std::clamp(msaa_, 1u, 8u));
    while (n > 1 && !(counts & n)) n >>= 1;
    samples_ = VkSampleCountFlagBits(n);
  }
  // The pipeline cache, from the file of an earlier run when it is this device's and driver's (its header says).
  std::vector<uint8_t> cache_data;
  if (!pipeline_cache_path_.empty()) {
    if (FILE* file = std::fopen(pipeline_cache_path_.c_str(), "rb")) {
      std::fseek(file, 0, SEEK_END);
      const long bytes = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (bytes > 0) {
        cache_data.resize(size_t(bytes));
        if (std::fread(cache_data.data(), 1, cache_data.size(), file) != cache_data.size()) cache_data.clear();
      }
      std::fclose(file);
    }
    VkPipelineCacheHeaderVersionOne header{};
    const VkPhysicalDeviceProperties& p = device_.properties();
    if (cache_data.size() < sizeof(header)) {
      cache_data.clear();
    } else {
      std::memcpy(&header, cache_data.data(), sizeof(header));
      if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header.vendorID != p.vendorID ||
          header.deviceID != p.deviceID || std::memcmp(header.pipelineCacheUUID, p.pipelineCacheUUID, VK_UUID_SIZE)) {
        cache_data.clear();
      }
    }
  }
  VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  cache_info.initialDataSize = cache_data.size();
  cache_info.pInitialData = cache_data.data();
  if (f.vkCreatePipelineCache(d, &cache_info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
    cache_info.initialDataSize = 0;
    if (f.vkCreatePipelineCache(d, &cache_info, nullptr, &pipeline_cache_) != VK_SUCCESS) pipeline_cache_ = VK_NULL_HANDLE;
  }
  if (async_pipelines_) {
    // A few threads: pipelines come in bursts (a new area), and the title's own threads keep the rest busy.
    const uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
    for (uint32_t i = 0; i < threads; ++i) compilers_.emplace_back([this] { CompilePipelines(); });
  }
  return true;
}

void VulkanRenderer::ReadsGuestMemory(uint32_t address, uint32_t bytes) {
  if ((shared_memory_imported_ && !vram_guest_on_) || !bytes || address >= mirror_bytes_) return;
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
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // k_2_10_10_10 (and AS_10_10_10_10)
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

VulkanRenderer::Surface* VulkanRenderer::GetSurface(const SurfaceKey& requested) {
  // The AS_ formats are the same EDRAM data as their base format (the reference's GetBaseColorFormat):
  // k_2_10_10_10_AS_10_10_10_10 is k_2_10_10_10, k_2_10_10_10_FLOAT_AS_16_16_16_16 is k_2_10_10_10_FLOAT. One
  // surface for both, or what a title draws as one is not there when it resolves or draws as the other.
  SurfaceKey key = requested;
  if (!key.depth && key.format == 10) key.format = 2;
  if (!key.depth && key.format == 12) key.format = 3;
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
  // Host pixels: the resolution scale's along each axis (the key stays in guest samples).
  // Colour surfaces are also sampled: the gamma pass reads the presented frame from its surface.
  // Depth surfaces too: depth resolves read them on the GPU.
  const VkImageUsageFlags usage = (key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageAspectFlags aspect =
      key.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  if (!CreateImage2D(device_, key.pitch * scale_, height * scale_, format, usage, aspect, slot->image)) {
    DestroyImage(device_, slot->image);
    return nullptr;
  }
  // Host MSAA: draws render into a multisampled image, resolved into this one when it is read.
  if (samples_ > VK_SAMPLE_COUNT_1_BIT && !CreateSamples(*slot)) {
    DestroyImage(device_, slot->image);
    return nullptr;
  }
  ++stats_.render_targets;
  return slot.get();
}

void VulkanRenderer::ResolveSamples(VkCommandBuffer cb, Surface& s, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  // What was drawn within what is read (a draw's scissor usually spans the whole surface, far more than it covers).
  const int32_t left = std::max({s.drawn[0], x0, 0}), top = std::max({s.drawn[1], y0, 0});
  const int32_t right = std::min({s.drawn[2], x1, int32_t(s.image.width)});
  const int32_t bottom = std::min({s.drawn[3], y1, int32_t(s.image.height)});
  if (!s.ms.image || right <= left || bottom <= top) return;
  // What stays to resolve: nothing once the read covers all that was drawn; less when it covers a band across it
  // (the rows or columns it spans); else all of it (resolved again when read).
  int32_t* d = s.drawn;
  if (x0 <= d[0] && x1 >= d[2]) {
    if (y0 <= d[1]) d[1] = std::max(d[1], y1);
    if (y1 >= d[3]) d[3] = std::min(d[3], y0);
  } else if (y0 <= d[1] && y1 >= d[3]) {
    if (x0 <= d[0]) d[0] = std::max(d[0], x1);
    if (x1 >= d[2]) d[2] = std::min(d[2], x0);
  }
  if (d[2] <= d[0] || d[3] <= d[1]) d[0] = d[1] = INT32_MAX, d[2] = d[3] = INT32_MIN;
  ++stats_.sample_resolves;
  stats_.sample_resolve_pixels += uint64_t(right - left) * uint64_t(bottom - top);
  const bool in_batch = batch_open_ && cb == current_piece_;  // not the presenter's command buffer
  if (in_batch) GpuMark(batch(), cb, kGpuSampleResolves);
  const Functions& f = device_.fn();
  const bool depth = s.key.depth != 0;
  const VkImageLayout attachment =
      depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  // After the passes that drew the samples (a pass reads and writes its attachments: nothing orders two passes).
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  f.vkCmdPipelineBarrier2(cb, &dependency);
  Transition(cb, s.ms, s.ms_layout, attachment, aspect);
  Transition(cb, s.image, s.layout, attachment, aspect);
  // An empty pass whose attachment resolves at its end: colour averaged, depth and stencil sample 0.
  VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  a.imageView = s.ms.view;
  a.imageLayout = attachment;
  a.resolveMode = depth ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
  a.resolveImageView = s.image.view;
  a.resolveImageLayout = attachment;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  // The samples are read, never written: not stored back (STORE would write the whole area again, as much traffic
  // as the load). The resolve attachment is written whatever the store op.
  a.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = depth ? 0 : 1;
  rendering.pColorAttachments = depth ? nullptr : &a;
  rendering.pDepthAttachment = depth ? &a : nullptr;
  rendering.pStencilAttachment = depth ? &a : nullptr;
  f.vkCmdBeginRendering(cb, &rendering);
  f.vkCmdEndRendering(cb);
  f.vkCmdPipelineBarrier2(cb, &dependency);  // the passes after it may draw the samples again
}

bool VulkanRenderer::CreateSamples(Surface& s) {
  const VkImageUsageFlags usage =
      (s.key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const VkImageAspectFlags aspect =
      s.key.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  if (!CreateImage2D(device_, s.image.width, s.image.height, s.image.format, usage, aspect, s.ms, samples_)) {
    DestroyImage(device_, s.ms);
    return false;
  }
  s.ms_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  s.drawn[0] = s.drawn[1] = INT32_MAX, s.drawn[2] = s.drawn[3] = INT32_MIN;
  return true;
}

void VulkanRenderer::GiveSamplesBack(Surface* s, const int32_t area[4]) {
  if (!s || s->ms.image || !s->paired || samples_ == VK_SAMPLE_COUNT_1_BIT) return;
  const int32_t* w = s->written;
  if (w[2] > w[0] && w[3] > w[1] && (area[0] > w[0] || area[1] > w[1] || area[2] < w[2] || area[3] < w[3])) return;
  if (!CreateSamples(*s)) return;
  target_memo_.valid = false;  // its pipeline had one sample
  ++stats_.samples_given_back;
}

void VulkanRenderer::DropSamples(Surface& s) {
  if (!s.ms.image) return;
  EndRendering();
  ResolveSamples(Record(), s, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
  batch().retired.push_back(s.ms);  // the batch may still draw into it: freed after it has run
  s.ms = Image{};
  s.ms_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  target_memo_.valid = false;  // its pipeline had the samples
  ++stats_.samples_dropped;
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
    if (s->ms.image) {
      Transition(cb, s->ms, s->ms_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      f.vkCmdClearColorImage(cb, s->ms.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
      s->drawn[0] = s->drawn[1] = INT32_MAX, s->drawn[2] = s->drawn[3] = INT32_MIN;
    }
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
    if (s->ms.image) {
      Transition(cb, s->ms, s->ms_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspects);
      f.vkCmdClearDepthStencilImage(cb, s->ms.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
      s->drawn[0] = s->drawn[1] = INT32_MAX, s->drawn[2] = s->drawn[3] = INT32_MIN;
    }
  });
}

bool VulkanRenderer::ReadBack(const SurfaceKey& key, uint32_t width, uint32_t height, std::vector<uint8_t>& bytes) {
  Flush();  // what was drawn before comes first
  WaitIdle();  // and guest memory has what the batches write into it
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
    ResolveSamples(cb, *s, 0, 0, int32_t(width), int32_t(height));
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
  const uint32_t stage = source.type & 1;
  if (last_shader_[stage] && last_shader_hash_[stage] == source.hash) return last_shader_[stage];
  auto& slot = shaders_[{source.type, source.hash}];
  last_shader_hash_[stage] = source.hash;
  if (slot) return last_shader_[stage] = slot.get();
  slot = std::make_unique<TranslatedShader>();
  const rg::xenos::ShaderType type = source.type ? rg::xenos::ShaderType::kPixel : rg::xenos::ShaderType::kVertex;
  slot->shader = std::make_unique<rg::SpirvShader>(type, source.hash, source.microcode.data(), source.microcode.size(),
                                                   std::endian::native);
  rex::string::StringBuffer disassembly;
  slot->shader->AnalyzeUcode(disassembly);
  return last_shader_[stage] = slot.get();
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

VkPipeline VulkanRenderer::GetPipeline(const PipelineKey& key, bool& pending) {
  pending = false;
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  if (compilers_.empty()) {
    const VkPipeline pipeline = CreatePipeline(key);
    if (pipeline) {
      ++stats_.pipelines;
      pipelines_[key] = pipeline;
    }
    return pipeline;
  }
  // Queued unless it already is. Waited for a moment when just queued (a pipeline the driver has in its caches takes
  // far less), twice a frame at most: the draws after it, and a burst of new pipelines, are left out instead.
  std::unique_lock lock(compile_mutex_);
  TakeCompiledPipelines();
  it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  if (compiling_.insert(key).second) {
    compile_queue_.push_back(key);
    compile_cv_.notify_one();
    if (pipeline_waits_this_frame_ < 2) {
      ++pipeline_waits_this_frame_;
      compiled_cv_.wait_for(lock, std::chrono::milliseconds(1), [&] {
        return std::any_of(compiled_.begin(), compiled_.end(), [&](const auto& c) { return c.first == key; });
      });
    }
  }
  TakeCompiledPipelines();
  it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  pending = true;
  ++stats_.pipeline_waits;
  return VK_NULL_HANDLE;
}

void VulkanRenderer::TakeCompiledPipelines() {
  for (const auto& [key, pipeline] : compiled_) {
    compiling_.erase(key);
    pipelines_[key] = pipeline;
    if (pipeline) ++stats_.pipelines;
  }
  compiled_.clear();
}

void VulkanRenderer::CompilePipelines() {
  std::unique_lock lock(compile_mutex_);
  for (;;) {
    compile_cv_.wait(lock, [this] { return compile_stop_ || !compile_queue_.empty(); });
    if (compile_stop_) return;
    const PipelineKey key = compile_queue_.front();
    compile_queue_.pop_front();
    lock.unlock();
    const VkPipeline pipeline = CreatePipeline(key);
    lock.lock();
    compiled_.emplace_back(key, pipeline);
    ++compiled_total_;
    compiled_cv_.notify_all();
    // The file written once a burst is over (the title may end without the renderer's destructor).
    if (compile_queue_.empty() && compiled_total_ >= compiled_saved_ + 16) {
      compiled_saved_ = compiled_total_;
      lock.unlock();
      SavePipelineCache();
      lock.lock();
    }
  }
}

void VulkanRenderer::SavePipelineCache() {
  if (!pipeline_cache_ || pipeline_cache_path_.empty()) return;
  std::lock_guard lock(save_mutex_);
  const Functions& f = device_.fn();
  size_t bytes = 0;
  if (f.vkGetPipelineCacheData(device_.device(), pipeline_cache_, &bytes, nullptr) != VK_SUCCESS || !bytes) return;
  std::vector<uint8_t> data(bytes);
  const VkResult result = f.vkGetPipelineCacheData(device_.device(), pipeline_cache_, &bytes, data.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) return;  // incomplete: what fits, still valid
  // Written beside the file, then moved over it: a run cut short leaves the previous file whole.
  std::error_code ec;
  const std::filesystem::path path(pipeline_cache_path_);
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const std::string temp = pipeline_cache_path_ + ".tmp";
  FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file) return;
  const bool written = std::fwrite(data.data(), 1, bytes, file) == bytes;
  if (std::fclose(file) == 0 && written) std::filesystem::rename(temp, path, ec);
}

VkPipeline VulkanRenderer::CreatePipeline(const PipelineKey& key) {
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
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples ? key.samples : 1);
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
  if (device_.fn().vkCreateGraphicsPipelines(device_.device(), pipeline_cache_, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

void VulkanRenderer::Draw(const DrawCall& draw) {
  Timer draw_timer(stats_.ns_draws);
  const bool timing = draw_timer.on;
  auto section = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  auto mark = [&](uint64_t& total) {
    if (!timing) return;
    const auto now = std::chrono::steady_clock::now();
    total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - section).count());
    section = now;
  };
  const Functions& f = device_.fn();
  const uint32_t* regs = draw.registers;
  // EDRAM mode: 4 colour and depth, 5 depth only; anything else leaves the targets alone.
  const uint32_t edram_mode = regs[kRbModeControl] & 7;
  if (edram_mode != 4 && edram_mode != 5) {
    if (MemExportWithoutRasterization(draw)) return;  // a vertex shader exporting to memory runs anyway
    ++stats_.skipped;
    return;
  }
  if (!draw.vertex_shader || draw.vertex_shader->microcode.empty()) {
    Fail("no vertex shader");
    return;
  }
  // Rectangle lists are drawn as two-triangle strips whose fourth corner the vertex shader makes, quad lists as
  // triangle lists (a, b, c, a, c, d), as the reference does without geometry shaders; line loops as line strips
  // back to their first vertex; point lists as two-triangle strips too, each the point's square of
  // PA_SU_POINT_SIZE (or the size the vertex shader writes), as the reference does without geometry shaders.
  const bool rectangle_list = draw.primitive_type == 0x08, quad_list = draw.primitive_type == 0x0D,
             line_loop = draw.primitive_type == 0x0C, point_list = draw.primitive_type == 0x01;
  const VkPrimitiveTopology topology = rectangle_list || point_list ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                       : quad_list    ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                       : line_loop    ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                                                      : Topology(draw.primitive_type);
  const uint32_t primitives = rectangle_list ? draw.index_count / 3
                              : quad_list    ? draw.index_count / 4
                              : line_loop || point_list ? draw.index_count
                                                        : 0;
  const uint32_t host_index_count = rectangle_list || point_list ? (primitives ? 5 * primitives - 1 : 0)
                                    : quad_list    ? 6 * primitives
                                    : line_loop    ? (primitives ? primitives + 1 : 0)
                                                   : draw.index_count;
  if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
    Fail("primitive type " + std::to_string(draw.primitive_type) + " needs expansion (not yet)");
    return;
  }
  // Immediate indices (source 1: the packet's own dwords after the initiator) are drawn as DMA indices staged in the
  // batch's index buffer: two 16-bit indices a dword, the first in its low half, as an R6xx-family VGT takes them
  // (Mesa's r600 driver copies little-endian user indices straight into its DRAW_INDEX_IMMD packets). The reference
  // does not draw them. Rectangle and point lists, whose vertex shader loads the guest indices from memory, refuse
  // them.
  const bool immediate = draw.source == DrawSource::kImmediate, indexed = immediate || draw.source == DrawSource::kDma;
  const uint32_t index_bytes = immediate ? draw.index_count * (draw.index_32 ? 4 : 2) : draw.index_bytes;
  if (immediate && (rectangle_list || point_list)) {
    Fail("immediate indices for a rectangle or point list");
    return;
  }
  if (immediate && size_t(index_bytes) > draw.immediate.size() * 4) {
    Fail("immediate indices past the packet");
    return;
  }
  const bool polygonal = IsPolygonal(topology) && !point_list;
  // Primitive reset (PA_SU_SC_MODE_CNTL multi_prim_ib_ena, VGT_MULTI_PRIM_IB_RESET_INDX): strips of DMA indices are
  // cut at the reset index, as the reference does (16-bit indices only when the reset index fits in 16 bits).
  const uint32_t reset_index = regs[kVgtMultiPrimIbResetIndx];
  const bool strip = topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                     topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  const bool restart = rectangle_list || point_list || (indexed && strip && ((regs[kPaSuScModeCntl] >> 21) & 1) &&
                                          (draw.index_32 || reset_index <= UINT16_MAX));

  const bool wants_ps = edram_mode == 4 && draw.pixel_shader && !draw.pixel_shader->microcode.empty();
  // User clip planes (PA_CL_CLIP_CNTL ucp_ena_0-5, unless clip_disable): the vertex shader writes a clip (or, with
  // ucp_cull_only_ena, cull) distance for each enabled one, as the reference translates them.
  const uint32_t clip_cntl = regs[kPaClClipCntl];
  const uint32_t clip_planes = (clip_cntl >> 16) & 1 ? 0 : clip_cntl & 0x3F, clip_cull = (clip_cntl >> 17) & 1;
  // The guest MSAA mode and SQ_INTERPOLATOR_CNTL's sampling pattern choose the centroid interpolators.
  const uint32_t guest_msaa = (regs[kRbSurfaceInfo] >> 16) & 3;
  const uint64_t shader_key[4] = {draw.vertex_shader->hash, wants_ps ? draw.pixel_shader->hash : 0,
                                  uint64_t(regs[kSqProgramCntl]) << 32 | regs[kSqContextMisc],
                                  uint64_t(wants_ps) | uint64_t(rectangle_list) << 1 |
                                      uint64_t(point_list) << 2 |
                                      uint64_t(clip_planes) << 3 | uint64_t(clip_cull) << 9 |
                                      uint64_t(guest_msaa) << 10 | uint64_t(regs[kSqInterpolatorCntl] >> 16) << 12};
  const bool shaders_known = shader_memo_.valid && std::memcmp(shader_memo_.key, shader_key, sizeof(shader_key)) == 0;
  TranslatedShader* vs = shaders_known ? shader_memo_.vs : GetShader(*draw.vertex_shader);
  TranslatedShader* ps = shaders_known ? shader_memo_.ps : wants_ps ? GetShader(*draw.pixel_shader) : nullptr;
  if (!vs || (wants_ps && !ps)) {
    Fail("shader analysis");
    return;
  }
  std::vector<MemExport> exports = MemExportRanges(regs, vs, ps);
  // The vertex buffers the vertex shader fetches from (vertex fetch constants: address in dword 0, size in words in
  // dword 1, and as far again as VGT_INDX_OFFSET moves the indices), for the copy into the mirror when guest memory
  // is not read in place.
  if (!shared_memory_imported_ || vram_guest_on_) {
    const uint64_t index_offset = regs[kVgtIndxOffset] & 0xFFFFFF;
    for (const auto& binding : vs->shader->vertex_bindings()) {
      const uint32_t* fetch = regs + reg::kFetchConstants + 2 * binding.fetch_constant;
      const uint64_t bytes = uint64_t((fetch[1] >> 2) & 0xFFFFFF) * 4 + index_offset * binding.stride_words * 4;
      ReadsGuestMemory(fetch[0] & 0x1FFFFFFC, uint32_t(std::min<uint64_t>(bytes, mirror_bytes_)));
    }
  }
  // The translations for this draw: the interpolators the vertex shader writes and the pixel shader reads
  // (SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC), and the register counts dynamic addressing may reach.
  VkShaderModule vs_module = shader_memo_.vs_module, ps_module = shader_memo_.ps_module;
  if (!shaders_known) {
  rg::reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = regs[kSqProgramCntl];
  rg::reg::SQ_CONTEXT_MISC context_misc;
  context_misc.value = regs[kSqContextMisc];
  uint32_t interpolators = 0;
  // Pixel parameter generation (SQ_PROGRAM_CNTL gen_index_pix): the pixel shader reads, in one of its interpolators,
  // the pixel position, whether the primitive is back-facing, a line or a point, and the point coordinates. The
  // translator makes it from the fragment coordinates and the primitive flags of the system constants; on points, the
  // point sprite coordinates come from the vertex shader's point expansion (0 to 1 across the square), as the
  // reference passes them.
  uint32_t param_gen = UINT32_MAX;
  if (ps) {
    interpolators = vs->shader->writes_interpolators() & ps->shader->GetInterpolatorInputMask(program_cntl, context_misc, param_gen);
  }
  SpirvShaderTranslator::Modification vs_modification(translator_->GetDefaultVertexShaderModification(
      vs->shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rectangle_list ? rg::Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
      : point_list   ? rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip
                     : rg::Shader::HostVertexShaderType::kVertex));
  vs_modification.vertex.interpolator_mask = interpolators;
  vs_modification.vertex.user_clip_plane_count = uint32_t(std::popcount(clip_planes));
  vs_modification.vertex.user_clip_plane_cull = clip_planes ? clip_cull : 0;
  vs_modification.vertex.output_point_parameters = uint32_t(point_list && param_gen < 16);
  vs_module = GetModule(*vs, vs_modification.value);
  ps_module = VK_NULL_HANDLE;
  if (ps) {
    SpirvShaderTranslator::Modification ps_modification(
        translator_->GetDefaultPixelShaderModification(ps->shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    ps_modification.pixel.interpolator_mask = interpolators;
    ps_modification.pixel.interpolators_centroid =
        CentroidInterpolators(interpolators, regs[kRbSurfaceInfo], regs[kSqContextMisc], regs[kSqInterpolatorCntl]);
    if (param_gen < 16) {
      ps_modification.pixel.param_gen_enable = 1;
      ps_modification.pixel.param_gen_interpolator = param_gen;
      ps_modification.pixel.param_gen_point = uint32_t(point_list);
    }
    ps_module = GetModule(*ps, ps_modification.value);
  }
  if (!vs_module || (ps && !ps_module)) {
    Fail("shader translation");
    return;
  }
  std::memcpy(shader_memo_.key, shader_key, sizeof(shader_key));
  shader_memo_.vs = vs, shader_memo_.ps = ps;
  shader_memo_.vs_module = vs_module, shader_memo_.ps_module = ps_module;
  shader_memo_.valid = true;
  }

  // Textures and samplers of each stage, in the order the translated shader binds them.
  StageTextures (&stage_textures)[2] = stage_textures_;
  for (StageTextures& st : stage_textures) st.views.clear(), st.samplers.clear();
  uint32_t swizzled_signs[8] = {};
  uint32_t textures_scaled = 0;  // fetch constants bound to resolution-scaled textures
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
      uint32_t signs = 0;
      bool scaled = false;
      TextureMemo* memo =
          binding.fetch_constant < 32 ? &texture_memo_[(binding.fetch_constant * 3 + uint32_t(view_type)) * 2 + signed_view] : nullptr;
      if (memo && memo->view && !textures_->changes_queued() && memo->version == textures_->version() &&
          (textures_->watching() || memo->epoch == textures_->epoch()) && std::memcmp(memo->fetch, fetch, sizeof(memo->fetch)) == 0) {
        view = memo->view;
        signs = memo->signs;
        if (memo->used) *memo->used = textures_->epoch();
        scaled = memo->scaled;
      } else {
        {
          Timer texture_timer(stats_.ns_textures);
          view = textures_->View(fetch, view_type, error, signed_view);
          scaled = textures_->last_view_scaled();
        }
        if (!view) {
          Fail("texture " + std::to_string(binding.fetch_constant) + ": " + error);
          return;
        }
        // The signs the shader applies after the swizzle (unsigned is 0), a byte per fetch constant.
        const TextureFetch t = TextureFetch::Parse(fetch);
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t component = (t.swizzle >> (3 * i)) & 7;
          if (component < 4) signs |= ((t.signs >> (2 * component)) & 3) << (2 * i);
        }
        if (memo) {
          std::memcpy(memo->fetch, fetch, sizeof(memo->fetch));
          memo->version = textures_->version(), memo->epoch = textures_->epoch();
          memo->view = view, memo->signs = signs, memo->scaled = scaled;
          memo->used = textures_->last_used();
        }
      }
      stage_textures[stage].views.push_back(view);
      swizzled_signs[binding.fetch_constant / 4] |= signs << (8 * (binding.fetch_constant % 4));
      if (scaled && binding.fetch_constant < 32) textures_scaled |= 1u << binding.fetch_constant;
    }
    for (const auto& binding : shader->shader->GetSamplerBindingsAfterTranslation()) {
      const uint32_t* fetch = regs + reg::kFetchConstants + 6 * binding.fetch_constant;
      const uint32_t key[10] = {fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], uint32_t(binding.mag_filter),
                                uint32_t(binding.min_filter), uint32_t(binding.mip_filter), uint32_t(binding.aniso_filter)};
      SamplerMemo* memo = binding.fetch_constant < 32 ? &sampler_memo_[binding.fetch_constant] : nullptr;
      VkSampler sampler;
      if (memo && memo->sampler && std::memcmp(memo->key, key, sizeof(key)) == 0) {
        sampler = memo->sampler;
      } else {
        sampler = textures_->Sampler(fetch, key[6], key[7], key[8], key[9]);
        if (memo && sampler) std::memcpy(memo->key, key, sizeof(key)), memo->sampler = sampler;
      }
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
  const uint32_t layout_counts[4] = {uint32_t(stage_textures[0].views.size()), uint32_t(stage_textures[0].samplers.size()),
                                     uint32_t(stage_textures[1].views.size()), uint32_t(stage_textures[1].samplers.size())};
  if (!layout_memo_.layout || std::memcmp(layout_memo_.counts, layout_counts, sizeof(layout_counts))) {
    layout_memo_.layout = PipelineLayout(layout_counts[0], layout_counts[1], layout_counts[2], layout_counts[3]);
    std::memcpy(layout_memo_.counts, layout_counts, sizeof(layout_counts));
  }
  const VkPipelineLayout pipeline_layout = layout_memo_.layout;
  if (!pipeline_layout) {
    Fail("pipeline layout");
    return;
  }

  mark(stats_.ns_draw_setup);
  // The surfaces: colour targets the pixel shader writes and RB_COLOR_MASK enables, the depth/stencil target when
  // the depth or stencil test is on; all at the pitch of RB_SURFACE_INFO.
  // An MSAA surface is drawn as its samples: EDRAM holds a 4x surface of pitch P like a 1x surface of pitch 2P
  // with twice the rows (2x: the same pitch, twice the rows), so the title's MSAA draws (its fast clears among them)
  // reach the same host image as its 1x draws to that EDRAM. Coordinates are scaled to samples; resolves pick or
  // average a pixel's samples.
  const uint32_t surface_info = regs[kRbSurfaceInfo];
  const uint32_t mode_cntl = regs[kPaSuScModeCntl];
  // Everything the targets, the scissor and the pipeline come from (the surfaces stay while the renderer does).
  uint32_t target_key[TargetMemo::kWords] = {
      surface_info, regs[kRbDepthControl], regs[kRbColorMask], regs[kRbColorInfo[0]], regs[kRbColorInfo[1]],
      regs[kRbColorInfo[2]], regs[kRbColorInfo[3]], regs[kRbDepthInfo], regs[kPaScWindowOffset],
      regs[kPaScWindowScissorTl], regs[kPaScWindowScissorBr], regs[kPaScScreenScissorTl], regs[kPaScScreenScissorBr],
      mode_cntl, regs[kRbBlendControl[0]], regs[kRbBlendControl[1]], regs[kRbBlendControl[2]], regs[kRbBlendControl[3]],
      uint32_t(topology), uint32_t(restart), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  std::memcpy(target_key + 20, &vs_module, 8);
  std::memcpy(target_key + 22, &ps_module, 8);
  std::memcpy(target_key + 24, &pipeline_layout, 8);
  std::memcpy(target_key + 26, &ps, sizeof(ps));
  static_assert(sizeof(ps) <= 8 && 28 <= TargetMemo::kWords);
  Surface* colors[4] = {};
  uint32_t color_infos[4] = {};
  Surface* depth_surface = nullptr;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  int32_t left, top, right, bottom;
  uint32_t depth_control;
  VkPipeline pipeline;
  const int32_t msaa_x = ((surface_info >> 16) & 3) == 2 ? 2 : 1, msaa_y = ((surface_info >> 16) & 3) ? 2 : 1;
  // Host pixels a guest pixel: its samples, each scale x scale pixels (the scale is the renderer's: not in the key).
  const int32_t sample_x = msaa_x * int32_t(scale_), sample_y = msaa_y * int32_t(scale_);
  const uint32_t window_offset = regs[kPaScWindowOffset];
  const int32_t offset_x = int32_t(window_offset << 17) >> 17, offset_y = int32_t(window_offset << 1) >> 17;
  for (uint32_t i = 0; i < 4; ++i) color_infos[i] = regs[kRbColorInfo[i]];
  if (target_memo_.valid && std::memcmp(target_memo_.key, target_key, sizeof(target_key)) == 0) {
    for (uint32_t i = 0; i < 4; ++i) colors[i] = static_cast<Surface*>(target_memo_.colors[i]);
    depth_surface = static_cast<Surface*>(target_memo_.depth);
    width = target_memo_.width, height = target_memo_.height, depth_control = target_memo_.depth_control;
    left = target_memo_.left, top = target_memo_.top, right = target_memo_.right, bottom = target_memo_.bottom;
    pipeline = target_memo_.pipeline;
  } else {
  const uint32_t msaa = (surface_info >> 16) & 3;
  if (msaa > 2) {
    Fail("MSAA mode 3");
    return;
  }
  const uint32_t pitch = (surface_info & 0x3FFF) * uint32_t(msaa_x);
  depth_control = regs[kRbDepthControl];
  // Depth tested always without writes does nothing (the reference drops it the same way).
  if ((depth_control & 2) && !(depth_control & 4) && ((depth_control >> 4) & 7) == 7) depth_control &= ~2u;
  if (!polygonal) depth_control &= ~(1u << 7);  // back-face stencil only for polygons
  const uint32_t color_mask_register = regs[kRbColorMask];
  for (uint32_t i = 0; i < 4; ++i) {
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
    if (MemExportWithoutRasterization(draw)) return;
    ++stats_.skipped;  // nothing bound: no target this draw could change
    return;
  }
  // Host MSAA where it smooths edges. A depth target drawn without colour targets through a whole frame (a shadow
  // map; a depth pre-pass is drawn with them in the same frame) goes back to one sample at the swap, as do the
  // multisampled targets of a pass that mixes both kinds (their samples resolved first: nothing lost).
  VkSampleCountFlagBits pass_samples = VK_SAMPLE_COUNT_1_BIT;
  if (samples_ > VK_SAMPLE_COUNT_1_BIT) {
    Surface* const targets[5] = {colors[0], colors[1], colors[2], colors[3], depth_surface};
    const bool paired = depth_surface && (colors[0] || colors[1] || colors[2] || colors[3]);
    bool multisampled = false, single = false;
    for (Surface* t : targets) {
      if (!t) continue;
      if (paired) {
        t->paired = true;
      } else if (t == depth_surface) {
        t->unpaired_this_frame = true;
      }
      (t->ms.image ? multisampled : single) = true;
    }
    if (multisampled && single) {
      for (Surface* t : targets) {
        if (t && t->ms.image) DropSamples(*t);
      }
      multisampled = false;
    }
    if (multisampled) pass_samples = samples_;
  }

  // The scissor: the window scissor (moved by the window offset unless it says not to) within the screen scissor
  // and the targets.
  const uint32_t window_tl = regs[kPaScWindowScissorTl], window_br = regs[kPaScWindowScissorBr];
  const bool offset_scissor = !(window_tl >> 31);
  left = int32_t(window_tl & 0x3FFF) + (offset_scissor ? offset_x : 0);
  top = int32_t((window_tl >> 16) & 0x3FFF) + (offset_scissor ? offset_y : 0);
  right = int32_t(window_br & 0x3FFF) + (offset_scissor ? offset_x : 0);
  bottom = int32_t((window_br >> 16) & 0x3FFF) + (offset_scissor ? offset_y : 0);
  left = std::max({left, int32_t(regs[kPaScScreenScissorTl] & 0x7FFF), 0}) * sample_x;
  top = std::max({top, int32_t((regs[kPaScScreenScissorTl] >> 16) & 0x7FFF), 0}) * sample_y;
  right = std::min(std::min(right, int32_t(regs[kPaScScreenScissorBr] & 0x7FFF)) * sample_x, int32_t(width));
  bottom = std::min(std::min(bottom, int32_t((regs[kPaScScreenScissorBr] >> 16) & 0x7FFF)) * sample_y, int32_t(height));
  if (right <= left || bottom <= top) {
    if (MemExportWithoutRasterization(draw)) return;
    ++stats_.skipped;
    return;
  }

  // The pipeline state from the registers.
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
  key.samples = uint32_t(pass_samples);
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED;
    key.blend_controls[i] = colors[i] ? (regs[kRbBlendControl[i]] & 0x1FFF1FFF) : kNoBlending;
    if (colors[i]) key.color_masks |= color_mask_register & (0xFu << (4 * i));
  }
  bool pending;
  {
    Timer pipeline_timer(stats_.ns_pipelines);
    pipeline = GetPipeline(key, pending);
  }
  if (pending) return;  // drawn once its pipeline is ready
  if (!pipeline) {
    Fail("pipeline creation");
    return;
  }
  std::memcpy(target_memo_.key, target_key, sizeof(target_key));
  for (uint32_t i = 0; i < 4; ++i) target_memo_.colors[i] = colors[i];
  target_memo_.depth = depth_surface;
  target_memo_.width = width, target_memo_.height = height, target_memo_.depth_control = depth_control;
  target_memo_.left = left, target_memo_.top = top, target_memo_.right = right, target_memo_.bottom = bottom;
  target_memo_.pipeline = pipeline;
  target_memo_.valid = true;
  }

  // Constants, each at an aligned offset of the batch's ring: system, VS floats, PS floats, bool/loop, fetch. A draw
  // takes at most this much, and every range the descriptors read (up to 4 KB) must stay inside the buffer.
  const uint32_t most = uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)) + 2 * kFloatBufferBytes + kBoolLoopBytes +
                        kFetchBytes + 5 * constant_alignment_ + kFloatBufferBytes;
  // Everything this draw takes from the batch, checked up front: a flush once its constants are written would let
  // later draws overwrite them.
  const uint32_t index_need = rectangle_list || quad_list || line_loop || point_list ? host_index_count * 4 + 4
                              : indexed ? index_bytes + 4
                                        : 0;
  const uint32_t immediate_need = immediate ? index_bytes + 4 : 0;  // the packet's indices staged too
  const uint32_t images_need = uint32_t(stage_textures[0].views.size() + stage_textures[1].views.size());
  const uint32_t samplers_need = uint32_t(stage_textures[0].samplers.size() + stage_textures[1].samplers.size());
  if (constant_head_ + most > batch().constants.size || index_head_ + index_need + immediate_need > batch().indices.size || draw_sets_ + 2 > kDrawSets ||
      (query_open_ && queries_used_ >= kQueriesPerBatch) ||
      draw_images_ + images_need > kDrawDescriptors || draw_samplers_ + samplers_need > kDrawDescriptors) {
    Flush();
  }
  draw_images_ += images_need;
  draw_samplers_ += samplers_need;
  mark(stats_.ns_draw_targets);
  uint8_t* base = static_cast<uint8_t*>(batch().constants.mapped);
  uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
  uint32_t& at = constant_head_;
  // Each buffer: built in ordinary memory, then written into the batch's buffer unless it is the block the last
  // draw of the batch wrote (that one is bound again).
  static_assert(SpirvShaderTranslator::kConstantBufferCount <= 8);
  auto put = [&](uint32_t index, const void* data, uint32_t bytes) {
    LastConstants& last = last_constants_[index];
    if (last.valid && last.bytes.size() == bytes && std::memcmp(last.bytes.data(), data, bytes) == 0) {
      offsets[index] = last.offset;
      return;
    }
    offsets[index] = at;
    std::memcpy(base + at, data, bytes);
    at = AlignUp(at + bytes, constant_alignment_);
    if (data == float_scratch_.data() && bytes == float_scratch_.size()) {
      std::swap(last.bytes, float_scratch_);  // packed for this draw only: kept without a copy
    } else {
      last.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    }
    last.offset = offsets[index];
    last.valid = true;
  };
  // System constants: the guest viewport transform (PA_CL_VTE_CNTL, PA_CL_VPORT_*) folded into the NDC scale and
  // offset for a host viewport covering the target.
  SpirvShaderTranslator::SystemConstants system_constants{};
  auto* system = &system_constants;
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
  // Points: PA_SU_POINT_SIZE (half width and height) and PA_SU_POINT_MINMAX (the radius a vertex shader's size is
  // clamped to), both 12.4 fixed point, as diameters in guest pixels; and how a guest pixel diameter becomes a
  // radius in host normalized device coordinates (a guest pixel is `sample` host pixels).
  if (point_list) {
    const uint32_t point_size = regs[kPaSuPointSize], point_minmax = regs[kPaSuPointMinmax];
    system->point_constant_diameter[0] = float(point_size >> 16) * 0.125f;
    system->point_constant_diameter[1] = float(point_size & 0xFFFF) * 0.125f;
    system->point_vertex_diameter_min = float(point_minmax & 0xFFFF) * 0.125f;
    system->point_vertex_diameter_max = float(point_minmax >> 16) * 0.125f;
    system->point_screen_diameter_to_ndc_radius[0] = float(sample_x) / size[0];
    system->point_screen_diameter_to_ndc_radius[1] = float(sample_y) / size[1];
  }
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
  // A rectangle or point list's vertex shader loads its guest indices itself (from the strip vertex it runs for).
  if ((rectangle_list || point_list) && draw.source == DrawSource::kDma) {
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
  system->textures_resolution_scaled = textures_scaled;
  // Alpha test (RB_COLORCONTROL): the compare function's less / equal / greater bits are the pass flags; a disabled
  // test is "always" (with none of them set, every pixel would be discarded).
  const uint32_t color_control = regs[kRbColorControl];
  const uint32_t alpha_func = (color_control & (1u << 3)) ? (color_control & 7) : 7;
  system->flags |= alpha_func << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  system->alpha_test_reference = AsFloat(regs[kRbAlphaRef]);
  // Alpha to coverage (RB_COLORCONTROL alpha_to_mask_enable, the dither offsets in bits 24-31) as the reference
  // passes it, with the guest MSAA mode that places each fragment's guest sample.
  system->alpha_to_mask = (color_control & (1u << 4)) ? (color_control >> 24) | (1u << 8) : 0;
  system->flags |= ((regs[kRbSurfaceInfo] >> 16) & 3) << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // The enabled user clip planes (PA_CL_UCP_n), packed in order.
  for (uint32_t bits = clip_planes, n = 0; bits; bits &= bits - 1, ++n) {
    std::memcpy(system->user_clip_planes[n], regs + kPaClUcp0X + 4 * std::countr_zero(bits), 16);
  }
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
  put(SpirvShaderTranslator::kConstantBufferSystem, system, sizeof(*system));
  // Float constants, packed as each shader uses them (all 256 when it addresses them dynamically).
  auto pack_floats = [&](uint32_t index, const TranslatedShader* shader, uint32_t first_register) {
    // Only the constants the shader uses take room (the descriptor's 4 KB range reads past them harmlessly).
    const auto& map = shader ? shader->shader->constant_register_map() : vs->shader->constant_register_map();
    const uint32_t count = !shader ? 1 : map.float_dynamic_addressing ? 256 : std::max(1u, map.float_count);
    if (!shader) {
      static constexpr uint8_t kZero[16] = {};
      put(index, kZero, 16);
      return;
    }
    if (map.float_dynamic_addressing) {
      put(index, regs + first_register, 256 * 16);
      return;
    }
    float_scratch_.resize(16 * size_t(count));
    uint8_t* out = float_scratch_.data();
    // The registers the shader uses, set bit by set bit.
    uint32_t packed = 0;
    for (uint32_t word = 0; word < 4; ++word) {
      for (uint64_t bits = map.float_bitmap[word]; bits; bits &= bits - 1) {
        const uint32_t c = 64 * word + uint32_t(std::countr_zero(bits));
        std::memcpy(out + 16 * packed++, regs + first_register + 4 * c, 16);
      }
    }
    if (!packed) std::memset(out, 0, 16);
    put(index, out, 16 * count);
  };
  pack_floats(SpirvShaderTranslator::kConstantBufferFloatVertex, vs, 0x4000);
  pack_floats(SpirvShaderTranslator::kConstantBufferFloatPixel, ps, 0x4000 + 256 * 4);
  put(SpirvShaderTranslator::kConstantBufferBoolLoop, regs + 0x4900, kBoolLoopBytes);
  put(SpirvShaderTranslator::kConstantBufferFetch, regs + 0x4800, kFetchBytes);

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

  // The indices: room in the batch's ring, reserved here; converted (endian-swapped to host order) where the draw
  // is recorded.
  DrawJob job{};
  job.query = UINT32_MAX;
  job.index_type = VK_INDEX_TYPE_NONE_KHR;
  job.host_index_count = host_index_count;
  job.index_buffer = batch().indices.buffer;
  job.source = indexed ? DrawSource::kDma : draw.source;
  job.index_32 = draw.index_32;
  job.endian = immediate ? Endian::kNone : draw.index_endian;
  // The immediate indices staged where the batch keeps them until it completes (jobs may be recorded later).
  const uint8_t* index_src = physical_ + draw.index_base;
  if (immediate) {
    uint8_t* staged = static_cast<uint8_t*>(batch().indices.mapped) + index_head_;
    std::memcpy(staged, draw.immediate.data(), index_bytes);
    index_src = staged;
    index_head_ = AlignUp(index_head_ + index_bytes, 4);
  }
  job.restart = restart;
  job.reset_index = reset_index;
  job.primitives = primitives;
  if (rectangle_list || quad_list || line_loop || point_list) {
    if (draw.source == DrawSource::kDma && uint64_t(draw.index_base) + draw.index_bytes > mirror_bytes_) {
      Fail("index buffer outside the mirrored memory");
      return;
    }
    job.indices = rectangle_list || point_list ? DrawJob::Indices::kRectangles
                  : quad_list    ? DrawJob::Indices::kQuads
                                 : DrawJob::Indices::kLineLoop;
    job.index_type = VK_INDEX_TYPE_UINT32;
    job.index_offset = index_head_;
    index_head_ = AlignUp(index_head_ + host_index_count * 4, 4);
  } else if (indexed) {
    if ((!immediate && uint64_t(draw.index_base) + draw.index_bytes > mirror_bytes_) || index_bytes > batch().indices.size) {
      Fail("index buffer outside the mirrored memory");
      return;
    }
    job.indices = DrawJob::Indices::kDma;
    job.index_type = draw.index_32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    job.index_offset = index_head_;
    index_head_ = AlignUp(index_head_ + index_bytes, 4);
  } else {
    job.indices = DrawJob::Indices::kNone;
  }
  job.index_src = index_src;
  job.index_bytes = index_bytes;
  job.index_dst = static_cast<uint8_t*>(batch().indices.mapped) + job.index_offset;
  if (draw.source == DrawSource::kDma && !rectangle_list && !point_list && draw.index_endian != Endian::kNone) {
    CountSwapped(g_swapped.indices, draw.index_bytes);
  }
  mark(stats_.ns_draw_constants);
  // The texture sets of both stages: sampled images, then samplers (empty sets for a stage without textures).
  VkDescriptorSet texture_sets[2] = {empty_set_, empty_set_};
  // With recording threads, they make the sets (the bindings in the job).
  bool sets_on_threads = !workers_.empty();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    sets_on_threads &= stage_textures[stage].views.size() <= DrawJob::kMaxBindings &&
                       stage_textures[stage].samplers.size() <= DrawJob::kMaxBindings;
  }
  for (uint32_t stage = 0; stage < 2 && sets_on_threads; ++stage) {
    const StageTextures& st = stage_textures[stage];
    job.texture_layouts[stage] = VK_NULL_HANDLE;
    if (st.views.empty() && st.samplers.empty()) continue;
    job.texture_layouts[stage] = TextureSetLayout(stage == 0, uint32_t(st.views.size()), uint32_t(st.samplers.size()));
    job.view_count[stage] = uint8_t(st.views.size());
    job.sampler_count[stage] = uint8_t(st.samplers.size());
    std::copy(st.views.begin(), st.views.end(), job.views[stage]);
    std::copy(st.samplers.begin(), st.samplers.end(), job.samplers[stage]);
  }
  for (uint32_t stage = 0; stage < 2 && !sets_on_threads; ++stage) {
    const StageTextures& st = stage_textures[stage];
    if (st.views.empty() && st.samplers.empty()) continue;
    VkDescriptorSetLayout layout = TextureSetLayout(stage == 0, uint32_t(st.views.size()), uint32_t(st.samplers.size()));
    std::vector<uint64_t>& set_key = set_key_;
    set_key.clear();
    set_key.push_back(uint64_t(layout));
    for (VkImageView v : st.views) set_key.push_back(uint64_t(v));
    for (VkSampler s : st.samplers) set_key.push_back(uint64_t(s));
    if (const auto known = batch_sets_.find(set_key); known != batch_sets_.end()) {
      texture_sets[stage] = known->second;
      continue;
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = batch().draw_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    if (f.vkAllocateDescriptorSets(device_.device(), &alloc, &texture_sets[stage]) != VK_SUCCESS) {
      Fail("texture descriptor set");
      return;
    }
    ++draw_sets_;
    std::vector<VkDescriptorImageInfo>& infos = set_infos_;
    std::vector<VkWriteDescriptorSet>& writes = set_writes_;
    infos.resize(st.views.size() + st.samplers.size());
    writes.resize(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
      const bool is_image = i < st.views.size();
      infos[i] = is_image ? VkDescriptorImageInfo{VK_NULL_HANDLE, st.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                          : VkDescriptorImageInfo{st.samplers[i - st.views.size()], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, texture_sets[stage], uint32_t(i), 0, 1,
                   is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, &infos[i], nullptr, nullptr};
    }
    f.vkUpdateDescriptorSets(device_.device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    batch_sets_.emplace(set_key, texture_sets[stage]);  // a copy: the key's buffer stays for the next draw
  }

  if (query_open_) {
    job.query = kQueriesPerBatch * current_ + queries_used_++;
    query_parts_.push_back(job.query);
  }
  job.pipeline = pipeline;
  job.layout = pipeline_layout;
  job.sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_set_;
  job.sets[SpirvShaderTranslator::kDescriptorSetConstants] = batch().constants_set;
  job.sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = texture_sets[0];
  job.sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = texture_sets[1];
  std::memcpy(job.offsets, offsets, sizeof(offsets[0]) * SpirvShaderTranslator::kConstantBufferCount);
  job.viewport = {0, 0, size[0], size[1], 0, 1};
  job.scissor = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
  job.depth_bias[0] = bias_offset, job.depth_bias[1] = bias_scale;
  std::memcpy(job.blend, blend_constants, sizeof(job.blend));
  job.stencil[0] = stencil_front, job.stencil[1] = stencil_back;

  {
    Timer submit_timer(stats_.ns_submit);
    const VkCommandBuffer cb = Record();
    // The render pass: kept open while this draw's targets are the open pass's; otherwise ended, the targets moved
    // to attachment layouts (outside any pass) and a new one begun.
    const std::array<uint64_t, 7> key = {
        colors[0] ? uint64_t(colors[0]->target().view) : 0, colors[1] ? uint64_t(colors[1]->target().view) : 0,
        colors[2] ? uint64_t(colors[2]->target().view) : 0, colors[3] ? uint64_t(colors[3]->target().view) : 0,
        depth_surface ? uint64_t(depth_surface->target().view) : 0, width, height};
    for (uint32_t i = 0; i < 4; ++i) GrowDrawn(colors[i], left, top, right, bottom);
    GrowDrawn(depth_surface, left, top, right, bottom);
    bool layouts_ready = true;
    for (uint32_t i = 0; i < 4; ++i) layouts_ready &= !colors[i] || colors[i]->target_layout() == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    layouts_ready &= !depth_surface || depth_surface->target_layout() == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (rendering_open_ && (key != rendering_key_ || !layouts_ready)) EndRendering();
    if (!rendering_open_) {
      VkRenderingAttachmentInfo color[4];
      for (uint32_t i = 0; i < 4; ++i) {
        color[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        segment_formats_[i] = colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED;
        if (!colors[i]) continue;  // no view: the location is unused
        Transition(cb, colors[i]->target(), colors[i]->target_layout(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT);
        color[i].imageView = colors[i]->target().view;
        color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      segment_depth_ = depth_surface ? depth_surface->image.format : VK_FORMAT_UNDEFINED;
      Surface* first = depth_surface;
      for (uint32_t i = 0; i < 4 && !first; ++i) first = colors[i];
      segment_samples_ = first->ms.image ? samples_ : VK_SAMPLE_COUNT_1_BIT;  // the pass's targets all alike
      if (depth_surface) {
        Transition(cb, depth_surface->target(), depth_surface->target_layout(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth.imageView = depth_surface->target().view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      // With recording threads, the pass's draws come in secondary command buffers.
      segment_secondary_ = !workers_.empty();
      rendering.flags = segment_secondary_ ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0;
      rendering.renderArea = {{0, 0}, {width, height}};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = 4;
      rendering.pColorAttachments = color;
      rendering.pDepthAttachment = depth_surface ? &depth : nullptr;
      rendering.pStencilAttachment = depth_surface ? &depth : nullptr;
      if (segment_secondary_) {
        // Recorded at the flush, around the chunks: this piece ends here, the next serial commands go after the pass.
        f.vkEndCommandBuffer(cb);
        piece_open_ = false;
        if (segment_count_ == segments_.size()) segments_.emplace_back();
        Segment& s = segments_[segment_count_];
        std::memcpy(s.color, color, sizeof(color));
        s.depth = depth;
        s.has_depth = depth_surface != nullptr;
        s.area = rendering.renderArea;
        s.tasks.clear();
        order_.push_back({VK_NULL_HANDLE, int32_t(segment_count_++)});
      } else {
        GpuMark(batch(), cb, kGpuDraws);
        f.vkCmdBeginRendering(cb, &rendering);
      }
      rendering_open_ = true;
      rendering_key_ = key;
    }
    if (segment_secondary_) {
      if (!open_task_) {
        if (!spare_tasks_.empty()) {
          open_task_ = std::move(spare_tasks_.back());
          spare_tasks_.pop_back();
        } else {
          open_task_ = std::make_unique<RecordTask>();
        }
        open_task_->jobs.clear();
        open_task_->batch = current_;
        open_task_->done = false;
        std::memcpy(open_task_->formats, segment_formats_, sizeof(segment_formats_));
        open_task_->depth = segment_depth_;
        open_task_->samples = segment_samples_;
      }
      open_task_->jobs.push_back(job);
      if (open_task_->jobs.size() >= kChunkDraws) DispatchChunk();
    } else {
      RecordJob(cb, job, recorded_, index_scratch_);
    }
  }
  mark(stats_.ns_draw_record);
  ++stats_.draws;
  if (!exports.empty()) MemExported(std::move(exports));
}

std::vector<VulkanRenderer::MemExport> VulkanRenderer::MemExportRanges(const uint32_t* regs, const TranslatedShader* vs,
                                                                       const TranslatedShader* ps) const {
  // Memory exports (memexport): the streams the shaders' eA writes name (`mad eA, r#, const0100, c#`: the stream
  // constant c# of the stage's float constants), as the reference gathers them, a range each (several exports to one
  // base address: the largest), checked as the hardware checks eA (the 0b01, 0x4B0 and 0x96 marks, a count).
  std::vector<MemExport> exports;
  for (const TranslatedShader* t : {vs, ps}) {

    if (!t || !t->shader->memexport_eM_written()) continue;
    const uint32_t* floats = regs + 0x4000 + (t == ps ? 256 * 4 : 0);
    for (uint32_t c : t->shader->memexport_stream_constants()) {
      if (c >= 256) continue;
      const uint32_t* stream = floats + 4 * c;
      const uint32_t count = stream[3] & 0x7FFFFF, bytes = MemExportElementBytes((stream[2] >> 8) & 0x3F);
      if ((stream[0] >> 30) != 1 || (stream[2] >> 20) != 0x4B0 || (stream[3] >> 23) != 0x96 || !count || !bytes) continue;
      const uint32_t base = (stream[0] & 0x3FFFFFFF) << 2;
      if (base >= mirror_bytes_) continue;
      const uint32_t size = uint32_t(std::min<uint64_t>(uint64_t(count) * bytes, mirror_bytes_ - base));
      bool merged = false;
      for (MemExport& e : exports) {
        if (e.base == base) e.bytes = std::max(e.bytes, size), merged = true;
      }
      if (!merged) exports.push_back({base, size, 0});
    }
  }
  return exports;
}

bool VulkanRenderer::MemExportWithoutRasterization(const DrawCall& draw) {
  if (!draw.vertex_shader || draw.vertex_shader->microcode.empty()) return false;
  TranslatedShader* vs = GetShader(*draw.vertex_shader);
  if (!vs || !vs->shader->memexport_eM_written()) return false;
  const uint32_t* regs = draw.registers;
  std::vector<MemExport> exports = MemExportRanges(regs, vs, nullptr);
  if (exports.empty() || !draw.index_count) return true;  // nothing it could write: as skipped
  if (draw.source == DrawSource::kImmediate) {
    Fail("memexport without rasterization from immediate indices (not yet)");
    return true;
  }
  if (!vs->shader->texture_bindings().empty()) {
    Fail("memexport without rasterization from a vertex shader with textures (not yet)");
    return true;
  }
  const Functions& f = device_.fn();
  rg::reg::SQ_PROGRAM_CNTL program_cntl;
  program_cntl.value = regs[kSqProgramCntl];
  SpirvShaderTranslator::Modification modification(translator_->GetDefaultVertexShaderModification(
      vs->shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rg::Shader::HostVertexShaderType::kMemExportCompute));
  const VkShaderModule module = GetModule(*vs, modification.value);
  const VkPipelineLayout layout = PipelineLayout(0, 0, 0, 0);
  VkPipeline& pipeline = memexport_pipelines_[module];
  if (module && layout && !pipeline) {
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
    info.layout = layout;
    if (f.vkCreateComputePipelines(device_.device(), pipeline_cache_, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    }
  }
  if (!pipeline) {
    Fail("memexport compute pipeline");
    return true;
  }
  // The guest memory it reads: its vertex buffers and DMA indices.
  const uint64_t index_offset = regs[kVgtIndxOffset] & 0xFFFFFF;
  for (const auto& binding : vs->shader->vertex_bindings()) {
    const uint32_t* fetch = regs + reg::kFetchConstants + 2 * binding.fetch_constant;
    const uint64_t bytes = uint64_t((fetch[1] >> 2) & 0xFFFFFF) * 4 + index_offset * binding.stride_words * 4;
    ReadsGuestMemory(fetch[0] & 0x1FFFFFFC, uint32_t(std::min<uint64_t>(bytes, mirror_bytes_)));
  }
  // Constants, as a draw's: the system ones (the vertex index source, the vertex count), the vertex shader's floats
  // packed as it uses them, a pixel float block, bool/loop and fetch constants.
  const auto& map = vs->shader->constant_register_map();
  const uint32_t float_bytes = map.float_dynamic_addressing ? 256 * 16 : 16 * std::max(1u, map.float_count);
  const uint32_t need = uint32_t(sizeof(SpirvShaderTranslator::SystemConstants)) + float_bytes + 16 + kBoolLoopBytes +
                        kFetchBytes + 6 * constant_alignment_ + kFloatBufferBytes;
  if (constant_head_ + need > batch().constants.size) Flush();
  uint8_t* base = static_cast<uint8_t*>(batch().constants.mapped);
  uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
  auto put = [&](uint32_t index, const void* data, uint32_t bytes) {
    offsets[index] = constant_head_;
    std::memcpy(base + constant_head_, data, bytes);
    constant_head_ = AlignUp(constant_head_ + bytes, constant_alignment_);
    last_constants_[index].valid = false;
  };
  SpirvShaderTranslator::SystemConstants system{};
  if (draw.source == DrawSource::kDma) {
    system.flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (draw.index_32) system.flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    system.vertex_index_load_address = draw.index_base;
    ReadsGuestMemory(draw.index_base, draw.index_count * (draw.index_32 ? 4 : 2));
    Endian endian = draw.index_endian;
    if (!draw.index_32) endian = endian == Endian::k8in32 ? Endian::k8in16 : endian == Endian::k16in32 ? Endian::kNone : endian;
    system.vertex_index_endian = decltype(system.vertex_index_endian)(uint32_t(endian));
  }
  system.vertex_base_index = int32_t(regs[kVgtIndxOffset]);
  system.vertex_index_min = regs[kVgtMinVtxIndx];
  system.vertex_index_max = regs[kVgtMaxVtxIndx];
  system.compute_memexport_vertex_count = draw.index_count;
  put(SpirvShaderTranslator::kConstantBufferSystem, &system, sizeof(system));
  if (map.float_dynamic_addressing) {
    put(SpirvShaderTranslator::kConstantBufferFloatVertex, regs + 0x4000, 256 * 16);
  } else {
    std::vector<uint8_t> floats(float_bytes, 0);
    uint32_t packed = 0;
    for (uint32_t word = 0; word < 4; ++word) {
      for (uint64_t bits = map.float_bitmap[word]; bits; bits &= bits - 1) {
        const uint32_t c = 64 * word + uint32_t(std::countr_zero(bits));
        std::memcpy(floats.data() + 16 * packed++, regs + 0x4000 + 4 * c, 16);
      }
    }
    put(SpirvShaderTranslator::kConstantBufferFloatVertex, floats.data(), float_bytes);
  }
  static constexpr uint8_t kZero[16] = {};
  put(SpirvShaderTranslator::kConstantBufferFloatPixel, kZero, 16);
  put(SpirvShaderTranslator::kConstantBufferBoolLoop, regs + 0x4900, kBoolLoopBytes);
  put(SpirvShaderTranslator::kConstantBufferFetch, regs + 0x4800, kFetchBytes);
  // Outside any render pass, after the batch's draws so far; one invocation a guest vertex, in rows of the
  // translation's dispatch width.
  EndRendering();
  const VkCommandBuffer cb = Record();
  VkDescriptorSet sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_set_;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = batch().constants_set;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = empty_set_;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = empty_set_;
  f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, SpirvShaderTranslator::kDescriptorSetCount, sets,
                            SpirvShaderTranslator::kConstantBufferCount, offsets);
  const uint32_t width = SpirvShaderTranslator::kMemExportComputeDispatchX;
  f.vkCmdDispatch(cb, std::min(draw.index_count, width), (draw.index_count + width - 1) / width, 1);
  ++memexport_dispatches_;
  MemExported(std::move(exports));
  return true;
}

void VulkanRenderer::MemExported(std::vector<MemExport> exports) {
  // The draw's writes reach guest memory before anything else runs: the batch is submitted with them and awaited
  // (memexport is rare; later draws and the title's code may read what it wrote). Guest memory in video memory: the
  // ranges copied into the readback buffer at the flush, then into guest memory; a mirror: copied back from it;
  // imported: written in place. The texture cache forgets what it held there.
  ++stats_.memexports;
  // The ranges come back whole, elements the shaders did not write included: the GPU's copy of them is made current
  // first (uploaded with the batch), or what the title wrote there since would be undone.
  for (const MemExport& e : exports) ReadsGuestMemory(e.base, e.bytes);
  uint32_t readback_bytes = 0;
  for (MemExport& e : exports) e.readback = readback_bytes, readback_bytes += (e.bytes + 15) & ~15u;
  if (vram_guest_on_ && memexport_readback_.size < readback_bytes) {
    WaitIdle();  // the buffer may still be read by an earlier batch's completion
    DestroyBuffer(device_, memexport_readback_);
    if (!CreateBuffer(device_, std::max<VkDeviceSize>(readback_bytes, 1u << 20), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                      memexport_readback_)) {
      Fail("memexport readback buffer");
      return;
    }
  }
  memexports_ = exports;
  WhenDone([this, exports = std::move(exports)] {
    for (const MemExport& e : exports) {
      if (vram_guest_on_) {
        std::memcpy(physical_ + e.base, static_cast<const uint8_t*>(memexport_readback_.mapped) + e.readback, e.bytes);
      } else if (!shared_memory_imported_) {
        std::memcpy(physical_ + e.base, static_cast<const uint8_t*>(shared_memory_.mapped) + e.base, e.bytes);
      }
      if (textures_) textures_->MemoryChanged(e.base, e.base + e.bytes);
    }
  });
  WaitIdle();
}

void VulkanRenderer::RecordJob(VkCommandBuffer cb, const DrawJob& job, RecordedState& rs, std::vector<uint16_t>& scratch) const {
  const Functions& f = device_.fn();
  // The indices, into the room reserved for them.
  if (job.indices == DrawJob::Indices::kRectangles) {
    // Four strip vertices a rectangle, a restart between rectangles.
    uint32_t* dst = reinterpret_cast<uint32_t*>(job.index_dst);
    for (uint32_t r = 0; r < job.primitives; ++r) {
      if (r) *dst++ = UINT32_MAX;
      for (uint32_t i = 0; i < 4; ++i) *dst++ = 4 * r + i;
    }
  } else if (job.indices == DrawJob::Indices::kQuads || job.indices == DrawJob::Indices::kLineLoop) {
    uint32_t* dst = reinterpret_cast<uint32_t*>(job.index_dst);
    auto guest_index = [&](uint32_t i) -> uint32_t {
      if (job.source != DrawSource::kDma) return i;
      if (job.index_32) {
        uint32_t v;
        std::memcpy(&v, job.index_src + 4 * i, 4);
        return GpuSwap(v, job.endian);
      }
      uint16_t v;
      std::memcpy(&v, job.index_src + 2 * i, 2);
      if (job.endian == Endian::k8in16 || job.endian == Endian::k8in32) v = uint16_t(v << 8 | v >> 8);
      return v;
    };
    if (job.indices == DrawJob::Indices::kLineLoop) {
      for (uint32_t i = 0; i < job.primitives; ++i) *dst++ = guest_index(i);
      if (job.primitives) *dst = guest_index(0);
    } else for (uint32_t q = 0; q < job.primitives; ++q) {
      const uint32_t a = guest_index(4 * q), b = guest_index(4 * q + 1), cc = guest_index(4 * q + 2), d = guest_index(4 * q + 3);
      const uint32_t six[6] = {a, b, cc, a, cc, d};
      std::memcpy(dst, six, sizeof(six));
      dst += 6;
    }
  } else if (job.indices == DrawJob::Indices::kDma) {
    const uint8_t* src = job.index_src;
    uint8_t* dst = job.index_dst;
    const bool restart = job.restart;
    // With primitive reset, the guest's reset index becomes the host's (all ones). Loops without per-index branches
    // where they can (they vectorize): a copy, or a byte swap, and the reset test only when reset is on.
    if (job.index_32) {
      const uint32_t n = job.index_bytes / 4;
      if (job.endian == Endian::kNone && !restart) {
        std::memcpy(dst, src, size_t(n) * 4);
      } else if (job.endian == Endian::k8in32 && !restart) {
        for (uint32_t i = 0; i < n; ++i) {
          uint32_t v;
          std::memcpy(&v, src + 4 * i, 4);
          v = std::byteswap(v);
          std::memcpy(dst + 4 * i, &v, 4);
        }
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          uint32_t v;
          std::memcpy(&v, src + 4 * i, 4);
          v = GpuSwap(v, job.endian);
          if (restart && v == job.reset_index) v = UINT32_MAX;
          std::memcpy(dst + 4 * i, &v, 4);
        }
      }
    } else {
      const uint32_t n = job.index_bytes / 2;
      const bool swap = job.endian == Endian::k8in16 || job.endian == Endian::k8in32;
      if (!swap && !restart) {
        std::memcpy(dst, src, size_t(n) * 2);
      } else if (!restart) {
        for (uint32_t i = 0; i < n; ++i) {
          uint16_t v;
          std::memcpy(&v, src + 2 * i, 2);
          v = uint16_t(v << 8 | v >> 8);
          std::memcpy(dst + 2 * i, &v, 2);
        }
      } else {
        // Converted in ordinary memory (vectorized, one loop per byte order), then copied at once: element stores into
        // the write-combined buffer stall.
        scratch.resize(n);
        uint16_t* out = scratch.data();
        const uint16_t reset = uint16_t(job.reset_index);
        if (swap) {
          for (uint32_t i = 0; i < n; ++i) {
            uint16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            v = uint16_t(v << 8 | v >> 8);
            out[i] = v == reset ? uint16_t(UINT16_MAX) : v;
          }
        } else {
          for (uint32_t i = 0; i < n; ++i) {
            uint16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            out[i] = v == reset ? uint16_t(UINT16_MAX) : v;
          }
        }
        std::memcpy(dst, out, size_t(n) * 2);
      }
    }
  }

  // Only what changed since the last draw recorded into `cb` (the state stays across render passes).
  if (!rs.valid || rs.pipeline != job.pipeline) f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, job.pipeline);
  // From the first set that changed (all with another layout); the constants' set when its offsets changed.
  static_assert(SpirvShaderTranslator::kDescriptorSetConstants == 1 && SpirvShaderTranslator::kDescriptorSetCount == 4);
  uint32_t first_set = 0;
  if (rs.valid && rs.layout == job.layout) {
    first_set = SpirvShaderTranslator::kDescriptorSetCount;
    for (uint32_t i = 0; i < SpirvShaderTranslator::kDescriptorSetCount; ++i) {
      if (rs.sets[i] != job.sets[i]) {
        first_set = i;
        break;
      }
    }
    if (first_set > SpirvShaderTranslator::kDescriptorSetConstants &&
        std::memcmp(rs.offsets, job.offsets, sizeof(job.offsets[0]) * SpirvShaderTranslator::kConstantBufferCount)) {
      first_set = SpirvShaderTranslator::kDescriptorSetConstants;
    }
  }
  if (first_set < SpirvShaderTranslator::kDescriptorSetCount) {
    const bool with_constants = first_set <= SpirvShaderTranslator::kDescriptorSetConstants;
    f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, job.layout, first_set,
                              SpirvShaderTranslator::kDescriptorSetCount - first_set, job.sets + first_set,
                              with_constants ? SpirvShaderTranslator::kConstantBufferCount : 0,
                              with_constants ? job.offsets : nullptr);
    rs.layout = job.layout;
    std::memcpy(rs.sets, job.sets, sizeof(job.sets));
    std::memcpy(rs.offsets, job.offsets, sizeof(job.offsets[0]) * SpirvShaderTranslator::kConstantBufferCount);
  }
  if (!rs.valid || std::memcmp(&rs.viewport, &job.viewport, sizeof(job.viewport))) f.vkCmdSetViewport(cb, 0, 1, &job.viewport);
  if (!rs.valid || std::memcmp(&rs.scissor, &job.scissor, sizeof(job.scissor))) f.vkCmdSetScissor(cb, 0, 1, &job.scissor);
  if (!rs.valid || rs.depth_bias[0] != job.depth_bias[0] || rs.depth_bias[1] != job.depth_bias[1]) {
    f.vkCmdSetDepthBias(cb, job.depth_bias[0], 0.0f, job.depth_bias[1]);
  }
  if (!rs.valid || std::memcmp(rs.blend, job.blend, sizeof(rs.blend))) f.vkCmdSetBlendConstants(cb, job.blend);
  if (!rs.valid || rs.stencil[0] != job.stencil[0]) {
    f.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (job.stencil[0] >> 8) & 0xFF);
    f.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (job.stencil[0] >> 16) & 0xFF);
    f.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, job.stencil[0] & 0xFF);
  }
  if (!rs.valid || rs.stencil[1] != job.stencil[1]) {
    f.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (job.stencil[1] >> 8) & 0xFF);
    f.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (job.stencil[1] >> 16) & 0xFF);
    f.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, job.stencil[1] & 0xFF);
  }
  rs.pipeline = job.pipeline;
  rs.viewport = job.viewport;
  rs.scissor = job.scissor;
  rs.depth_bias[0] = job.depth_bias[0], rs.depth_bias[1] = job.depth_bias[1];
  std::memcpy(rs.blend, job.blend, sizeof(rs.blend));
  rs.stencil[0] = job.stencil[0], rs.stencil[1] = job.stencil[1];
  rs.valid = true;
  if (job.query != UINT32_MAX) {
    f.vkCmdBeginQuery(cb, query_pool_, job.query, device_.occlusion_query_precise() ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
  }
  if (job.index_type != VK_INDEX_TYPE_NONE_KHR) {
    f.vkCmdBindIndexBuffer(cb, job.index_buffer, job.index_offset, job.index_type);
    f.vkCmdDrawIndexed(cb, job.host_index_count, 1, 0, 0, 0);
  } else {
    f.vkCmdDraw(cb, job.host_index_count, 1, 0, 0);
  }
  if (job.query != UINT32_MAX) f.vkCmdEndQuery(cb, query_pool_, job.query);
}

void VulkanRenderer::MakeTextureSets(RecordWorker& w, uint32_t batch, DrawJob& job) const {
  const Functions& f = device_.fn();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const VkDescriptorSetLayout layout = job.texture_layouts[stage];
    if (!layout) continue;
    const uint32_t views = job.view_count[stage], samplers = job.sampler_count[stage];
    std::vector<uint64_t>& key = w.key;
    key.clear();
    key.push_back(uint64_t(layout));
    for (uint32_t i = 0; i < views; ++i) key.push_back(uint64_t(job.views[stage][i]));
    for (uint32_t i = 0; i < samplers; ++i) key.push_back(uint64_t(job.samplers[stage][i]));
    if (const auto known = w.sets[batch].find(key); known != w.sets[batch].end()) {
      job.sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex + stage] = known->second;
      continue;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
      if (w.set_pool_used[batch] == w.set_pools[batch].size()) {
        // Another pool for this batch.
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192}, {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 2048;
        info.poolSizeCount = 2;
        info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (f.vkCreateDescriptorPool(device_.device(), &info, nullptr, &pool) != VK_SUCCESS) return;
        w.set_pools[batch].push_back(pool);
      }
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = w.set_pools[batch][w.set_pool_used[batch]];
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &layout;
      if (f.vkAllocateDescriptorSets(device_.device(), &alloc, &set) == VK_SUCCESS) break;
      ++w.set_pool_used[batch];  // full: the next one
    }
    w.infos.resize(views + samplers);
    w.writes.resize(views + samplers);
    for (uint32_t i = 0; i < views + samplers; ++i) {
      const bool is_image = i < views;
      w.infos[i] = is_image ? VkDescriptorImageInfo{VK_NULL_HANDLE, job.views[stage][i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                            : VkDescriptorImageInfo{job.samplers[stage][i - views], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      w.writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                     is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, &w.infos[i], nullptr, nullptr};
    }
    f.vkUpdateDescriptorSets(device_.device(), views + samplers, w.writes.data(), 0, nullptr);
    w.sets[batch].emplace(key, set);
    job.sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex + stage] = set;
  }
}

void VulkanRenderer::DispatchChunk() {
  if (!open_task_ || open_task_->jobs.empty()) return;
  RecordTask* task = open_task_.get();
  segment_tasks_.push_back(std::move(open_task_));
  bool idle;
  {
    std::lock_guard lock(record_mutex_);
    record_queue_.push_back(task);
    idle = record_idle_ > 0;
  }
  if (idle) record_cv_.notify_one();  // busy threads take it from the queue when they are done
}

void VulkanRenderer::RecordLoop(uint32_t index) {
  RecordWorker& w = *workers_[index];
  const Functions& f = device_.fn();
  for (;;) {
    RecordTask* task;
    {
      std::unique_lock lock(record_mutex_);
      while (!record_stopping_ && record_queue_.empty()) {
        ++record_idle_;
        record_cv_.wait(lock);
        --record_idle_;
      }
      if (record_queue_.empty()) return;
      task = record_queue_.front();
      record_queue_.pop_front();
    }
    // A secondary command buffer of this thread's pool for the task's batch (reused once that batch has run).
    const uint32_t b = task->batch;
    if (w.used[b] == w.buffers[b].size()) {
      VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = w.pools[b];
      alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
      alloc.commandBufferCount = 1;
      VkCommandBuffer cb = VK_NULL_HANDLE;
      f.vkAllocateCommandBuffers(device_.device(), &alloc, &cb);
      w.buffers[b].push_back(cb);
    }
    const VkCommandBuffer cb = w.buffers[b][w.used[b]++];
    // Inside the render pass the thread that prepares draws began, with its attachments' formats.
    VkCommandBufferInheritanceRenderingInfo rendering{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = task->formats;
    rendering.depthAttachmentFormat = task->depth;
    rendering.stencilAttachmentFormat = task->depth;
    rendering.rasterizationSamples = task->samples;
    VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &rendering};
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inheritance;
    f.vkBeginCommandBuffer(cb, &begin);
    RecordedState rs;  // a secondary command buffer starts with no state
    for (const DrawJob& queued : task->jobs) {
      if (queued.texture_layouts[0] || queued.texture_layouts[1]) {
        DrawJob job = queued;
        MakeTextureSets(w, b, job);
        RecordJob(cb, job, rs, w.scratch);
      } else {
        RecordJob(cb, queued, rs, w.scratch);
      }
    }
    f.vkEndCommandBuffer(cb);
    task->cb = cb;
    {
      std::lock_guard lock(record_mutex_);
      task->done.store(true, std::memory_order_release);
    }
    record_done_cv_.notify_all();
  }
}

VkCommandBuffer VulkanRenderer::Record() {
  Batch& b = batch();
  if (!batch_open_) {
    const Functions& f = device_.fn();
    f.vkResetCommandBuffer(b.cb, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(b.cb, &begin);
    // After everything submitted before (batches overlap on the queue otherwise).
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    f.vkCmdPipelineBarrier2(b.cb, &dependency);
    if (query_pool_) f.vkCmdResetQueryPool(b.cb, query_pool_, kQueriesPerBatch * uint32_t(&b - batches_.data()), kQueriesPerBatch);
    if (timestamps_) {
      const uint32_t index = uint32_t(&b - batches_.data());
      f.vkCmdResetQueryPool(b.cb, timestamps_, 4 * index, 2);
      f.vkCmdWriteTimestamp2(b.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, 4 * index);
    }
    if (gpu_marks_) {
      f.vkCmdResetQueryPool(b.cb, gpu_marks_, kGpuMarks * uint32_t(&b - batches_.data()), kGpuMarks);
      b.marks = 0;
      GpuMark(b, b.cb, kGpuOther);
    }
    recorded_.valid = false;  // a new command buffer has no state
    batch_open_ = true;
    b.pieces_used = 0;
    order_.clear();
    order_.push_back({b.cb, -1});
    segment_count_ = 0;
    current_piece_ = b.cb;
    piece_open_ = true;
  } else if (!piece_open_) {
    // After a render pass recorded on the recording threads: a new piece.
    current_piece_ = NextPiece(b);
    order_.push_back({current_piece_, -1});
    piece_open_ = true;
  }
  return current_piece_;
}

VkCommandBuffer VulkanRenderer::NextPiece(Batch& b) {
  const Functions& f = device_.fn();
  if (b.pieces_used == b.pieces.size()) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = batch_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    f.vkAllocateCommandBuffers(device_.device(), &alloc, &cb);
    b.pieces.push_back(cb);
  }
  const VkCommandBuffer cb = b.pieces[b.pieces_used++];
  f.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  f.vkBeginCommandBuffer(cb, &begin);
  return cb;
}

void VulkanRenderer::EndRendering() {
  if (!rendering_open_) return;
  rendering_open_ = false;
  if (segment_secondary_) {
    // The pass's chunks: the last dispatched; the pass is recorded around them at the flush (no wait here).
    DispatchChunk();
    Segment& s = segments_[segment_count_ - 1];
    for (auto& task : segment_tasks_) s.tasks.push_back(std::move(task));
    segment_tasks_.clear();
    segment_secondary_ = false;
    return;
  }
  device_.fn().vkCmdEndRendering(current_piece_);
  GpuMark(batch(), current_piece_, kGpuOther);
}

uint32_t VulkanRenderer::AutoRecordThreads() {
  // Measured on the replay bench, two recording threads take most of the gain (the preparation here is then the
  // limit) and four the rest: two from 6 hardware threads on (8 on a Steam Deck), four from 12; fewer leave the
  // cores to the title's threads and the command processor's.
  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  return hardware >= 12 ? 4u : hardware >= 6 ? 2u : 0u;
}

void VulkanRenderer::SetRecordThreads(uint32_t count) {
  if (!workers_.empty() || !count) return;
  const Functions& f = device_.fn();
  for (uint32_t i = 0; i < count; ++i) {
    auto w = std::make_unique<RecordWorker>();
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = device_.queue_family();
    for (uint32_t b = 0; b < kBatches; ++b) {
      if (f.vkCreateCommandPool(device_.device(), &info, nullptr, &w->pools[b]) != VK_SUCCESS) return;
    }
    workers_.push_back(std::move(w));
  }
  record_threads_ = count;
  for (uint32_t i = 0; i < count; ++i) workers_[i]->thread = std::thread([this, i] { RecordLoop(i); });
  VkCommandPoolCreateInfo submit_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  submit_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  submit_pool.queueFamilyIndex = device_.queue_family();
  if (f.vkCreateCommandPool(device_.device(), &submit_pool, nullptr, &submit_pool_) == VK_SUCCESS) {
    submitter_ = std::thread([this] { SubmitLoop(); });
  }
}

void VulkanRenderer::SubmitLoop() {
  for (;;) {
    uint32_t index;
    {
      std::unique_lock lock(submit_mutex_);
      submit_cv_.wait(lock, [&] { return submit_stopping_ || !to_submit_.empty(); });
      if (to_submit_.empty()) return;
      index = to_submit_.front();
      to_submit_.pop_front();
      submitting_ = true;
      submitting_index_ = index;
    }
    Submit(index);
    {
      std::lock_guard lock(submit_mutex_);
      submitting_ = false;
    }
    submitted_cv_.notify_all();
  }
}

void VulkanRenderer::WaitSubmitted() {
  if (!submitter_.joinable()) return;
  std::unique_lock lock(submit_mutex_);
  submitted_cv_.wait(lock, [&] { return to_submit_.empty() && !submitting_; });
}

void VulkanRenderer::Submit(uint32_t index) {
  const Functions& f = device_.fn();
  Batch& b = batches_[index];
  Submission& s = submissions_[index];
  if (s.segment_count) {
    // The render passes the recording threads drew: their chunks recorded, each pass a buffer around them.
    {
      std::unique_lock lock(record_mutex_);
      record_done_cv_.wait(lock, [&] {
        for (uint32_t i = 0; i < s.segment_count; ++i) {
          for (const auto& task : s.segments[i].tasks) {
            if (!task->done.load(std::memory_order_acquire)) return false;
          }
        }
        return true;
      });
    }
    s.pieces_used = 0;
    for (Entry& entry : s.order) {
      if (entry.segment < 0) continue;
      Segment& seg = s.segments[entry.segment];
      if (s.pieces_used == s.pieces.size()) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = submit_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        f.vkAllocateCommandBuffers(device_.device(), &alloc, &cb);
        s.pieces.push_back(cb);
      }
      entry.cb = s.pieces[s.pieces_used++];
      f.vkResetCommandBuffer(entry.cb, 0);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      f.vkBeginCommandBuffer(entry.cb, &begin);
      GpuMark(b, entry.cb, kGpuDraws);
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
      rendering.renderArea = seg.area;
      rendering.layerCount = 1;
      rendering.colorAttachmentCount = 4;
      rendering.pColorAttachments = seg.color;
      rendering.pDepthAttachment = seg.has_depth ? &seg.depth : nullptr;
      rendering.pStencilAttachment = seg.has_depth ? &seg.depth : nullptr;
      f.vkCmdBeginRendering(entry.cb, &rendering);
      s.segment_cbs.clear();
      for (auto& task : seg.tasks) {
        s.segment_cbs.push_back(task->cb);
        s.spent.push_back(std::move(task));
      }
      seg.tasks.clear();
      if (!s.segment_cbs.empty()) f.vkCmdExecuteCommands(entry.cb, uint32_t(s.segment_cbs.size()), s.segment_cbs.data());
      f.vkCmdEndRendering(entry.cb);
      GpuMark(b, entry.cb, kGpuOther);
      f.vkEndCommandBuffer(entry.cb);
    }
    s.segment_count = 0;
  }
  s.cbs.clear();
  if (s.upload) s.cbs.push_back(b.upload_cb);
  for (const Entry& entry : s.order) s.cbs.push_back(entry.cb);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = uint32_t(s.cbs.size());
  submit.pCommandBuffers = s.cbs.data();
  if (const VkResult result = f.vkQueueSubmit(device_.queue(), 1, &submit, b.fence); result != VK_SUCCESS) {
    // The first failure with its result (and the driver's fault description when the device is lost).
    static bool told = false;
    if (!told) {
      told = true;
      Fail("batch submission failed first with VkResult " + std::to_string(int(result)) +
           (result == VK_ERROR_DEVICE_LOST ? " (device lost; " + device_.DescribeFault() + ")" : std::string()));
    } else {
      Fail("batch submission");
    }
  }
  std::vector<std::function<void()>>& released = released_actions_;
  {
    std::lock_guard lock(done_mutex_);
    b.submitted = true;
    TakeReleased(released);
  }
  for (auto& action : released) action();
  released.clear();
}

void VulkanRenderer::TakeReleased(std::vector<std::function<void()>>& actions) {
  if (!fences_when_captured_) return;
  // In submission order: those in flight, then the one being submitted if Flush has not listed it yet.
  auto release = [&](uint32_t i) {
    Batch& b = batches_[i];
    if (b.done || b.released) return true;
    // Colour resolves it reads back into guest memory do not hold it (they land when the batch has run; the runtime
    // never writes them back at all). Depth resolves do: a title reading depth on the CPU must see this frame's.
    if (!b.submitted || !b.captured) return false;
    for (const PendingWriteBack& w : b.write_backs) {
      if (w.depth) return false;
    }
    b.released = true;
    for (auto& action : b.actions) actions.push_back(std::move(action));
    b.actions.clear();
    return true;
  };
  for (uint32_t i : in_flight_) {
    if (!release(i)) return;
  }
  if (submitted_unlisted_ >= 0) release(uint32_t(submitted_unlisted_));
}

void VulkanRenderer::Flush() {
  if (!batch_open_) return;
  Timer submit_timer(stats_.ns_submit);
  const Functions& f = device_.fn();
  const VkDevice d = device_.device();
  Batch& b = batch();
  EndRendering();
  const VkCommandBuffer last = Record();  // after the last render pass
  if (!memexports_.empty()) {
    // A draw's memory exports: its shader writes visible to the copy into the readback buffer (guest memory in
    // video memory) and to the host.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    f.vkCmdPipelineBarrier2(last, &dependency);
    if (vram_guest_on_) {
      std::vector<VkBufferCopy> regions;
      for (const MemExport& e : memexports_) regions.push_back({e.base, e.readback, e.bytes});
      f.vkCmdCopyBuffer(last, vram_guest_.buffer, memexport_readback_.buffer, uint32_t(regions.size()), regions.data());
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      f.vkCmdPipelineBarrier2(last, &dependency);
    }
    memexports_.clear();
  }
  if (timestamps_) {
    f.vkCmdWriteTimestamp2(last, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, 4 * uint32_t(&b - batches_.data()) + 1);
  }
  f.vkEndCommandBuffer(last);
  piece_open_ = false;
  const uint32_t submitted = current_;
  Submission& submission = submissions_[submitted];
  queries_used_ = 0;
  std::swap(submission.order, order_);
  std::swap(submission.segments, segments_);
  submission.segment_count = segment_count_;
  segment_count_ = 0;
  submitted_at_[uint32_t(&b - batches_.data())] = std::chrono::steady_clock::now();
  // Guest memory the batch's shaders fetch from: read in place, or copied once here (the draws run now, so they
  // see memory as it is now either way).
  if (!shared_memory_imported_ && !vram_guest_on_) {
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
  // Into video memory: the ranges the batch's shaders read, merged (gaps under 64 KB copied along), copied from the
  // imported guest memory by a command buffer that runs just before the batch.
  bool upload = false;
  if (vram_guest_on_ && !batch_ranges_.empty()) {
    std::sort(batch_ranges_.begin(), batch_ranges_.end());
    upload_regions_.clear();
    for (auto [begin, end] : batch_ranges_) {
      begin &= ~3u, end = (end + 3) & ~3u;
      if (!upload_regions_.empty() && begin <= upload_regions_.back().srcOffset + upload_regions_.back().size + (64u << 10)) {
        VkBufferCopy& last = upload_regions_.back();
        last.size = std::max<VkDeviceSize>(last.size, end - last.srcOffset);
      } else {
        upload_regions_.push_back({begin, begin, end - begin});
      }
    }
    batch_ranges_.clear();
    if (vram_valid_) {
      // Only the pages not already current in video memory: marked current, then watched (a write before the watch
      // is armed still lands before the copy runs), copied in runs.
      std::vector<VkBufferCopy>& regions = upload_regions_scratch_;
      regions.swap(upload_regions_);
      upload_regions_.clear();
      for (const VkBufferCopy& region : regions) {
        const uint32_t first = uint32_t(region.srcOffset) >> kGuestPageShift;
        const uint32_t last = uint32_t((region.srcOffset + region.size - 1) >> kGuestPageShift);
        for (uint32_t page = first; page <= last; ++page) {
          std::atomic<uint64_t>& word = vram_valid_[page / 64];
          const uint64_t bit = uint64_t(1) << (page % 64);
          if (word.load(std::memory_order_acquire) & bit) continue;
          const VkDeviceSize at = VkDeviceSize(page) << kGuestPageShift;
          const VkDeviceSize size = std::min<VkDeviceSize>(VkDeviceSize(1) << kGuestPageShift, mirror_bytes_ - at);
          if (!upload_regions_.empty() && upload_regions_.back().srcOffset + upload_regions_.back().size == at) {
            upload_regions_.back().size += size;
          } else {
            upload_regions_.push_back({at, at, size});
          }
          // A page the CPU keeps writing stays stale (copied again by the next batch that reads it), unwatched.
          if (vram_writes_[page].load(std::memory_order_relaxed) >= kHotPageWrites) continue;
          word.fetch_or(bit, std::memory_order_acq_rel);
          if (!watch_runs_.empty() && watch_runs_.back().srcOffset + watch_runs_.back().size == at) {
            watch_runs_.back().size += size;
          } else {
            watch_runs_.push_back({at, at, size});
          }
        }
      }
      // Only the copied runs are watched: the pages already current still are, and protecting a range is a system
      // call for each of the runtime's views of physical memory.
      if (guest_watch_) {
        for (const VkBufferCopy& run : watch_runs_) guest_watch_(uint32_t(run.srcOffset), uint32_t(run.srcOffset + run.size));
      }
      watch_runs_.clear();
    }
  }
  // Draws read video memory only (or nothing new to copy): captured, unless the upload below cannot stage.
  b.captured = vram_guest_on_;
  if (vram_guest_on_ && !upload_regions_.empty()) {
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(b.upload_cb, &begin_info);
    // After the batches before (they may still read these ranges); the batch's own barrier orders its draws after.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    const uint32_t stamp = 4 * uint32_t(&b - batches_.data()) + 2;
    if (timestamps_) {
      f.vkCmdResetQueryPool(b.upload_cb, timestamps_, stamp, 2);
      f.vkCmdWriteTimestamp2(b.upload_cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, stamp);
    }
    f.vkCmdPipelineBarrier2(b.upload_cb, &dependency);
    // Copied from guest memory now, into the batch's staging buffer (the GPU copies from it): the batch reads no guest
    // memory once submitted. Too much for the buffer: copied from guest memory when the batch runs.
    VkDeviceSize total = 0;
    for (const VkBufferCopy& region : upload_regions_) total += region.size;
    if (b.guest_staging.size < total) {
      DestroyBuffer(device_, b.guest_staging);
      if (!CreateBuffer(device_, std::max<VkDeviceSize>(std::bit_ceil(total), 4u << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        true, b.guest_staging)) {
        b.guest_staging = {};
      }
    }
    const bool staged = b.guest_staging.size >= total;
    if (!staged && !shared_memory_imported_) {
      Fail("guest memory staging buffer");  // nothing else to copy from: these pages stay as they were
      upload_regions_.clear();
    }
    if (staged) {
      VkDeviceSize at = 0;
      for (VkBufferCopy& region : upload_regions_) {
        std::memcpy(static_cast<uint8_t*>(b.guest_staging.mapped) + at, physical_ + region.srcOffset, region.size);
        region.srcOffset = at;
        at += region.size;
      }
    }
    b.captured = staged;
    if (!upload_regions_.empty()) {
      f.vkCmdCopyBuffer(b.upload_cb, staged ? b.guest_staging.buffer : shared_memory_.buffer, vram_guest_.buffer,
                        uint32_t(upload_regions_.size()), upload_regions_.data());
    }
    if (timestamps_) f.vkCmdWriteTimestamp2(b.upload_cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps_, stamp + 1);
    f.vkEndCommandBuffer(b.upload_cb);
    for (const VkBufferCopy& region : upload_regions_) stats_.guest_upload_bytes += region.size;
    upload = true;
  }
  if (textures_) textures_->TakeStaging(b.staging);  // the batch's uploads read them
  b.uploaded = upload;
  submission.upload = upload;
  {
    std::lock_guard lock(done_mutex_);
    b.submitted = b.released = false;
    submitted_unlisted_ = int32_t(submitted);
  }
  if (submitter_.joinable()) {
    // Submitted by its thread, once its render passes' chunks are recorded: no wait for them here.
    {
      std::lock_guard lock(submit_mutex_);
      to_submit_.push_back(submitted);
    }
    submit_cv_.notify_one();
  } else {
    Submit(submitted);
  }
  batch_open_ = false;
  {
    std::lock_guard lock(done_mutex_);
    b.done = false;
    in_flight_.push_back(submitted);
    last_submitted_ = int32_t(submitted);
    if (submitted_unlisted_ == int32_t(submitted)) submitted_unlisted_ = -1;
  }
  work_cv_.notify_one();
  if (!shared_memory_imported_ && !vram_guest_on_) WaitBatch(submitted);  // the mirror is rewritten for the next batch
  // The next batch: the oldest of the ring, once the GPU has run it; what it used is free again.
  current_ = (current_ + 1) % kBatches;
  WaitBatch(current_);
  Batch& next = batch();
  f.vkResetFences(d, 1, &next.fence);
  for (auto& task : submissions_[current_].spent) spare_tasks_.push_back(std::move(task));
  submissions_[current_].spent.clear();
  // The recording threads' command buffers for this batch are free again (every chunk of it was recorded before its
  // submission).
  for (auto& w : workers_) {
    if (w->used[current_]) f.vkResetCommandPool(d, w->pools[current_], 0);
    w->used[current_] = 0;
    for (uint32_t i = 0; i < w->set_pools[current_].size() && i <= w->set_pool_used[current_]; ++i) {
      f.vkResetDescriptorPool(d, w->set_pools[current_][i], 0);
    }
    w->set_pool_used[current_] = 0;
    w->sets[current_].clear();
  }
  f.vkResetDescriptorPool(d, next.draw_pool, 0);
  batch_sets_.clear();
  if (next.depth_pool) f.vkResetDescriptorPool(d, next.depth_pool, 0);
  depth_sets_ = 0;
  constant_head_ = index_head_ = 0;
  for (LastConstants& last : last_constants_) last.valid = false;  // the next batch has its own buffer
  draw_sets_ = draw_images_ = draw_samplers_ = 0;
  ++stats_.flushes;
}

void VulkanRenderer::WhenDone(std::function<void()> action) {
  if (batch_open_) {
    // After the draws recorded so far: with the batch that holds them.
    batch().actions.push_back(std::move(action));
    Flush();
    return;
  }
  {
    // Nothing recorded since the last submission: after that one, or now if it has run.
    std::lock_guard lock(done_mutex_);
    if (last_submitted_ >= 0 && !batches_[last_submitted_].done && !batches_[last_submitted_].released) {
      batches_[last_submitted_].actions.push_back(std::move(action));
      return;
    }
  }
  action();
}

void VulkanRenderer::WaitBatch(uint32_t index) {
  std::unique_lock lock(done_mutex_);
  done_cv_.wait(lock, [&] { return batches_[index].done; });
}

void VulkanRenderer::WaitIdle() {
  for (uint32_t i = 0; i < kBatches; ++i) WaitBatch(i);
}

void VulkanRenderer::Complete() {
  for (;;) {
    uint32_t index;
    {
      std::unique_lock lock(done_mutex_);
      work_cv_.wait(lock, [&] { return stopping_ || !in_flight_.empty(); });
      if (in_flight_.empty()) return;
      index = in_flight_.front();
    }
    Batch& b = batches_[index];
    // Not before the submitter thread has submitted it: a fence must not be waited on while vkQueueSubmit uses it.
    if (submitter_.joinable()) {
      std::unique_lock lock(submit_mutex_);
      submitted_cv_.wait(lock, [&] {
        return !(submitting_ && submitting_index_ == index) &&
               std::find(to_submit_.begin(), to_submit_.end(), index) == to_submit_.end();
      });
    }
    device_.fn().vkWaitForFences(device_.device(), 1, &b.fence, VK_TRUE, UINT64_MAX);
    FinishBatch(b);
    // Actions WhenDone attached while these ran, until none is left: then the batch is done.
    for (;;) {
      std::vector<std::function<void()>>& late = finished_actions_;
      {
        std::lock_guard lock(done_mutex_);
        if (b.actions.empty()) {
          b.done = true;
          in_flight_.pop_front();
          TakeReleased(late);  // the captured batches that waited behind this one
          break;
        }
        late.swap(b.actions);
      }
      for (auto& action : late) action();
      late.clear();
    }
    for (auto& action : finished_actions_) action();  // the batches released after it
    finished_actions_.clear();
    done_cv_.notify_all();
  }
}

void VulkanRenderer::OcclusionQuery(bool end, uint32_t address) {
  if (!end) {
    query_open_ = true;  // a begin without an end replaces the open query
    query_parts_.clear();
    return;
  }
  Batch::Query query{address, {}};
  if (query_open_) query.parts.swap(query_parts_);
  query_open_ = false;
  if (batch_open_) {
    batch().queries.push_back(std::move(query));  // with the batch that holds its last draws
  } else {
    // Its draws, if any, are in batches submitted already: after the last of them.
    WhenDone([this, query = std::move(query)] { WriteQuery(query); });
  }
}

void VulkanRenderer::WriteQuery(const Batch::Query& query) {
  uint64_t samples = 0;
  for (uint32_t part : query.parts) {
    uint64_t count = 0;
    if (device_.fn().vkGetQueryPoolResults(device_.device(), query_pool_, part, 1, sizeof(count), &count, sizeof(count),
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      samples += count;
    }
  }
  // Guest samples: the host draws scale x scale pixels a guest pixel, and its MSAA counts each sample.
  samples /= uint64_t(scale_) * scale_ * std::max(1u, msaa_);
  // The counts as D3D lays them out (Total, ZFail, ZPass, StencilFail; A and B each), in host order as the
  // reference writes them; ZFail and the rest zero.
  uint32_t counts[8] = {};
  counts[0] = counts[4] = uint32_t(std::min<uint64_t>(samples, UINT32_MAX));
  if (uint64_t(query.address) + sizeof(counts) <= mirror_bytes_) {
    std::memcpy(physical_ + query.address, counts, sizeof(counts));
    GuestMemoryWritten(query.address, query.address + uint32_t(sizeof(counts)));
  }
}

void VulkanRenderer::GpuMark(Batch& b, VkCommandBuffer cb, GpuKind kind) {
  if (!gpu_marks_) return;
  const uint32_t n = b.marks.fetch_add(1, std::memory_order_relaxed);
  if (n >= kGpuMarks) return;  // the rest of the batch counted with the last kind
  b.mark_kinds[n] = kind;
  device_.fn().vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpu_marks_,
                                    kGpuMarks * uint32_t(&b - batches_.data()) + n);
}

void VulkanRenderer::FinishBatch(Batch& b) {
  for (const Batch::Query& query : b.queries) WriteQuery(query);
  b.queries.clear();
  const uint32_t index = uint32_t(&b - batches_.data());
  stats_.ns_gpu_latency += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - submitted_at_[index]).count());
  if (timestamps_) {
    const double period = device_.properties().limits.timestampPeriod;
    uint64_t stamps[4] = {};
    if (device_.fn().vkGetQueryPoolResults(device_.device(), timestamps_, 4 * index, 2, 16, stamps, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] > stamps[0]) {
      stats_.ns_gpu += uint64_t(double(stamps[1] - stamps[0]) * period);
    }
    // The batch's time by kind: its marks in the order they ran (by time stamp), each interval to the kind that
    // started it, the last until the batch's end.
    const uint32_t marks = std::min(b.marks.load(), kGpuMarks);
    if (gpu_marks_ && marks && stamps[1] > stamps[0]) {
      std::vector<uint64_t> values(marks);
      if (device_.fn().vkGetQueryPoolResults(device_.device(), gpu_marks_, kGpuMarks * index, marks, marks * 8,
                                             values.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        std::vector<std::pair<uint64_t, uint8_t>> order(marks);
        for (uint32_t i = 0; i < marks; ++i) order[i] = {values[i], b.mark_kinds[i]};
        std::sort(order.begin(), order.end());
        for (uint32_t i = 0; i < marks; ++i) {
          const uint64_t end = i + 1 < marks ? order[i + 1].first : stamps[1];
          if (end > order[i].first) stats_.ns_gpu_kind[order[i].second] += uint64_t(double(end - order[i].first) * period);
        }
      }
    }
    if (b.uploaded && device_.fn().vkGetQueryPoolResults(device_.device(), timestamps_, 4 * index + 2, 2, 16, stamps + 2, 8,
                                                         VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[3] > stamps[2]) {
      stats_.ns_gpu_upload += uint64_t(double(stamps[3] - stamps[2]) * period);
    }
  }
  ++stats_.gpu_batches;
  FinishWriteBacks(b);
  if (textures_) {
    textures_->RecycleStaging(b.staging);  // for later uploads
  } else {
    for (Buffer& staging : b.staging) DestroyBuffer(device_, staging);
    b.staging.clear();
  }
  for (Image& image : b.retired) DestroyImage(device_, image);
  b.retired.clear();
  for (VkImageView view : b.retired_views) device_.fn().vkDestroyImageView(device_.device(), view, nullptr);
  b.retired_views.clear();
  std::vector<std::function<void()>>& actions = finished_actions_;  // reused: no allocation per batch
  {
    std::lock_guard lock(done_mutex_);
    actions.swap(b.actions);
  }
  for (auto& action : actions) action();
  actions.clear();
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
// SSSE3's byte shuffle (every x86-64 CPU this runs on has it; the build targets the baseline).
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("ssse3")))
#endif
__m128i ShuffleBytes(__m128i v, __m128i mask) {
  return _mm_shuffle_epi8(v, mask);
}

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
  // `sample_x` / `sample_y`: host pixels a guest pixel; a sample is scale x scale of them (its top-left one taken).
  const uint32_t offset_x = (sample_x > scale_ ? sample >> 1 : 0) * scale_, offset_y = (sample_y > scale_ ? sample & 1 : 0) * scale_;
  const VkDeviceSize depth_bytes = VkDeviceSize(samples_wide) * samples_high * 4;
  Buffer readback;
  if (!CreateBuffer(device_, depth_bytes + VkDeviceSize(samples_wide) * samples_high, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true, readback)) {
    return Fail("resolve readback buffer");
  }
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    ResolveSamples(cb, surface, x0 * int32_t(sample_x), y0 * int32_t(sample_y),
                   (x0 + int32_t(width)) * int32_t(sample_x), (y0 + int32_t(height)) * int32_t(sample_y));
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
                                  uint32_t dest_endian, VkFormat format, int32_t dest_dx, int32_t dest_dy) {
  const Functions& f = device_.fn();
  // The destination's texels: the source's coordinates, moved by dest_dx/dest_dy within a containing resolve.
  // Within the source surface (its pixels: samples x scale a guest pixel).
  const int32_t surface_w = int32_t(surface.image.width / (sample_x * scale_));
  const int32_t surface_h = int32_t(surface.image.height / (sample_y * scale_));
  if (x0 < 0 || y0 < 0 || x0 >= surface_w || y0 >= surface_h) return Fail("resolve outside its surface");
  width = std::min<uint32_t>(width, uint32_t(surface_w - x0));
  height = std::min<uint32_t>(height, uint32_t(surface_h - y0));
  const int32_t dx0 = x0 + dest_dx, dy0 = y0 + dest_dy;
  const uint32_t pitch = (dest_pitch + 31) & ~31u;
  // Within the destination's pitch (the image is that wide).
  if (dx0 < 0 || dy0 < 0 || uint32_t(dx0) >= pitch) return Fail("resolve outside its destination");
  width = std::min<uint32_t>(width, pitch - uint32_t(dx0));
  const uint32_t rows = (uint32_t(dy0) + height + 31) & ~31u;
  Resolved& r = resolved_[dest_base];
  // Scaled as the surface is: textures copied from it keep the resolution.
  const int32_t s = int32_t(scale_);
  if (r.image.image && r.scale != scale_) {
    Record();
    batch().retired.push_back(r.image);  // another scale (a depth resolve there before): nothing of it is kept
    r.image = Image{};
    r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  if (!EnsureResolveImage(r, format, pitch * scale_, rows * scale_,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
    resolved_.erase(dest_base);
    return Fail("resolve image");
  }
  // After the draws in the batch: the surface's samples over the rectangle into the destination's texels, one sample
  // copied as it is, or the samples averaged (a linear blit to half the size averages each 2x2 or 1x2 block).
  EndRendering();
  VkCommandBuffer cb = Record();
  GpuMark(batch(), cb, kGpuResolves);
  ResolveSamples(cb, surface, x0 * int32_t(sample_x) * s, y0 * int32_t(sample_y) * s,
                 (x0 + int32_t(width)) * int32_t(sample_x) * s, (y0 + int32_t(height)) * int32_t(sample_y) * s);
  GpuMark(batch(), cb, kGpuResolves);
  Transition(cb, surface.image, surface.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageBlit region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {x0 * int32_t(sample_x) * s, y0 * int32_t(sample_y) * s, 0};
  region.srcOffsets[1] = {(x0 + int32_t(width)) * int32_t(sample_x) * s, (y0 + int32_t(height)) * int32_t(sample_y) * s, 1};
  region.dstOffsets[0] = {dx0 * s, dy0 * s, 0};
  region.dstOffsets[1] = {(dx0 + int32_t(width)) * s, (dy0 + int32_t(height)) * s, 1};
  f.vkCmdBlitImage(cb, surface.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   sample_x * sample_y > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  // The rectangle to write back: this one, with what earlier ones left unwritten (another player's half).
  uint32_t rx0 = uint32_t(dx0), ry0 = uint32_t(dy0), rx1 = rx0 + width, ry1 = ry0 + height;
  if (r.stale && r.width && r.height) {
    rx0 = std::min(rx0, r.x), ry0 = std::min(ry0, r.y);
    rx1 = std::max(rx1, r.x + r.width), ry1 = std::max(ry1, r.y + r.height);
  }
  r.pitch = pitch, r.endian = dest_endian, r.x = rx0, r.y = ry0, r.width = rx1 - rx0, r.height = ry1 - ry0;
  r.stale = true;
  r.scale = scale_;
  if (textures_) {
    textures_->SetGpuSource({r.image.image, r.image.format, dest_base, pitch, dest_endian, r.image.width, r.image.height,
                             ++resolve_generation_, false, scale_});
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
  // The scissor with the window offset applied to it unless window_offset_disable (as the reference does): a screen
  // resolved in bands (Test Drive Unlimited's second band, drawn with a window offset of -360 rows) otherwise keeps
  // only the rows where the unmoved scissor and the moved rectangle overlap.
  const uint32_t window_tl = regs[kPaScWindowScissorTl], window_br = regs[kPaScWindowScissorBr];
  int32_t wl = int32_t(window_tl & 0x3FFF), wt = int32_t((window_tl >> 16) & 0x3FFF);
  int32_t wr = int32_t(window_br & 0x3FFF), wb = int32_t((window_br >> 16) & 0x3FFF);
  if (!(window_tl >> 31)) {
    const int32_t ox = int32_t(window_offset << 17) >> 17, oy = int32_t(window_offset << 1) >> 17;
    wl += ox, wr += ox, wt += oy, wb += oy;
  }
  const int32_t sl = std::max({wl, int32_t(regs[kPaScScreenScissorTl] & 0x7FFF), 0});
  const int32_t st = std::max({wt, int32_t((regs[kPaScScreenScissorTl] >> 16) & 0x7FFF), 0});
  const int32_t sr = std::min(wr, int32_t(regs[kPaScScreenScissorBr] & 0x7FFF));
  const int32_t sb = std::min(wb, int32_t((regs[kPaScScreenScissorBr] >> 16) & 0x7FFF));
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
  // Host pixels a guest pixel (samples, each scale x scale pixels).
  const uint32_t host_x = sample_x * scale_, host_y = sample_y * scale_;
  if (x0 >= int32_t(surface->image.width / host_x) || y0 >= int32_t(surface->image.height / host_y)) {
    Fail("resolve region outside the source surface");
    return;
  }
  const uint32_t width = uint32_t(std::min<int32_t>(x1, int32_t(surface->image.width / host_x)) - x0);
  const uint32_t height = uint32_t(std::min<int32_t>(y1, int32_t(surface->image.height / host_y)) - y0);
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
  uint32_t dest_base = regs[kRbCopyDestBase] & 0x1FFFFFFF;
  const uint32_t dest_pitch = regs[kRbCopyDestPitch] & 0x3FFF;
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
    } else if (!dest_swap && dest_format == 36 && host_format == VK_FORMAT_R32_SFLOAT) {
      gpu_format = host_format;  // k_32_FLOAT from a k_32_FLOAT target (Test Drive Unlimited)
    } else if (!dest_swap && dest_format == 25 && host_format == VK_FORMAT_R16G16_SNORM) {
      gpu_format = host_format;  // k_16_16 from a k_16_16 target, the same 16-bit words (Test Drive Unlimited)
    } else if (dest_format == 7 &&
               (host_format == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || host_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
      // k_2_10_10_10 (a 10-bit front buffer: The Simpsons Game) from a k_2_10_10_10 target, its red/blue swap kept
      // in the image's component order as the 8-bit path does.
      gpu_format = dest_swap ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
  }
  // A depth resolve: one sample a pixel, the depth value for the depth texture formats, on the GPU too.
  const bool depth_on_gpu = is_depth && (dest_format == 6 || dest_format == 22 || dest_format == 23 || dest_format == 36);
  const bool on_gpu = gpu_format != VK_FORMAT_UNDEFINED || depth_on_gpu;
  if (Diagnosing()) {
    char path[192];
    std::snprintf(path, sizeof(path), "%s %s target format %u (host %d) to format %u%s%s%s, %ux%u",
                  on_gpu ? "GPU" : "CPU", is_depth ? "depth" : "colour", source_key.format, int(host_format), dest_format,
                  dest_bias ? ", exponent bias" : "", dest_swap ? ", red/blue swap" : "",
                  !msaa ? "" : sample_count == sample_x * sample_y ? ", samples averaged" : ", one sample", width, height);
    ++resolve_paths_[path];
  }
  if (depth_on_gpu && !ResolveDepthOnGpu(*surface, x0, y0, width, height, host_x, host_y, first_sample, dest_base,
                                         dest_pitch, dest_endian)) {
    return;
  }
  if (gpu_format != VK_FORMAT_UNDEFINED) {
    // A destination within one an earlier resolve kept on the GPU (same pitch and format), at a whole tile of it:
    // that image at the tile's place. The Simpsons Game's split screen resolves each player's half to the full
    // screen's base, the second half's at the base plus x 640's tiled offset; apart, a texture of the whole screen
    // would read the first half only.
    const uint32_t aligned_pitch = (dest_pitch + 31) & ~31u;
    const uint32_t texel_log2 = gpu_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 3 : 2;
    int32_t dest_dx = 0, dest_dy = 0;
    for (auto it = resolved_.lower_bound(dest_base); it != resolved_.begin();) {
      --it;
      const Resolved& c = it->second;
      if (!c.image.image || c.image.format != gpu_format || c.pitch != aligned_pitch || c.scale != scale_ ||
          c.endian != dest_endian) {
        continue;
      }
      const uint32_t held_rows = c.image.height / c.scale;
      const uint32_t delta = dest_base - it->first;
      if (uint64_t(delta) >= (uint64_t(aligned_pitch) * held_rows << texel_log2)) continue;
      int32_t dx = -1, dy = -1;
      for (uint32_t ty = 0; ty < held_rows && dx < 0; ty += 32) {
        for (uint32_t tx = 0; tx < aligned_pitch; tx += 32) {
          if (uint32_t(TiledOffset2D(int32_t(tx), int32_t(ty), dest_pitch, texel_log2)) == delta) {
            dx = int32_t(tx), dy = int32_t(ty);
            break;
          }
        }
      }
      // Only where its width fits (a match of the tiled offset elsewhere would write past the image's side); it may
      // reach below the image, which grows to hold it (Test Drive Unlimited's second screen band starts at row 352
      // of the first band's 384-row image).
      if (dx < 0 || uint32_t(dx) + width > c.image.width / c.scale) continue;
      const auto own = resolved_.find(dest_base);
      if (own != resolved_.end()) {
        // An image of its own from before: superseded (its texels now live in the containing one).
        if (own->second.stale) WriteBackResolved(own->first, own->second);
        Record();
        batch().retired.push_back(own->second.image);
        if (own->second.small.image) batch().retired.push_back(own->second.small);
        if (own->second.spare.image) batch().retired.push_back(own->second.spare);
        if (own->second.spare_small.image) batch().retired.push_back(own->second.spare_small);
        resolved_.erase(own);
        if (textures_) textures_->DropGpuSources(dest_base, dest_base + 1);
      }
      dest_base = it->first;
      dest_dx = dx, dest_dy = dy;
      break;
    }
    if (!ResolveOnGpu(*surface, x0, y0, width, height, sample_x, sample_y, dest_base, dest_pitch, dest_endian,
                      gpu_format, dest_dx, dest_dy)) {
      return;
    }
  }
  // Read back as well: what the CPU may read (small resolves) and front buffers (presented from guest memory), when
  // the CPU path has the format.
  // A 10-bit resolve kept on the GPU is written back from its image's bytes too (a texture taller than the resolve,
  // its other rows written by the CPU, reads guest memory: The Simpsons Game's 64x8 palette rows in a 64x64 texture).
  const bool cpu_has_format = is_depth || (dest_format == 6 && host_format == VK_FORMAT_R8G8B8A8_UNORM) ||
                              (dest_format == 32 && host_format == VK_FORMAT_R16G16B16A16_SFLOAT) ||
                              (dest_format == 7 && gpu_format != VK_FORMAT_UNDEFINED);
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
    if (!ResolveDepth(*surface, source_key.format == 1, x0, y0, width, height, host_x, host_y, first_sample, dest_base,
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
    const uint32_t samples_wide = width * host_x, samples_high = height * host_y;
    Buffer readback;
    if (!CreateBuffer(device_, VkDeviceSize(samples_wide) * samples_high * source_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      true, readback)) {
      Fail("resolve readback buffer");
      return;
    }
    bool ok = device_.Submit([&](VkCommandBuffer cb) {
      ResolveSamples(cb, *surface, x0 * int32_t(host_x), y0 * int32_t(host_y),
                     x0 * int32_t(host_x) + int32_t(samples_wide), y0 * int32_t(host_y) + int32_t(samples_high));
      Transition(cb, surface->image, surface->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {x0 * int32_t(host_x), y0 * int32_t(host_y), 0};
      region.imageExtent = {samples_wide, samples_high, 1};
      f.vkCmdCopyImageToBuffer(cb, surface->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });
    std::vector<uint8_t> resolved;
    if (ok && host_x * host_y > 1) {
      // One texel a pixel: the selected sample, or the average of the selected samples (8-bit components rounded to
      // nearest, half floats averaged as floats); of a scaled sample, its top-left host pixel.
      const uint8_t* samples = static_cast<const uint8_t*>(readback.mapped);
      resolved.resize(size_t(width) * height * source_bytes);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* taken[4];
          for (uint32_t s = 0; s < sample_count; ++s) {
            const uint32_t sample = first_sample + s;
            taken[s] = samples + (size_t(y * host_y + (sample & 1) * scale_) * samples_wide + x * host_x +
                                  (sample >> 1) * scale_) * source_bytes;
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
        case 2: case 10:  // 2_10_10_10 (and AS_10_10_10_10): three 10-bit components and a 2-bit alpha
          for (uint32_t j = 0; j < 3; ++j) color_value.color.float32[j] = float((clear >> (10 * j)) & 0x3FF) / 1023.0f;
          color_value.color.float32[3] = float((clear >> 30) & 3) / 3.0f;
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
      GpuMark(batch(), cb, kGpuResolves);  // a resolve clearing its targets
      const int32_t area[4] = {x0 * int32_t(host_x), y0 * int32_t(host_y), (x0 + int32_t(width)) * int32_t(host_x),
                               (y0 + int32_t(height)) * int32_t(host_y)};
      // A clear over all a target holds: one drawn back at one sample may have its samples again.
      GiveSamplesBack(color_target, area);
      GiveSamplesBack(depth_target, area);
      VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if (color_target) {
        Transition(cb, color_target->target(), color_target->target_layout(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT);
        color.imageView = color_target->target().view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = color_value;
      }
      if (depth_target) {
        Transition(cb, depth_target->target(), depth_target->target_layout(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        depth.imageView = depth_target->target().view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = depth_value;
      }
      // What a target holds is the clear when it covers all it held.
      for (Surface* t : {color_target, depth_target}) {
        if (!t) continue;
        int32_t* w = t->written;
        if (area[0] <= w[0] && area[1] <= w[1] && area[2] >= w[2] && area[3] >= w[3]) {
          w[0] = w[1] = INT32_MAX, w[2] = w[3] = INT32_MIN;
        }
        GrowDrawn(t, area[0], area[1], area[2], area[3]);
      }
      // A pass for each target: one may be multisampled and the other not.
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea = {{area[0], area[1]}, {width * host_x, height * host_y}};
      rendering.layerCount = 1;
      if (color_target) {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        f.vkCmdBeginRendering(cb, &rendering);
        f.vkCmdEndRendering(cb);
      }
      if (depth_target) {
        rendering.colorAttachmentCount = 0;
        rendering.pColorAttachments = nullptr;
        rendering.pDepthAttachment = rendering.pStencilAttachment = &depth;
        f.vkCmdBeginRendering(cb, &rendering);
        f.vkCmdEndRendering(cb);
      }
    }
  }
  // Textures over the destination are hashed again before their next use (the tiled rows the rectangle reaches).
  if (textures_) {
    const uint32_t element = is_depth ? 4 : (dest_format == 32 ? 8 : 4);
    const uint64_t rows = (uint64_t(y0) + height + 31) & ~uint64_t(31);
    const uint32_t end = uint32_t(std::min<uint64_t>(dest_base + rows * ((dest_pitch + 31) & ~31u) * element, mirror_bytes_));
    textures_->MemoryChanged(dest_base, end);
    GuestMemoryWritten(dest_base, end);  // the pages copied into video memory too (vertices a title resolved)
  }
  last_resolve_ = {surface, dest_base, dest_endian, uint32_t(x0), uint32_t(y0), width, height,
                   !is_depth && !msaa && dest_format == 6 && !dest_bias && !dest_swap};
  ++stats_.resolves;
  resolve_formats_[dest_base] = is_depth ? 0x100 | dest_format : dest_format;
  if (is_depth) ++stats_.resolves_depth;
  if (on_gpu) ++stats_.resolves_on_gpu;
  if (read_back) ++stats_.resolves_read_back;
}

void VulkanRenderer::SetMemoryWatch(std::function<void(uint32_t, uint32_t)> watch) {
  guest_watch_ = std::move(watch);
  const size_t words = (size_t(mirror_bytes_) >> kGuestPageShift) / 64 + 1;
  vram_valid_ = std::make_unique<std::atomic<uint64_t>[]>(words);
  for (size_t i = 0; i < words; ++i) vram_valid_[i] = 0;
  vram_writes_ = std::make_unique<std::atomic<uint8_t>[]>(words * 64);
  for (size_t i = 0; i < words * 64; ++i) vram_writes_[i] = 0;
}

void VulkanRenderer::GuestMemoryWritten(uint32_t begin, uint32_t end) {
  if (!vram_valid_ || begin >= end) return;
  const uint32_t first = begin >> kGuestPageShift, last = (std::min<uint64_t>(end, mirror_bytes_) - 1) >> kGuestPageShift;
  for (uint32_t page = first; page <= last;) {
    // A word's bits at once.
    const uint32_t word = page / 64, bit = page % 64, bits = std::min<uint32_t>(64 - bit, last - page + 1);
    const uint64_t mask = (bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1)) << bit;
    const uint64_t was = vram_valid_[word].fetch_and(~mask, std::memory_order_acq_rel) & mask;
    // A write to a page we copied: counted (the watch is one-shot, so a write between copies counts once).
    for (uint64_t hit = was; hit; hit &= hit - 1) {
      std::atomic<uint8_t>& writes = vram_writes_[size_t(word) * 64 + std::countr_zero(hit)];
      const uint8_t n = writes.load(std::memory_order_relaxed);
      if (n < 255) writes.store(uint8_t(n + 1), std::memory_order_relaxed);
    }
    page += bits;
  }
}

void VulkanRenderer::Invalidate(uint32_t base, uint32_t size) {
  // A coherency request (hundreds a frame): with the CPU's writes watched, the textures already know.
  if (textures_ && !textures_->watching()) textures_->MemoryChanged(base & 0x1FFFFFFF, uint32_t(std::min<uint64_t>(uint64_t(base & 0x1FFFFFFF) + size, mirror_bytes_)));
}

bool VulkanRenderer::EnsureResolveImage(Resolved& r, VkFormat format, uint32_t pitch, uint32_t rows,
                                        VkImageUsageFlags usage) {
  if (r.image.image && r.image.format == format && r.image.width >= pitch && r.image.height >= rows) return true;
  if (r.image.image && r.image.format != format) {
    // Another format: the spare if it is this one and large enough, the current one kept as the spare.
    if (r.spare.image && r.spare.format == format && r.spare_scale == r.scale && r.spare.width >= pitch && r.spare.height >= rows) {
      std::swap(r.image, r.spare);
      std::swap(r.layout, r.spare_layout);
      std::swap(r.small, r.spare_small);
      std::swap(r.small_layout, r.spare_small_layout);
      return true;
    }
    if (r.spare.image) {
      Record();
      batch().retired.push_back(r.spare);  // batches may still read it: freed after this one has run
    }
    if (r.spare_small.image) {
      Record();
      batch().retired.push_back(r.spare_small);
    }
    r.spare = r.image, r.spare_layout = r.layout, r.spare_scale = r.scale;
    r.spare_small = r.small, r.spare_small_layout = r.small_layout;
    r.image = Image{};
    r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    r.small = Image{};
    r.small_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  Image grown;
  const bool keep = r.image.image && r.image.format == format;
  if (!CreateImage2D(device_, std::max(pitch, keep ? r.image.width : 0u), std::max({rows, 32u, keep ? r.image.height : 0u}),
                     format, usage, VK_IMAGE_ASPECT_COLOR_BIT, grown)) {
    DestroyImage(device_, grown);
    return false;
  }
  VkImageLayout grown_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (keep) {
    // What earlier resolves to this base wrote stays (a second rectangle below the first, say).
    EndRendering();
    VkCommandBuffer cb = Record();
    Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    Transition(cb, grown, grown_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {r.image.width, r.image.height, 1};
    device_.fn().vkCmdCopyImage(cb, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, grown.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  if (r.image.image) {
    Record();
    batch().retired.push_back(r.image);  // batches may still read it: freed after this one has run
  }
  r.image = grown;
  r.layout = grown_layout;
  return true;
}

bool VulkanRenderer::WriteBackResolved(uint32_t base, Resolved& r) {
  if (!QueueWriteBack(base, r)) return false;
  const uint32_t index = current_;
  Flush();
  WaitBatch(index);
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
  if (!r.width || !AcquireReadback(VkDeviceSize(r.width) * r.height * w.element, w.buffer)) {
    return false;
  }
  EndRendering();
  VkCommandBuffer cb = Record();
  GpuMark(batch(), cb, kGpuWriteBacks);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Image* source = &r.image;
  if (r.scale > 1) {
    // A scaled resolve: averaged down to one texel a guest texel (a linear blit by an integer factor; depth: one
    // texel taken) into a small image of its own, read back from there.
    const uint32_t small_width = r.image.width / r.scale, small_height = r.image.height / r.scale;
    if (!r.small.image || r.small.format != r.image.format || r.small.width < small_width || r.small.height < small_height) {
      if (r.small.image) {
        batch().retired.push_back(r.small);
        r.small = Image{};
      }
      if (!CreateImage2D(device_, small_width, small_height, r.image.format,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, r.small)) {
        DestroyImage(device_, r.small);
        ReleaseReadback(w.buffer);
        return Fail("resolve write-back image");
      }
      r.small_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    Transition(cb, r.small, r.small_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    const int32_t s = int32_t(r.scale);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {int32_t(r.x) * s, int32_t(r.y) * s, 0};
    blit.srcOffsets[1] = {int32_t(r.x + r.width) * s, int32_t(r.y + r.height) * s, 1};
    blit.dstOffsets[0] = {int32_t(r.x), int32_t(r.y), 0};
    blit.dstOffsets[1] = {int32_t(r.x + r.width), int32_t(r.y + r.height), 1};
    device_.fn().vkCmdBlitImage(cb, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.small.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, w.depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    Transition(cb, r.small, r.small_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    source = &r.small;
  }
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(r.x), int32_t(r.y), 0};
  region.imageExtent = {r.width, r.height, 1};
  device_.fn().vkCmdCopyImageToBuffer(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, w.buffer.buffer, 1, &region);
  r.pending = true;
  batch().write_backs.push_back(w);
  return true;
}

bool VulkanRenderer::AcquireReadback(VkDeviceSize size, Buffer& out) {
  {
    // The smallest free one that fits.
    std::lock_guard lock(readback_mutex_);
    size_t best = free_readbacks_.size();
    for (size_t i = 0; i < free_readbacks_.size(); ++i) {
      if (free_readbacks_[i].size >= size && (best == free_readbacks_.size() || free_readbacks_[i].size < free_readbacks_[best].size)) {
        best = i;
      }
    }
    if (best != free_readbacks_.size()) {
      out = free_readbacks_[best];
      free_readbacks_[best] = free_readbacks_.back();
      free_readbacks_.pop_back();
      free_readback_bytes_ -= out.size;
      return true;
    }
  }
  // None: a new one, rounded up to a power of two (64 KB at least) so it serves other sizes later.
  const VkDeviceSize rounded = VkDeviceSize(1) << (64 - std::countl_zero(uint64_t(size - 1) | 0xFFFF));
  return CreateBuffer(device_, std::max(rounded, size), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, out);
}

void VulkanRenderer::ReleaseReadback(Buffer& buffer) {
  constexpr VkDeviceSize kMaxFreeBytes = 128ull << 20;
  std::lock_guard lock(readback_mutex_);
  if (free_readback_bytes_ + buffer.size > kMaxFreeBytes) {
    DestroyBuffer(device_, buffer);
    return;
  }
  free_readback_bytes_ += buffer.size;
  free_readbacks_.push_back(buffer);
  buffer = Buffer{};
}

void VulkanRenderer::FinishWriteBacks(Batch& b) {
  // Byte orders as 16-byte shuffles: none, 8 in 16, 8 in 32, 16 in 32 (each 32-bit word), 8 in 64 (8-byte elements).
  alignas(16) static constexpr uint8_t kShuffles[5][16] = {
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
      {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12}, {2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13},
      {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8}};
  for (PendingWriteBack& w : b.write_backs) {
    const uint8_t* src = static_cast<const uint8_t*>(w.buffer.mapped);
    const uint32_t element = w.element;
    const bool depth = w.depth;
    const uint32_t bpp_log2 = element == 8 ? 3 : 2;
    // 16 bytes of a tiled row are contiguous from an x aligned to their element count (4 of 4 bytes, 2 of 8): one
    // tiled offset and one shuffle for them.
    const uint32_t group = 16 / element;
    const uint32_t shuffle = depth || w.endian > 4 || (w.endian == 4 && element != 8) ? 0 : w.endian;
    const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(kShuffles[shuffle]));
    auto depth_word = [&](const uint8_t* at) {
      // The guest's depth word (depth << 8, stencil not kept: 0), as the CPU depth resolve writes it.
      float value;
      std::memcpy(&value, at, 4);
      const uint32_t guest =
          w.float24 ? FloatTo20e4(value) : uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * float(0xFFFFFF)));
      return GpuSwap(guest << 8, Endian(w.endian <= 3 ? w.endian : 0));
    };
    for (uint32_t y = 0; y < w.height; ++y) {
      const uint8_t* row = src + size_t(y) * w.width * element;
      for (uint32_t x = 0; x < w.width;) {
        const uint32_t tx = w.x + x;
        const uint32_t address = w.base + uint32_t(TiledOffset2D(int32_t(tx), int32_t(w.y + y), w.pitch, bpp_log2));
        if (tx % group == 0 && x + group <= w.width) {
          alignas(16) uint8_t bytes[16];
          if (depth) {
            for (uint32_t i = 0; i < 4; ++i) {
              const uint32_t word = depth_word(row + size_t(x + i) * 4);
              std::memcpy(bytes + 4 * i, &word, 4);
            }
          } else {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + size_t(x) * element));
            _mm_store_si128(reinterpret_cast<__m128i*>(bytes), ShuffleBytes(v, mask));
          }
          if (address + 16 <= mirror_bytes_) std::memcpy(physical_ + address, bytes, 16);
          x += group;
          continue;
        }
        uint8_t bytes[8];
        std::memcpy(bytes, row + size_t(x) * element, element);
        if (depth) {
          const uint32_t word = depth_word(bytes);
          std::memcpy(bytes, &word, 4);
        } else {
          SwapElement(bytes, element, w.endian);
        }
        if (address + element <= mirror_bytes_) std::memcpy(physical_ + address, bytes, element);
        ++x;
      }
    }
    CountSwapped(g_swapped.write_backs, uint64_t(w.width) * w.height * element);
    if (textures_) textures_->QueueMemoryChanged(w.base, w.base + w.pitch * ((w.y + w.height + 31) & ~31u) * element);
    GuestMemoryWritten(w.base, w.base + w.pitch * ((w.y + w.height + 31) & ~31u) * element);
    w.resolved->pending = false;
    ReleaseReadback(w.buffer);
  }
  b.write_backs.clear();
}

bool VulkanRenderer::ReadImageRGBA8(Image& image, VkImageLayout& layout, uint32_t x, uint32_t y, uint32_t width,
                                    uint32_t height, std::vector<uint32_t>& pixels) {
  Flush();
  WaitIdle();
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
  // Pages written often are forgotten slowly: one the title stops writing is watched again after a while.
  if (vram_writes_ && ++hot_decay_frames_ >= 600) {
    hot_decay_frames_ = 0;
    const size_t pages = ((size_t(mirror_bytes_) >> kGuestPageShift) / 64 + 1) * 64;
    for (size_t i = 0; i < pages; ++i) {
      const uint8_t n = vram_writes_[i].load(std::memory_order_relaxed);
      if (n) vram_writes_[i].store(uint8_t(n / 2), std::memory_order_relaxed);
    }
  }
  pipeline_waits_this_frame_ = 0;
  if (samples_ > VK_SAMPLE_COUNT_1_BIT) {
    for (auto& [key, s] : surfaces_) {
      if (s->unpaired_this_frame && !s->paired && s->ms.image) DropSamples(*s);
      s->unpaired_this_frame = s->paired = false;
    }
  }
  if (textures_) textures_->NewFrame();  // the CPU may have written any texture since
  // Textures not used for a while (the title moved on) are dropped, now and then, once there are many: the cache
  // otherwise grows for as long as the title runs. Freed after the open batch has run (batches before may read them).
  if (textures_ && ++evict_frames_ >= 300) {
    evict_frames_ = 0;
    if (textures_->texture_count() > 1024) {
      Batch& b = batch();
      textures_->Evict(1200, b.retired, b.retired_views);
      for (TextureMemo& memo : texture_memo_) memo.view = VK_NULL_HANDLE;
    }
  }
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
  // The same for a 10-bit front buffer (k_2_10_10_10, or its AS_16_16_16_16 form) resolved into a 10-bit image.
  const bool eight_bit = fetch.format == 6, ten_bit = fetch.format == 7 || fetch.format == 54;
  if (front_resolve != resolved_.end() && fetch.type == 2 && (eight_bit || ten_bit)) {
    Resolved& r = front_resolve->second;
    const uint32_t swizzle = fetch.swizzle & 0x1FF, rgb = 0 | 1 << 3 | 2 << 6, bgr = 2 | 1 << 3 | 0 << 6;
    const VkFormat in_order = eight_bit ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    const VkFormat swapped = eight_bit ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    const bool as_shown = (r.image.format == in_order && swizzle == rgb) || (r.image.format == swapped && swizzle == bgr);
    if (as_shown && fetch.endian == r.endian && r.x == 0 && r.y == 0) {
      next.width = std::min({width ? width : fetch.width, fetch.width, r.width}) * r.scale;
      next.height = std::min({height ? height : fetch.height, fetch.height, r.height}) * r.scale;
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
    next.width = std::min({width ? width : fetch.width, fetch.width, lr.width}) * scale_;
    next.height = std::min({height ? height : fetch.height, fetch.height, lr.height}) * scale_;
    next.on_gpu = true;
    next.number = frame_.number + 1;
    frame_ = std::move(next);
    frame_surface_ = lr.surface;
    frame_x_ = lr.x * scale_, frame_y_ = lr.y * scale_;
    return;
  }
  frame_surface_ = nullptr;
  std::string error;
  std::vector<uint32_t> texels;
  uint32_t tw = 0, th = 0;
  WaitIdle();  // the front buffer read from guest memory: resolves written into it first
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
  for (Batch& b : batches_) {
    if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &b.depth_pool) != VK_SUCCESS) return Fail("depth copy pool");
  }
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
  // Scaled as the surface when each pixel is one sample (a copy of every host pixel); an MSAA surface's sample is
  // taken at the guest resolution (one host pixel of it).
  const uint32_t s = scale_ > 1 && sample_x == scale_ && sample_y == scale_ ? scale_ : 1;
  if (r.image.image && r.scale != s) {
    Record();
    batch().retired.push_back(r.image);  // another scale: nothing of it is kept
    r.image = Image{};
    r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  if (!EnsureResolveImage(r, VK_FORMAT_R32_SFLOAT, pitch * s, rows * s,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT)) {
    resolved_.erase(dest_base);
    return Fail("depth resolve image");
  }
  if (depth_sets_ >= kDepthSets) Flush();  // resets the pool
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = batch().depth_pool;
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
  GpuMark(batch(), cb, kGpuDepthResolves);
  ResolveSamples(cb, surface, x0 * int32_t(sample_x), y0 * int32_t(sample_y),
                 (x0 + int32_t(width)) * int32_t(sample_x), (y0 + int32_t(height)) * int32_t(sample_y));
  GpuMark(batch(), cb, kGpuDepthResolves);
  Transition(cb, surface.image, surface.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_);
  f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_layout_, 0, 1, &set, 0, nullptr);
  // `sample_x` / `sample_y`: host pixels a guest pixel; a sample is scale x scale of them (its top-left one read).
  const int32_t sc = int32_t(scale_), rs = int32_t(s);
  const int32_t region[8] = {x0 * int32_t(sample_x) + (int32_t(sample_x) > sc ? int32_t(sample >> 1) * sc : 0),
                             y0 * int32_t(sample_y) + (int32_t(sample_y) > sc ? int32_t(sample & 1) * sc : 0),
                             x0 * rs, y0 * rs, int32_t(width) * rs, int32_t(height) * rs,
                             rs > 1 ? 1 : int32_t(sample_x), rs > 1 ? 1 : int32_t(sample_y)};
  f.vkCmdPushConstants(cb, depth_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(region), region);
  f.vkCmdDispatch(cb, (width * s + 7) / 8, (height * s + 7) / 8, 1);
  Transition(cb, r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  r.pitch = pitch, r.endian = dest_endian, r.x = uint32_t(x0), r.y = uint32_t(y0), r.width = width, r.height = height;
  r.stale = true;
  r.float24 = surface.key.format == 1;
  r.scale = s;
  if (textures_) {
    textures_->SetGpuSource({r.image.image, VK_FORMAT_R32_SFLOAT, dest_base, pitch, dest_endian, r.image.width,
                             r.image.height, ++resolve_generation_, true, s});
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
  pool_info.maxSets = kGammaFrames;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &gamma_pool_) != VK_SUCCESS) return Fail("gamma pool");
  for (GammaFrame& g : gamma_frames_) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = gamma_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &gamma_layout_;
    if (f.vkAllocateDescriptorSets(d, &alloc, &g.set) != VK_SUCCESS) return Fail("gamma set");
    if (!CreateBuffer(device_, 3 * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, g.lut)) return Fail("gamma LUT");
  }
  return true;
}

void VulkanRenderer::RecordFrameBlit(VkCommandBuffer cb, VkImage target, VkOffset3D dst0, VkOffset3D dst1) {
  if (!frame_.on_gpu || (!frame_surface_ && !frame_resolved_) || !GammaPass()) return;
  Image& frame_image = frame_surface_ ? frame_surface_->image : frame_resolved_->image;
  VkImageLayout& frame_layout = frame_surface_ ? frame_surface_->layout : frame_resolved_->layout;
  // The layout the batches recorded so far leave the frame in: this present's command buffer, submitted apart from
  // them, puts it back, so that a batch recorded before it and submitted after finds what it expects.
  const VkImageLayout batches_layout = frame_layout;
  if (frame_surface_) ResolveSamples(cb, *frame_surface_, int32_t(frame_x_), int32_t(frame_y_),
                                     int32_t(frame_x_ + frame_.width), int32_t(frame_y_ + frame_.height));
  const Functions& f = device_.fn();
  GammaFrame& g = gamma_frames_[gamma_next_];
  gamma_next_ = (gamma_next_ + 1) % kGammaFrames;
  // The output image, as large as the frame.
  if (g.image.width < frame_.width || g.image.height < frame_.height) {
    f.vkDeviceWaitIdle(device_.device());
    DestroyImage(device_, g.image);
    g.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!CreateImage2D(device_, frame_.width, frame_.height, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, g.image)) {
      Fail("gamma image");
      return;
    }
  }
  // The ramp as 8-bit outputs (the shader's LUT), and the set (the frame's surface may change from frame to frame).
  uint32_t* lut = static_cast<uint32_t*>(g.lut.mapped);
  for (uint32_t ch = 0; ch < 3; ++ch) {
    for (uint32_t i = 0; i < 256; ++i) lut[ch * 256 + i] = gamma_lut_[ch][i];
  }
  const VkDescriptorImageInfo source{VK_NULL_HANDLE, frame_image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorImageInfo dest{VK_NULL_HANDLE, g.image.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo lut_info{g.lut.buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i].dstSet = g.set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = types[i];
  }
  writes[0].pImageInfo = &source;
  writes[1].pImageInfo = &dest;
  writes[2].pBufferInfo = &lut_info;
  f.vkUpdateDescriptorSets(device_.device(), 3, writes, 0, nullptr);

  Transition(cb, frame_image, frame_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  Transition(cb, g.image, g.layout, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
  f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline_);
  f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline_layout_, 0, 1, &g.set, 0, nullptr);
  const int32_t region[4] = {int32_t(frame_x_), int32_t(frame_y_), int32_t(frame_.width), int32_t(frame_.height)};
  f.vkCmdPushConstants(cb, gamma_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(region), region);
  f.vkCmdDispatch(cb, (frame_.width + 7) / 8, (frame_.height + 7) / 8, 1);
  if (batches_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
    Transition(cb, frame_image, frame_layout, batches_layout, VK_IMAGE_ASPECT_COLOR_BIT);
  }
  Transition(cb, g.image, g.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(frame_.width), int32_t(frame_.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = dst0;
  blit.dstOffsets[1] = dst1;
  f.vkCmdBlitImage(cb, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
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
