// The Vulkan renderer behind the command processor: each DrawCall becomes a host draw. Shaders are translated from
// Xenos microcode to SPIR-V (the vendored translator) and cached by microcode hash; pipelines are cached by shader
// pair, topology and render state; the five constant buffers the translated shaders read (system constants, vertex
// and pixel float constants packed as each shader uses them, bool/loop constants, fetch constants) are filled from
// the register file; guest physical memory is mirrored into a storage buffer (the translated shaders fetch vertices
// from it); indices are copied with their endian swap.
//
// Render targets are host images, one per EDRAM surface the title binds: RB_SURFACE_INFO (pitch), RB_COLOR{,1,2,3}_INFO
// and RB_DEPTH_INFO (EDRAM base and format) key them, so a title's colour targets 0-3 and its depth/stencil target
// are drawn into directly instead of through an EDRAM emulation. A colour target is bound when its RB_COLOR_MASK
// bits are set and the pixel shader writes it.
//
// Render state comes from the registers as the reference Vulkan backend reads them: depth and stencil
// (RB_DEPTHCONTROL, RB_STENCILREFMASK{,_BF}), blending and write mask per target (RB_BLENDCONTROL0-3, RB_COLOR_MASK,
// RB_BLEND_*), culling, front face, fill mode and polygon offset (PA_SU_SC_MODE_CNTL, PA_SU_POLY_OFFSET_*), the
// scissor (PA_SC_WINDOW_* with the window offset, PA_SC_SCREEN_SCISSOR_*); what Vulkan keeps dynamic (viewport,
// scissor, depth bias, blend constants, stencil masks and references) is set per draw.
//
// Textures come from the texture cache (textures.h), sampled through the descriptor sets the translated shaders
// declare; resolves (Copy) write guest memory; swaps keep the frame on the GPU when it is the last resolve.
// Guest memory is imported in place when the device allows it, and draws run in batches (Flush).
//
// Still narrow: 1x MSAA only; surfaces that alias the same EDRAM in another format do not share contents; colour
// resolves of a few formats only; the viewport without resolution scale. Each limit is a later step.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "device.h"
#include "textures.h"

namespace rex::graphics {
class SpirvShaderTranslator;
class SpirvShader;
}  // namespace rex::graphics

namespace any360::gfx::vk {

struct RendererStats {
  uint32_t draws = 0;
  uint32_t skipped = 0;     // draws with nothing to draw (EDRAM mode ignore, empty scissor, no target)
  uint32_t failed = 0;      // draws not drawn (see last_error)
  uint32_t translations = 0;
  uint32_t pipelines = 0;
  uint32_t render_targets = 0;
  uint32_t texture_uploads = 0;
  uint32_t resolves = 0;
  uint32_t flushes = 0;
  // Time spent (nanoseconds, summed): draws in all, of which copying guest memory and submitting; resolves; swaps.
  uint64_t ns_draws = 0, ns_memory = 0, ns_submit = 0, ns_resolves = 0, ns_swaps = 0;
};

// An EDRAM surface: base in tiles, pitch in pixels, a ColorRenderTargetFormat (or DepthRenderTargetFormat with
// `depth`).
struct SurfaceKey {
  uint32_t base = 0, pitch = 0, format = 0, depth = 0;
  bool operator<(const SurfaceKey& o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
};

// The host format of a Xenos colour render target format, as the reference maps it (VK_FORMAT_UNDEFINED: unknown).
VkFormat ColorTargetFormat(uint32_t color_format);

class VulkanRenderer : public gfx::Renderer {
 public:
  // `physical`: guest physical memory; the first `mirror_bytes` of it are visible to shaders.
  VulkanRenderer(Device& device, uint8_t* physical, uint32_t mirror_bytes);
  ~VulkanRenderer() override;

  bool Initialize();

