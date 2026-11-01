// The Vulkan side on a real driver: a headless device, the shaders gfx_shader_tests translated (SPIR-V files next
// to this executable) compiled into graphics pipelines with the descriptor layout the translator expects (as the
// ReXGlue / Xenia Vulkan backend builds it), and an offscreen render pass cleared, drawn into and read back.
// Without a Vulkan 1.3 device the test reports it and passes (CI machines without a GPU).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "any360/gfx/vulkan/device.h"
#include "rex/graphics/pipeline/shader/spirv_translator.h"

namespace {

using namespace any360::gfx;
using rex::graphics::SpirvShaderTranslator;

int g_failures = 0;
#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  std::vector<uint32_t> words(size_t(file.tellg()) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4));
  return words;
}

// The pipeline layout of translated shaders: shared memory (storage buffers), the five constant buffers (dynamic
// uniform buffers), vertex and pixel textures (empty here).
struct GuestLayout {
  VkDescriptorSetLayout shared = VK_NULL_HANDLE, constants = VK_NULL_HANDLE, empty = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;

  bool Create(const vk::Device& d) {
    const vk::Functions& f = d.fn();
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (f.vkCreateDescriptorSetLayout(d.device(), &info, nullptr, &empty) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      constant_bindings[i].binding = i;
      constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      constant_bindings[i].descriptorCount = 1;
      constant_bindings[i].stageFlags = stages;
    }
    constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
    info.pBindings = constant_bindings;
    if (f.vkCreateDescriptorSetLayout(d.device(), &info, nullptr, &constants) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding shared_binding{};
    shared_binding.binding = 0;
    shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_binding.descriptorCount =
        1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(d.properties().limits.maxStorageBufferRange);
    shared_binding.stageFlags = stages;
    info.bindingCount = 1;
    info.pBindings = &shared_binding;
    if (f.vkCreateDescriptorSetLayout(d.device(), &info, nullptr, &shared) != VK_SUCCESS) return false;
    VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
    sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared;
    sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = empty;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = empty;
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
    layout_info.pSetLayouts = sets;
    return f.vkCreatePipelineLayout(d.device(), &layout_info, nullptr, &layout) == VK_SUCCESS;
  }
  void Destroy(const vk::Device& d) {
    const vk::Functions& f = d.fn();
    if (layout) f.vkDestroyPipelineLayout(d.device(), layout, nullptr);
    for (VkDescriptorSetLayout l : {shared, constants, empty}) {
      if (l) f.vkDestroyDescriptorSetLayout(d.device(), l, nullptr);
    }
  }
};

VkShaderModule Module(const vk::Device& d, const std::vector<uint32_t>& words) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = words.size() * 4;
  info.pCode = words.data();
  VkShaderModule module = VK_NULL_HANDLE;
  return d.fn().vkCreateShaderModule(d.device(), &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

// A graphics pipeline for dynamic rendering into one RGBA8 target: what the driver compiles the SPIR-V in.
VkPipeline Pipeline(const vk::Device& d, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule ps, VkFormat format) {
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  return d.fn().vkCreateGraphicsPipelines(d.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS
             ? pipeline
             : VK_NULL_HANDLE;
}

void Barrier(const vk::Device& d, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
             VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  d.fn().vkCmdPipelineBarrier2(cb, &dependency);
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path dir = std::filesystem::path(argv[0]).parent_path();
  vk::Device device;
  vk::DeviceOptions options;
  if (argc > 1) options.prefer = argv[1];
  if (!device.Create(options)) {
    std::printf("gfx_vulkan_tests: skipped (%s)\n", device.error().c_str());
    return 0;
  }
  const vk::Functions& f = device.fn();
  std::printf("gfx_vulkan_tests: %s, Vulkan %u.%u.%u, max storage buffer range %u MB\n", device.properties().deviceName,
              VK_API_VERSION_MAJOR(device.properties().apiVersion), VK_API_VERSION_MINOR(device.properties().apiVersion),
              VK_API_VERSION_PATCH(device.properties().apiVersion), device.properties().limits.maxStorageBufferRange >> 20);

  // The translated shaders: each compiles into a module and, paired, into a pipeline.
  const std::vector<uint32_t> vs = ReadSpirv(dir / "smoke_vs.spv"), vs_reg = ReadSpirv(dir / "smoke_vs_reg.spv"),
                              ps = ReadSpirv(dir / "smoke_ps.spv");
  CHECK(!vs.empty() && !vs_reg.empty() && !ps.empty());
  GuestLayout layout;
  CHECK(layout.Create(device));
  const VkShaderModule vs_module = Module(device, vs), vs_reg_module = Module(device, vs_reg), ps_module = Module(device, ps);
  CHECK(vs_module && vs_reg_module && ps_module);
  const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  const VkPipeline pipeline = Pipeline(device, layout.layout, vs_module, ps_module, format);
  const VkPipeline pipeline_reg = Pipeline(device, layout.layout, vs_reg_module, ps_module, format);
  CHECK(pipeline != VK_NULL_HANDLE && pipeline_reg != VK_NULL_HANDLE);

  // Offscreen: a 64x64 target cleared through dynamic rendering, then copied back.
  vk::Image target;
  vk::Buffer readback;
  CHECK(vk::CreateImage2D(device, 64, 64, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, target));
  CHECK(vk::CreateBuffer(device, 64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback));
  const bool submitted = device.Submit([&](VkCommandBuffer cb) {
    Barrier(device, cb, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 1.0f}};  // exact in 8 bits
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {64, 64}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    f.vkCmdBeginRendering(cb, &rendering);
    f.vkCmdEndRendering(cb);
    Barrier(device, cb, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {64, 64, 1};
    f.vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
  });
  CHECK(submitted);
  if (submitted && readback.mapped) {
    const uint8_t* p = static_cast<const uint8_t*>(readback.mapped);
    std::printf("gfx_vulkan_tests: clear read back %u %u %u %u\n", p[0], p[1], p[2], p[3]);
    CHECK(p[0] == 51 && p[1] == 102 && p[2] == 153 && p[3] == 255);
    CHECK(std::memcmp(p, p + (64 * 64 - 1) * 4, 4) == 0);
  }

  vk::DestroyBuffer(device, readback);
  vk::DestroyImage(device, target);
  for (VkPipeline p : {pipeline, pipeline_reg}) {
    if (p) f.vkDestroyPipeline(device.device(), p, nullptr);
  }
  for (VkShaderModule m : {vs_module, vs_reg_module, ps_module}) {
    if (m) f.vkDestroyShaderModule(device.device(), m, nullptr);
  }
  layout.Destroy(device);
  std::printf("gfx_vulkan_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
