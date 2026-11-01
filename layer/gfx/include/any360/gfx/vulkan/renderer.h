// The Vulkan renderer behind the command processor: each DrawCall becomes a host draw. Shaders are translated from
// Xenos microcode to SPIR-V (the vendored translator) and cached by microcode hash; pipelines are cached by shader
// pair and topology; the five constant buffers the translated shaders read (system constants, vertex and pixel float
// constants packed as each shader uses them, bool/loop constants, fetch constants) are filled from the register file;
// guest physical memory is mirrored into a storage buffer (the translated shaders fetch vertices from it); indices
// are copied with their endian swap. It draws into one host render target of its own.
//
// First milestone, deliberately narrow: one RGBA8 target, no depth, no blending, no textures, no resolves; the
// viewport from PA_CL_VTE_CNTL / PA_CL_VPORT_* without window offsets, scissors or resolution scale; the whole
// mirrored range copied before every draw and every draw submitted on its own. Each limit is a later step.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "device.h"

namespace rex::graphics {
class SpirvShaderTranslator;
class SpirvShader;
}  // namespace rex::graphics

namespace any360::gfx::vk {

struct RendererStats {
  uint32_t draws = 0;
  uint32_t failed = 0;      // draws not drawn (see last_error)
  uint32_t translations = 0;
  uint32_t pipelines = 0;
};

class VulkanRenderer : public gfx::Renderer {
 public:
  // `physical`: guest physical memory; the first `mirror_bytes` of it are visible to shaders.
  VulkanRenderer(Device& device, const uint8_t* physical, uint32_t mirror_bytes);
  ~VulkanRenderer() override;

  // The render target: width x height RGBA8, cleared to `clear` (RGBA, 0-1).
  bool Initialize(uint32_t width, uint32_t height);
  bool Clear(const float clear[4]);
  // The target's pixels, RGBA8, row after row.
  bool ReadBack(std::vector<uint32_t>& pixels);

  void Draw(const DrawCall& draw) override;

  const RendererStats& stats() const { return stats_; }
  const std::string& last_error() const { return last_error_; }

 private:
  struct TranslatedShader;
  TranslatedShader* GetShader(const gfx::Shader& shader);
  VkPipeline GetPipeline(const TranslatedShader& vs, const TranslatedShader* ps, VkPrimitiveTopology topology);
  bool Fail(const std::string& why);

  Device& device_;
  const uint8_t* physical_;
  uint32_t mirror_bytes_;
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<TranslatedShader>> shaders_;  // (type, hash)
  std::map<std::tuple<uint64_t, uint64_t, uint32_t>, VkPipeline> pipelines_;

  VkDescriptorSetLayout shared_layout_ = VK_NULL_HANDLE, constants_layout_ = VK_NULL_HANDLE, empty_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE, constants_set_ = VK_NULL_HANDLE, empty_set_ = VK_NULL_HANDLE;
  Buffer shared_memory_, constants_, indices_;
  Image target_;
  VkImageLayout target_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t constant_alignment_ = 256;
  RendererStats stats_;
  std::string last_error_;
};

}  // namespace any360::gfx::vk
