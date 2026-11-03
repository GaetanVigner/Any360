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
// Resolution scale (an integer, both axes): surfaces, colour resolves and the textures copied from them hold scale x
// scale host texels a guest pixel; the translated shaders take the scale into account (pixel positions, texel sizes
// of scaled textures). What reaches guest memory (write-backs, CPU resolves) is one texel a guest pixel; depth
// resolves stay at the guest resolution.
//
// Still narrow: surfaces that alias the same EDRAM in another format do not share contents; colour resolves of a
// few formats only. Each limit is a later step.
#pragma once

#include <array>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <deque>
#include <condition_variable>
#include <atomic>
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
  uint32_t pipeline_waits = 0;  // draws not drawn while their pipeline compiles (SetAsyncPipelines)
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
  // The GPU's time running the batches (time stamps at each batch's start and end), and from each batch's submission
  // to its completion seen on the CPU.
  uint64_t ns_gpu = 0, ns_gpu_latency = 0, gpu_batches = 0;
  uint64_t ns_gpu_upload = 0;  // the GPU's time copying guest memory into video memory before batches
  uint64_t guest_upload_bytes = 0;  // guest memory copied into video memory for the shaders
  // Within draws: texture views (upload, untiling, hashing), shader translation, pipeline lookup and creation.
  uint64_t ns_textures = 0, ns_shaders = 0, ns_pipelines = 0;
  // Draws in four parts: shaders, textures and fixed state; targets, scissor and pipeline; constants and indices;
  // descriptor sets and recording.
  uint64_t ns_draw_setup = 0, ns_draw_targets = 0, ns_draw_constants = 0, ns_draw_record = 0;
  uint64_t ns_draw_indices = 0;  // within constants: the indices copied and swapped
};

