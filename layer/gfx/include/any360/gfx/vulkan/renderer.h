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
// MSAA surfaces are drawn as their samples (2x: twice the rows; 4x: twice the columns too), and resolves pick or
// average a pixel's samples as RB_COPY_CONTROL asks.
//
// Still narrow: surfaces that alias the same EDRAM in another format do not share contents; colour resolves of a
// few formats only; the viewport without resolution scale. Each limit is a later step.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
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
  uint32_t texture_gpu_copies = 0;  // textures copied from a resolve kept on the GPU
  uint32_t resolves = 0;
  uint32_t resolves_on_gpu = 0, resolves_read_back = 0, resolves_depth = 0;  // kinds of resolves among them
  uint32_t resolve_write_backs = 0;  // GPU resolves written into guest memory later (read by a texture otherwise)
  uint64_t ns_texture_hash = 0, ns_texture_load = 0;  // within textures: hashing, loading levels for uploads
  uint32_t flushes = 0;
  // Time spent (nanoseconds, summed): draws in all, of which copying guest memory and submitting; resolves; swaps.
  uint64_t ns_draws = 0, ns_memory = 0, ns_submit = 0, ns_resolves = 0, ns_swaps = 0;
  // Within draws: texture views (upload, untiling, hashing), shader translation, pipeline lookup and creation.
  uint64_t ns_textures = 0, ns_shaders = 0, ns_pipelines = 0;
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
    VkImageView depth_view = VK_NULL_HANDLE;  // a depth surface's depth aspect, sampled by depth resolves
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
  void Invalidate(uint32_t base, uint32_t size) override;
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
  // display gamma ramp as the reference presents them (k_8_8_8_8 front buffers: the 256-entry table). CPU frames'
  // pixels are already through it.
  bool ReadFrame(std::vector<uint32_t>& pixels);
  // The 8-bit output of each 8-bit input through the gamma ramp's table, per channel (red, green, blue).
  const std::array<std::array<uint8_t, 256>, 3>& gamma_lut() const { return gamma_lut_; }
  // Records the GPU frame's blit onto `target` (in TRANSFER_DST layout) over [dst0, dst1), through the gamma ramp (a
  // compute pass into an intermediate image, then the blit).
  void RecordFrameBlit(VkCommandBuffer cb, VkImage target, VkOffset3D dst0, VkOffset3D dst1);

  const RendererStats& stats() const { return stats_; }
  const std::string& last_error() const { return last_error_; }
  // Every failure so far, by reason (last_error's text).
  const std::map<std::string, uint32_t>& failures() const { return failures_; }
  bool guest_memory_in_place() const { return shared_memory_imported_; }
  // For diagnostics: the textures uploaded so far by (base, format, width, height), and whether a base is the
  // destination of a resolve (with its destination format, or UINT32_MAX).
  TextureCache* texture_cache() { return textures_.get(); }
  uint32_t ResolveFormatAt(uint32_t base) const {
    const auto it = resolve_formats_.find(base);
    return it == resolve_formats_.end() ? UINT32_MAX : it->second;
  }
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
  bool GammaPass();  // the gamma pass's pipeline and resources, made on first use
  // A depth resolve's copy: the surface's depth and stencil over the rectangle (in pixels; one sample of each pixel
  // of an MSAA surface, `sample_x` x `sample_y` samples a pixel), written as the EDRAM words.
  bool ResolveDepth(Surface& surface, bool float24, int32_t x0, int32_t y0, uint32_t width, uint32_t height,
                    uint32_t sample_x, uint32_t sample_y, uint32_t sample, uint32_t dest_base, uint32_t dest_pitch,
                    uint32_t dest_endian, uint32_t dest_format);
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
  // Without the import: the guest memory ranges the batch's shaders read (vertex buffers, indices a rectangle list's
  // vertex shader loads), copied into the mirror when the batch is submitted.
  std::vector<std::pair<uint32_t, uint32_t>> batch_ranges_;  // begin, end
  // Resolves kept on the GPU, by destination base: the destination's texels at their coordinates (an image as wide
  // as the destination pitch), which textures reading that destination copy from. A resolve of more than
  // kReadBackMaxBytes is not read back into guest memory (the CPU reads small ones: exposure, picking).
  struct Resolved {
    Image image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t pitch = 0, endian = 0, x = 0, y = 0, width = 0, height = 0;  // the last resolve's rectangle
    bool stale = false;    // guest memory lacks it (a resolve kept on the GPU only)
    bool pending = false;  // its write into guest memory is recorded in the batch, done when the batch has run
    bool float24 = false;  // a depth resolve from a D24FS8 surface (written back as float24 words)
  };
  // Writes a resolve kept on the GPU into guest memory: now (WriteBackResolved: the batch runs), or when the batch
  // has run (QueueWriteBack: its copy recorded in the batch; the CPU reads guest memory only after a fence, which runs
  // the batch first).
  bool WriteBackResolved(uint32_t base, Resolved& r);
  // The resolve image for `base` in `format`, at least `pitch` x `rows`: made, or grown keeping what earlier
  // resolves wrote there (copied in the batch; the old image freed once the batch has run).
  bool EnsureResolveImage(Resolved& r, VkFormat format, uint32_t pitch, uint32_t rows, VkImageUsageFlags usage);
  std::vector<Image> retired_images_;  // freed when the batch has run
  bool QueueWriteBack(uint32_t base, Resolved& r);
  void FinishWriteBacks();
  struct PendingWriteBack {
    Buffer buffer;
    Resolved* resolved;
    uint32_t base, x, y, width, height, pitch, endian, element;
    bool depth, float24;
  };
  std::vector<PendingWriteBack> pending_write_backs_;
  std::set<uint32_t> front_on_gpu_;  // front buffers presented from their resolve image (not read back)
  // Reads `image` (RGBA8 or BGRA8) over a rectangle as RGBA8 pixels.
  bool ReadImageRGBA8(Image& image, VkImageLayout& layout, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      std::vector<uint32_t>& pixels);
  // A depth resolve kept on the GPU: the depth of one sample a pixel into an R32F image (DepthPass, depth_shader.cpp).
  bool ResolveDepthOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint32_t sample_x,
                         uint32_t sample_y, uint32_t sample, uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian);
  bool DepthPass();
  VkDescriptorSetLayout depth_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_pool_ = VK_NULL_HANDLE;  // reset with each batch
  static constexpr uint32_t kDepthSets = 64;
  uint32_t depth_sets_ = 0;
  std::map<uint32_t, Resolved> resolved_;
  std::map<uint32_t, uint32_t> resolve_formats_;  // destination base: format of the last resolve there
 public:
  // For diagnostics: resolves by path taken (GPU or CPU, source and destination formats, what kept it off the GPU).
  std::map<std::string, uint32_t>& resolve_paths() { return resolve_paths_; }

 private:
  std::map<std::string, uint32_t> resolve_paths_;
  uint64_t resolve_generation_ = 0;
  static constexpr uint64_t kReadBackMaxBytes = 256 * 1024;
  // `format`: the resolve image's format, the destination's bytes as a texture reads them (B8G8R8A8 for an 8-bit
  // resolve with the red/blue swap: the blit places each component, the texture copies the bytes as they are).
  bool ResolveOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint32_t sample_x,
                    uint32_t sample_y, uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian, VkFormat format);
  std::set<uint32_t> front_buffers_;  // bases swapped as front buffers: resolves there are always read back
  void ReadsGuestMemory(uint32_t address, uint32_t bytes);
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
  Resolved* frame_resolved_ = nullptr;  // the frame's image when it is a resolve kept on the GPU
  uint32_t frame_x_ = 0, frame_y_ = 0;
  std::string last_error_;
  std::map<std::string, uint32_t> failures_;
  // The gamma pass of GPU frames: the shader's set (source surface, output image, LUT), the output image.
  VkDescriptorSetLayout gamma_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline gamma_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool gamma_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet gamma_set_ = VK_NULL_HANDLE;
  Buffer gamma_lut_buffer_;
  Image gamma_image_;
  VkImageLayout gamma_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  std::array<std::array<uint8_t, 256>, 3> gamma_lut_ = [] {
    std::array<std::array<uint8_t, 256>, 3> lut{};
    for (auto& channel : lut) {
      for (uint32_t i = 0; i < 256; ++i) channel[i] = uint8_t(i);
    }
    return lut;
  }();
};

}  // namespace any360::gfx::vk