  // The host image of a surface, created on first use (width = pitch, height = the EDRAM rows the surface spans).
  struct Surface {
    SurfaceKey key;
    Image image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  Surface* GetSurface(const SurfaceKey& key);
  bool ClearColor(const SurfaceKey& key, const float rgba[4]);
  bool ClearDepthStencil(const SurfaceKey& key, float depth, uint32_t stencil);
  // A width x height corner of a colour surface, as the host format's bytes, row after row.
  bool ReadBack(const SurfaceKey& key, uint32_t width, uint32_t height, std::vector<uint8_t>& bytes);

  void Draw(const DrawCall& draw) override;
  // Draws are recorded into one command buffer, the render pass kept open while the targets stay the same; Flush
  // submits them and waits (the command processor calls it before the CPU can observe GPU progress; resolves,
  // swaps, clears, readbacks and texture uploads call it first).
  void Flush() override;
  // A resolve: the EDRAM surface RB_COPY_CONTROL selects, over the rectangle of the copy draw's vertices, converted to
  // RB_COPY_DEST_INFO's format and written tiled to guest memory at RB_COPY_DEST_BASE; then the cleared targets.
  void Copy(const uint32_t* registers) override;
  // A swap: the front buffer (texture fetch constant 0, as VdSwap writes it) read into the presented frame.
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* registers) override;
  void GammaRamp(const uint32_t* table, const uint32_t* pwl) override;

  // The last presented frame: RGBA8 pixels (R in the low byte), row after row; `number` counts swaps.
  // A frame is either still on the GPU (the surface the front buffer was resolved from, when guest memory holds
  // exactly that resolve: presented with RecordFrameBlit, no CPU work) or CPU pixels read from guest memory.
  struct Frame {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> pixels;  // CPU frames only
    uint64_t number = 0;
    bool on_gpu = false;
  };
  const Frame& frame() const { return frame_; }
  // The frame's pixels whichever way it is held (a GPU frame is read back: for tests and screenshots), through the
  // display gamma ramp as the reference presents them (k_8_8_8_8 front buffers: the 256-entry table).
  bool ReadFrame(std::vector<uint32_t>& pixels);
  // The 8-bit output of each 8-bit input through the gamma ramp's table, per channel (red, green, blue).
  const std::array<std::array<uint8_t, 256>, 3>& gamma_lut() const { return gamma_lut_; }
  // Records the GPU frame's blit onto `target` (in TRANSFER_DST layout) over [dst0, dst1).
  void RecordFrameBlit(VkCommandBuffer cb, VkImage target, VkOffset3D dst0, VkOffset3D dst1);

  const RendererStats& stats() const { return stats_; }
  const std::string& last_error() const { return last_error_; }
  // Every failure so far, by reason (last_error's text).
  const std::map<std::string, uint32_t>& failures() const { return failures_; }
  bool guest_memory_in_place() const { return shared_memory_imported_; }
  VkFormat depth_format() const { return depth_format_; }