// An EDRAM surface: base in tiles, pitch in pixels, a ColorRenderTargetFormat (or DepthRenderTargetFormat with
// `depth`).
struct SurfaceKey {
  uint32_t base = 0, pitch = 0, format = 0, depth = 0;
  bool operator<(const SurfaceKey& o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
  bool operator==(const SurfaceKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
// A hash over a key's bytes (keys without padding, compared with memcmp).
struct BytesHash {
  template <typename T>
  size_t operator()(const T& key) const {
    static_assert(sizeof(T) % 4 == 0);
    uint32_t words[sizeof(T) / 4];
    std::memcpy(words, &key, sizeof(T));
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t w : words) h = (h ^ w) * 0x100000001B3ull;
    return size_t(h ^ (h >> 29));
  }
};

// The host format of a Xenos colour render target format, as the reference maps it (VK_FORMAT_UNDEFINED: unknown).
VkFormat ColorTargetFormat(uint32_t color_format);

class VulkanRenderer : public gfx::Renderer {
 public:
  // `physical`: guest physical memory; the first `mirror_bytes` of it are visible to shaders. `resolution_scale`: host
  // pixels a guest pixel along each axis (1-4).
  VulkanRenderer(Device& device, uint8_t* physical, uint32_t mirror_bytes, uint32_t resolution_scale = 1);
  uint32_t resolution_scale() const { return scale_; }
  // Pipelines kept in the file at `path` across runs (read by Initialize, written as new ones are made and at the
  // end). Before Initialize; empty: none kept.
  void SetPipelineCache(std::string path) { pipeline_cache_path_ = std::move(path); }
  // Pipelines compiled on threads of their own: a draw waits a millisecond at most for its pipeline, then is not
  // drawn until it is ready (a frame or so without it rather than a stall of tens of milliseconds). Before Initialize.
  void SetAsyncPipelines(bool on) { async_pipelines_ = on; }
  ~VulkanRenderer() override;

  bool Initialize();
  // Threads that record draws (into secondary command buffers, a chunk of a render pass's draws each) while this
  // thread prepares the next ones; 0 records them here. Set before the first draw.
  void SetRecordThreads(uint32_t count);
  // A count for this machine: what its cores leave after the title's threads and the command processor's.
  static uint32_t AutoRecordThreads();
  uint32_t record_threads() const { return record_threads_; }
  // With recording threads, a thread of its own submits the batches (it waits for their chunks, records their render
  // passes around them): wait until everything flushed is on the queue, before using the queue otherwise (presenting).
  void WaitSubmitted();

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
  void WhenDone(std::function<void()> action) override;
  // Waits until every batch submitted so far has run and what follows each is done (guest memory up to date).
  void WaitIdle();
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
  // A host that watches guest memory for CPU writes (as for textures): guest memory copied into video memory is then
  // copied again only after a write (GuestMemoryWritten), not at every batch. `watch(begin, end)` arms a one-shot watch.
  void SetMemoryWatch(std::function<void(uint32_t, uint32_t)> watch);
  // [begin, end) of guest memory was written (by the CPU, a resolve's write-back, a replay): its copy in video memory
  // is stale. Any thread.
  void GuestMemoryWritten(uint32_t begin, uint32_t end);
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
    bool operator==(const PipelineKey& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
  };
  // The analyzed shader (no translation yet), and its translation for a modification.
  TranslatedShader* GetShader(const gfx::Shader& shader);
  VkShaderModule GetModule(TranslatedShader& shader, uint64_t modification);
  // The pipeline of a key; asynchronously, VK_NULL_HANDLE with `pending` set while it compiles.
  VkPipeline GetPipeline(const PipelineKey& key, bool& pending);
  VkPipeline CreatePipeline(const PipelineKey& key);  // any thread
  void CompilePipelines();                            // a compiler thread
  void TakeCompiledPipelines();                       // compile_mutex_ held
  void SavePipelineCache();                           // any thread
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
  uint32_t scale_ = 1;  // resolution scale
  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  struct ShaderKeyHash {
    size_t operator()(const std::pair<uint32_t, uint64_t>& k) const { return size_t(k.second ^ (uint64_t(k.first) << 63)); }
  };
  std::unordered_map<std::pair<uint32_t, uint64_t>, std::unique_ptr<TranslatedShader>, ShaderKeyHash> shaders_;  // (type, hash)
  std::unordered_map<PipelineKey, VkPipeline, BytesHash> pipelines_;  // VK_NULL_HANDLE: creation failed
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::string pipeline_cache_path_;
  std::mutex save_mutex_;
  bool async_pipelines_ = false;
  // Asynchronous pipelines: the keys to compile, those queued or compiling, those done (taken by GetPipeline), and
  // the count made and the count in the file when it was last written.
  std::mutex compile_mutex_;
  std::condition_variable compile_cv_, compiled_cv_;
  std::deque<PipelineKey> compile_queue_;
  std::set<PipelineKey> compiling_;
  std::vector<std::pair<PipelineKey, VkPipeline>> compiled_;
  uint32_t compiled_total_ = 0, compiled_saved_ = 0;
  uint32_t pipeline_waits_this_frame_ = 0;  // draws that waited for a pipeline since the last swap
  bool compile_stop_ = false;
  std::vector<std::thread> compilers_;
  std::unordered_map<SurfaceKey, std::unique_ptr<Surface>, BytesHash> surfaces_;
  std::unique_ptr<TextureCache> textures_;
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

  VkDescriptorSetLayout shared_layout_ = VK_NULL_HANDLE, constants_layout_ = VK_NULL_HANDLE, empty_layout_ = VK_NULL_HANDLE;
  std::map<std::array<uint32_t, 3>, VkDescriptorSetLayout> texture_layouts_;
  std::map<std::array<uint32_t, 4>, VkPipelineLayout> pipeline_layouts_;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE, empty_set_ = VK_NULL_HANDLE;
  Buffer shared_memory_;
  bool shared_memory_imported_ = false;  // guest memory read in place (no copy per draw)
  // Guest memory the shaders read (vertices) from video memory: each batch first copies the ranges its draws fetch
  // from the imported guest memory (one transfer over the bus, not a fetch per vertex attribute through it).
  Buffer vram_guest_;
  bool vram_guest_on_ = false;
  // Pages (4 KB) of guest memory whose copy in video memory is current (with a watch set; else none are kept).
  static constexpr uint32_t kGuestPageShift = 12;
  std::unique_ptr<std::atomic<uint64_t>[]> vram_valid_;
  std::function<void(uint32_t, uint32_t)> guest_watch_;
  std::vector<VkBufferCopy> upload_regions_, upload_regions_scratch_;
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
    std::atomic<bool> pending{false};  // its write into guest memory is recorded in a batch, done when it has run
    bool float24 = false;  // a depth resolve from a D24FS8 surface (written back as float24 words)
    uint32_t scale = 1;    // host texels a guest texel along each axis (colour resolves: the resolution scale)
    Image small;           // a scaled resolve brought to one texel a guest texel for its write-back
    VkImageLayout small_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  // Writes a resolve kept on the GPU into guest memory: now (WriteBackResolved: the batch runs), or when the batch
  // has run (QueueWriteBack: its copy recorded in the batch; the CPU reads guest memory only after a fence, which runs
  // the batch first).
  bool WriteBackResolved(uint32_t base, Resolved& r);
  // The resolve image for `base` in `format`, at least `pitch` x `rows`: made, or grown keeping what earlier
  // resolves wrote there (copied in the batch; the old image freed once the batch has run).
  bool EnsureResolveImage(Resolved& r, VkFormat format, uint32_t pitch, uint32_t rows, VkImageUsageFlags usage);
  bool QueueWriteBack(uint32_t base, Resolved& r);
  struct PendingWriteBack {
    Buffer buffer;
    Resolved* resolved;
    uint32_t base, x, y, width, height, pitch, endian, element;
    bool depth, float24;
  };
  // Batches in flight: each submission keeps what its commands use until the GPU has run it. A completion thread
  // waits for them in order, then does what was to follow each (guest memory writes of resolves, the command
  // processor's fences and interrupts) and frees what it used (staging buffers, retired images).
  struct Batch {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBuffer upload_cb = VK_NULL_HANDLE;  // guest memory the batch's shaders read, copied into video memory first
    // With recording threads, the batch is several primary command buffers run in order: pieces this thread records
    // (cb the first), and a render pass's buffer each, recorded at the flush around its chunks.
    std::vector<VkCommandBuffer> pieces;
    uint32_t pieces_used = 0;
    VkFence fence = VK_NULL_HANDLE;
    Buffer constants, indices;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    VkDescriptorPool draw_pool = VK_NULL_HANDLE, depth_pool = VK_NULL_HANDLE;
    std::vector<Buffer> staging;
    std::vector<Image> retired;
    std::vector<PendingWriteBack> write_backs;
    std::vector<std::function<void()>> actions;
    bool done = true;  // run, and what follows it done (guarded by done_mutex_)
    bool uploaded = false;  // its upload command buffer ran too (time stamped)
  };
  static constexpr uint32_t kBatches = 8;
  int32_t last_submitted_ = -1;  // the batch submitted last (its actions take what follows it)
  std::array<Batch, kBatches> batches_;
  uint32_t current_ = 0;  // the batch being recorded (not in flight)
  Batch& batch() { return batches_[current_]; }
  void FinishBatch(Batch& b);  // on the completion thread
  void FinishWriteBacks(Batch& b);
  void WaitBatch(uint32_t index);
  void Complete();  // the completion thread
  std::mutex done_mutex_;
  std::condition_variable done_cv_, work_cv_;
  std::deque<uint32_t> in_flight_;
  bool stopping_ = false;
  std::thread completer_;
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
  // Readback buffers of resolve write-backs (a score a frame): back from the completion thread, kept for the next.
  bool AcquireReadback(VkDeviceSize size, Buffer& out);
  void ReleaseReadback(Buffer& buffer);
  std::mutex readback_mutex_;
  std::vector<Buffer> free_readbacks_;
  VkDeviceSize free_readback_bytes_ = 0;
  static constexpr uint64_t kReadBackMaxBytes = 256 * 1024;
  // `format`: the resolve image's format, the destination's bytes as a texture reads them (B8G8R8A8 for an 8-bit
  // resolve with the red/blue swap: the blit places each component, the texture copies the bytes as they are).
  bool ResolveOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint32_t sample_x,
                    uint32_t sample_y, uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian, VkFormat format);
  std::set<uint32_t> front_buffers_;  // bases swapped as front buffers: resolves there are always read back
  void ReadsGuestMemory(uint32_t address, uint32_t bytes);
  VkQueryPool timestamps_ = VK_NULL_HANDLE;  // four a batch: start, end, its upload's start, end
  std::array<std::chrono::steady_clock::time_point, 8> submitted_at_{};
  uint32_t constant_head_ = 0, index_head_ = 0;  // ring allocation of the batch's constants and indices
  // The last block of each constant buffer written in the open batch: a draw whose block is the same reuses it
  // (most draws change only some constants; the batch's buffer is write-combined, slow to write and to read back).
  struct LastConstants {
    std::vector<uint8_t> bytes;
    uint32_t offset = 0;
    bool valid = false;
  };
  std::array<LastConstants, 8> last_constants_;  // by constant buffer (fewer than 8)
  std::vector<uint8_t> float_scratch_;  // a stage's float constants packed, before they are compared
  static constexpr uint32_t kDrawSets = 8192, kDrawDescriptors = 32768;  // texture sets and descriptors a batch holds
  uint32_t draw_sets_ = 0, draw_images_ = 0, draw_samplers_ = 0;
  // Texture sets of the open batch by their contents (layout, views, samplers): draws with the same textures share
  // one, written once.
  struct SetKeyHash {
    size_t operator()(const std::vector<uint64_t>& key) const {
      uint64_t h = 0xCBF29CE484222325ull;
      for (uint64_t v : key) h = (h ^ v) * 0x100000001B3ull;
      return size_t(h ^ (h >> 29));
    }
  };
  std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, SetKeyHash> batch_sets_;
  // Reused by each draw (thousands a frame): each stage's views and samplers, a texture set's key and writes.
  struct StageTextures {
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
  } stage_textures_[2];
  std::vector<uint64_t> set_key_;
  std::vector<std::function<void()>> finished_actions_;  // the completion thread's, swapped with a batch's
  std::vector<VkDescriptorImageInfo> set_infos_;
  std::vector<VkWriteDescriptorSet> set_writes_;
  // Dynamic state last recorded into the open batch, so a draw records only what changed.
  struct RecordedState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport viewport{};
    VkRect2D scissor{};
    float depth_bias[2] = {};
    float blend[4] = {};
    uint32_t stencil[2] = {~0u, ~0u};
    VkPipelineLayout layout = VK_NULL_HANDLE;  // the descriptor sets bound with it, and the constants' offsets
    VkDescriptorSet sets[4] = {};
    uint32_t offsets[8] = {};
    bool valid = false;
  } recorded_;
  // The last shader and pipeline looked up (consecutive draws mostly use the same: no map search then).
  const void* last_shader_source_[2] = {};
  uint64_t last_shader_hash_[2] = {};
  // Most draws keep the previous draw's shaders, targets and pipeline state (only constants and fetch constants
  // change): what those registers gave last, reused while they hold the same values.
  struct ShaderMemo {
    uint64_t key[4] = {};  // vertex and pixel shader hashes, SQ_PROGRAM_CNTL << 32 | SQ_CONTEXT_MISC, flags
    bool valid = false;
    TranslatedShader *vs = nullptr, *ps = nullptr;
    VkShaderModule vs_module = VK_NULL_HANDLE, ps_module = VK_NULL_HANDLE;
  } shader_memo_;
  struct LayoutMemo {
    uint32_t counts[4] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
  } layout_memo_;
  struct TargetMemo {
    static constexpr uint32_t kWords = 30;
    uint32_t key[kWords] = {};
    bool valid = false;
    void* colors[4] = {};
    void* depth = nullptr;
    uint32_t width = 0, height = 0, depth_control = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
  } target_memo_;
  struct TranslatedShader* last_shader_[2] = {};
  std::vector<uint16_t> index_scratch_;
  // A draw as prepared here, recorded by RecordJob: what it binds, its dynamic state, its indices (converted into the
  // batch's ring at `index_dst`, reserved here).
  struct DrawJob {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[4];
    uint32_t offsets[8];
    VkViewport viewport;
    VkRect2D scissor;
    float depth_bias[2];
    float blend[4];
    uint32_t stencil[2];
    enum class Indices : uint8_t { kNone, kRectangles, kQuads, kDma } indices;
    bool index_32, restart;
    Endian endian;
    DrawSource source;
    VkIndexType index_type;
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    uint8_t* index_dst;
    const uint8_t* index_src;
    uint32_t index_bytes, primitives, reset_index, host_index_count;
    // The texture sets a recording thread makes (sets[2 + stage] then), by layout: images, then samplers.
    static constexpr uint32_t kMaxBindings = 16;
    VkDescriptorSetLayout texture_layouts[2];
    uint8_t view_count[2], sampler_count[2];
    VkImageView views[2][kMaxBindings];
    VkSampler samplers[2][kMaxBindings];
  };
  void RecordJob(VkCommandBuffer cb, const DrawJob& job, RecordedState& rs, std::vector<uint16_t>& scratch) const;
  // Recording threads: chunks of the open render pass's draws, each into a secondary command buffer; the pass's
  // chunks run in order when it ends (EndRendering waits for them).
  struct RecordTask {
    std::vector<DrawJob> jobs;
    uint32_t batch = 0;
    VkFormat formats[4] = {};  // the render pass's attachments
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    std::atomic<bool> done{false};
  };
  struct RecordWorker {
    std::thread thread;
    VkCommandPool pools[8] = {};              // by batch
    std::vector<VkCommandBuffer> buffers[8];  // allocated from each pool
    uint32_t used[8] = {};                    // of them, in use since the pool's reset
    std::vector<uint16_t> scratch;
    // Texture sets: pools by batch (another when one is full), the sets made in the batch by their contents.
    std::vector<VkDescriptorPool> set_pools[8];
    uint32_t set_pool_used[8] = {};
    std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, SetKeyHash> sets[8];
    std::vector<uint64_t> key;
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
  };
  void MakeTextureSets(RecordWorker& w, uint32_t batch, DrawJob& job) const;
  void RecordLoop(uint32_t worker);
  void DispatchChunk();
  uint32_t record_threads_ = 0;
  std::vector<std::unique_ptr<RecordWorker>> workers_;
  std::mutex record_mutex_;
  std::condition_variable record_cv_, record_done_cv_;
  std::deque<RecordTask*> record_queue_;
  bool record_stopping_ = false;
  uint32_t record_idle_ = 0;  // recording threads waiting for a chunk (guarded by record_mutex_)
  std::vector<std::unique_ptr<RecordTask>> segment_tasks_;  // the open render pass's chunks, in order
  std::vector<std::unique_ptr<RecordTask>> spare_tasks_;    // reused
  std::unique_ptr<RecordTask> open_task_;                   // the chunk being filled
  std::vector<VkCommandBuffer> segment_cbs_;
  std::vector<VkCommandBuffer> submit_cbs_;
  // The open batch in order: pieces (cb) and render passes (segment, an index into segments_).
  struct Segment {
    VkRenderingAttachmentInfo color[4];
    VkRenderingAttachmentInfo depth;
    bool has_depth = false;
    VkRect2D area;
    std::vector<std::unique_ptr<RecordTask>> tasks;
  };
  struct Entry {
    VkCommandBuffer cb;
    int32_t segment;  // -1: a piece
  };
  std::vector<Segment> segments_;
  uint32_t segment_count_ = 0;
  std::vector<Entry> order_;
  VkCommandBuffer current_piece_ = VK_NULL_HANDLE;
  bool piece_open_ = false;
  VkCommandBuffer NextPiece(Batch& b);
  // A flushed batch as the submission thread takes it: its order and render passes, the render passes' buffers (from
  // submit_pool_, that thread's), the chunks it has recorded (back to spare_tasks_ when the batch is reused).
  struct Submission {
    std::vector<Entry> order;
    std::vector<Segment> segments;
    uint32_t segment_count = 0;
    bool upload = false;
    std::vector<VkCommandBuffer> pieces;
    uint32_t pieces_used = 0;
    std::vector<VkCommandBuffer> cbs, segment_cbs;
    std::vector<std::unique_ptr<RecordTask>> spent;
  };
  std::array<Submission, 8> submissions_;  // by batch
  void Submit(uint32_t index);             // on the submission thread, or here without one
  void SubmitLoop();
  std::thread submitter_;
  VkCommandPool submit_pool_ = VK_NULL_HANDLE;
  std::mutex submit_mutex_;
  std::condition_variable submit_cv_, submitted_cv_;
  std::deque<uint32_t> to_submit_;
  bool submitting_ = false, submit_stopping_ = false;
  bool segment_secondary_ = false;  // the open pass takes secondary command buffers
  VkFormat segment_formats_[4] = {};
  VkFormat segment_depth_ = VK_FORMAT_UNDEFINED;
  static constexpr uint32_t kChunkDraws = 32;
  // By texture fetch constant (32), view type (3) and signedness: the last view and swizzled signs for its six words,
  // reused while the texture cache says views stay valid (no search by the fetch constant's words then).
  struct TextureMemo {
    uint32_t fetch[6] = {};
    uint64_t version = 0, epoch = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t signs = 0;
    bool scaled = false;  // a resolution-scaled texture (copied from a scaled resolve)
  };
  std::array<TextureMemo, 32 * 3 * 2> texture_memo_;
  // By texture fetch constant: the last sampler for its words and the binding's filters.
  struct SamplerMemo {
    uint32_t key[10] = {};
    VkSampler sampler = VK_NULL_HANDLE;
  };
  std::array<SamplerMemo, 32> sampler_memo_;  // indices converted in ordinary memory, then copied at once
  VkCommandPool batch_pool_ = VK_NULL_HANDLE;
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