 private:
  struct TranslatedShader;
  // Everything a pipeline is created from; plain dwords so it compares as bytes.
  struct PipelineKey {
    uint64_t vs_module, ps_module;  // the translations (a shader per modification: interpolators...)
    uint64_t layout;                // the pipeline layout (by texture and sampler counts)
    uint32_t topology, cull_mode, front_face_clockwise, polygon_mode;
    uint32_t depth_control;     // RB_DEPTHCONTROL normalized (0: depth and stencil off)
    uint32_t depth_format;      // host format of the depth/stencil attachment (VK_FORMAT_UNDEFINED: none)
    uint32_t color_formats[4];  // host format of each colour attachment (VK_FORMAT_UNDEFINED: none)
    uint32_t blend_controls[4]; // RB_BLENDCONTROL0-3 (0x00010001: no blending)
    uint32_t color_masks;       // RB_COLOR_MASK of the bound targets
    uint32_t primitive_restart; // strips cut where the index buffer holds the reset index (also keeps the key
                                // free of implicit padding: it is compared with memcmp)
    bool operator<(const PipelineKey& other) const { return std::memcmp(this, &other, sizeof(*this)) < 0; }
  };
  // The analyzed shader (no translation yet), and its translation for a modification.
  TranslatedShader* GetShader(const gfx::Shader& shader);
  VkShaderModule GetModule(TranslatedShader& shader, uint64_t modification);
  VkPipeline GetPipeline(const PipelineKey& key);
  // Texture set layouts by stage and counts (sampled images, then samplers, as the reference lays them out) and
  // pipeline layouts by the four counts.
  VkDescriptorSetLayout TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  VkPipelineLayout PipelineLayout(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures, uint32_t ps_samplers);
  bool Fail(const std::string& why);
  // A depth resolve's copy: the surface's depth and stencil over the rectangle, written as the EDRAM words.
  bool ResolveDepth(Surface& surface, bool float24, int32_t x0, int32_t y0, uint32_t width, uint32_t height,
                    uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian, uint32_t dest_format);
  bool ImportGuestMemory();
  VkCommandBuffer Record();  // the batch's command buffer, begun when needed
  void EndRendering();
  void Transition(VkCommandBuffer cb, Image& image, VkImageLayout& layout, VkImageLayout to, VkImageAspectFlags aspect);

  Device& device_;
  uint8_t* physical_;  // written by resolves
  uint32_t mirror_bytes_;
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<TranslatedShader>> shaders_;  // (type, hash)
  std::map<PipelineKey, VkPipeline> pipelines_;
  std::map<SurfaceKey, std::unique_ptr<Surface>> surfaces_;
  std::unique_ptr<TextureCache> textures_;
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

  VkDescriptorSetLayout shared_layout_ = VK_NULL_HANDLE, constants_layout_ = VK_NULL_HANDLE, empty_layout_ = VK_NULL_HANDLE;
  std::map<std::array<uint32_t, 3>, VkDescriptorSetLayout> texture_layouts_;
  std::map<std::array<uint32_t, 4>, VkPipelineLayout> pipeline_layouts_;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorPool draw_pool_ = VK_NULL_HANDLE;  // texture sets, reset after each draw
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE, constants_set_ = VK_NULL_HANDLE, empty_set_ = VK_NULL_HANDLE;
  Buffer shared_memory_, constants_, indices_;
  bool shared_memory_imported_ = false;  // guest memory read in place (no copy per draw)
  uint32_t constant_head_ = 0, index_head_ = 0;  // ring allocation of the batch's constants and indices
  static constexpr uint32_t kDrawSets = 8192, kDrawDescriptors = 32768;  // texture sets and descriptors a batch holds
  uint32_t draw_sets_ = 0, draw_images_ = 0, draw_samplers_ = 0;
  VkCommandPool batch_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer batch_ = VK_NULL_HANDLE;
  VkFence batch_fence_ = VK_NULL_HANDLE;
  bool batch_open_ = false, rendering_open_ = false;
  std::array<uint64_t, 7> rendering_key_{};  // the open render pass: 4 colour views, depth view, width, height
  uint32_t constant_alignment_ = 256;
  RendererStats stats_;
  Frame frame_;
  // The last resolve, to present a front buffer straight from its source surface.
  struct LastResolve {
    Surface* surface = nullptr;
    uint32_t dest_base = 0, endian = 0, x = 0, y = 0, width = 0, height = 0;
    bool plain = false;  // k_8_8_8_8 copied as it is (no exponent bias, no red/blue swap)
  } last_resolve_;
  Surface* frame_surface_ = nullptr;
  uint32_t frame_x_ = 0, frame_y_ = 0;
  std::string last_error_;
  std::map<std::string, uint32_t> failures_;
  std::array<std::array<uint8_t, 256>, 3> gamma_lut_ = [] {
    std::array<std::array<uint8_t, 256>, 3> lut{};
    for (auto& channel : lut) {
      for (uint32_t i = 0; i < 256; ++i) channel[i] = uint8_t(i);
    }
    return lut;
  }();
};

}  // namespace any360::gfx::vk
