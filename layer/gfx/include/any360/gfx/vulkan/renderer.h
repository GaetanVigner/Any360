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

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

// Kinds of GPU work, timed apart (RendererStats::ns_gpu_kind).
enum GpuKind : uint8_t {
  kGpuOther, kGpuDraws, kGpuResolves, kGpuSampleResolves, kGpuDepthResolves, kGpuTextureCopies, kGpuWriteBacks,
  kGpuKinds
};

struct RendererStats {
  uint32_t draws = 0;
  uint32_t skipped = 0;     // draws with nothing to draw (EDRAM mode ignore, empty scissor, no target)
  uint32_t failed = 0;      // draws not drawn (see last_error)
  uint32_t memexports = 0;  // draws whose shaders export to memory
  uint32_t translations = 0;
  uint32_t pipelines = 0;
  uint32_t sample_resolves = 0;        // host MSAA: surfaces' samples resolved for a read
  uint64_t sample_resolve_pixels = 0;  // and the host pixels they covered
  uint32_t samples_dropped = 0;        // surfaces back to one sample (drawn without colour and depth together)
  uint32_t samples_given_back = 0;     // and given their samples again (drawn with them together, then cleared)
  uint32_t samples_reused = 0;         // of those, samples taken from the pool of dropped ones (no image created)
  uint32_t view_loads = 0;             // 2x MSAA as views: surfaces' rows copied into their layers before a draw
  uint32_t view_resolves = 0;          // and their layers copied into their rows for a read
  uint64_t view_resolve_pixels = 0;    // the host pixels of those copies (both ways)
  uint32_t view_copies_by_rows = 0;    // of those, copies outside a batch (a region a pixel row's sample: slow)
  uint32_t view_direct_resolves = 0;   // colour resolves averaged straight from the layers (no copy into the rows)
  uint32_t edram_alias_copies = 0;     // copies between surfaces over the same EDRAM (SyncAliases)
  uint32_t edram_depth_to_colour = 0;
  uint32_t extents_estimated = 0;  // draws without clipping whose rows came from their vertices (EstimateVertexMaxY)
  uint64_t ns_extents = 0;         // time running their vertex shaders on the CPU for it  // of them, depth words into a colour surface (SyncDepthIntoColour)
  uint32_t pipeline_waits = 0;  // draws not drawn while their pipeline compiles (SetAsyncPipelines)
  uint32_t pipelines_warmed = 0;  // pipelines queued from the recipes of earlier runs (WarmUpPipelines)
  uint32_t render_targets = 0;
  uint32_t texture_uploads = 0;
  uint32_t texture_gpu_copies = 0;  // textures copied from a resolve kept on the GPU
  uint32_t texture_direct_views = 0;  // textures that sampled a resolve's image in place (no copy)
  uint32_t resolves = 0;
  uint32_t resolves_on_gpu = 0, resolves_read_back = 0, resolves_depth = 0;  // kinds of resolves among them
  uint32_t resolves_biased_on_gpu = 0;  // GPU resolves with an exponent bias (ScaleOnGpu)
  uint32_t resolves_fixed16_on_gpu = 0;  // k_16_16_16_16 resolves converted on the GPU (Fixed16OnGpu)
  uint32_t resolves_superseded = 0;  // resolves kept on the GPU the CPU wrote over before they were written back
  uint32_t resolve_write_backs = 0;  // GPU resolves written into guest memory later (read by a texture otherwise)
  // Their pixels written into guest memory (WriteBackTiled), and with ANY360_WRITE_BACK_BENCH the first version on
  // the same pixels too (each write-back done both ways, in alternating order: the same bytes twice).
  uint64_t ns_write_backs = 0, ns_write_backs_reference = 0;
  uint64_t ns_texture_hash = 0, ns_texture_load = 0;  // within textures: hashing, loading levels for uploads
  uint32_t flushes = 0;
  uint32_t ring_growths = 0;  // a ring of constants or indices doubled for the next batches (GrowRing)
  // Time spent (nanoseconds, summed): draws in all, of which copying guest memory and submitting; resolves; swaps.
  uint64_t ns_draws = 0, ns_memory = 0, ns_submit = 0, ns_resolves = 0, ns_swaps = 0;
  // The GPU's time running the batches (time stamps at each batch's start and end), and from each batch's submission
  // to its completion seen on the CPU.
  uint64_t ns_gpu = 0, ns_gpu_latency = 0, gpu_batches = 0;
  uint64_t ns_gpu_upload = 0;  // the GPU's time copying guest memory into video memory before batches
  // The GPU's time by kind of work (GpuKind), from time stamps at each piece of work's start.
  uint64_t ns_gpu_kind[8] = {};
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
// The interpolators (of `interpolators`) a pixel shader samples at their centroid: RB_SURFACE_INFO's MSAA mode,
// SQ_CONTEXT_MISC sc_sample_cntl and SQ_INTERPOLATOR_CNTL's sampling pattern, as the reference picks them.
uint32_t CentroidInterpolators(uint32_t interpolators, uint32_t surface_info, uint32_t context_misc,
                               uint32_t interpolator_cntl);

class VulkanRenderer : public gfx::Renderer {
 public:
  // `physical`: guest physical memory; the first `mirror_bytes` of it are visible to shaders. `resolution_scale`: host
  // pixels a guest pixel along each axis (1-4).
  VulkanRenderer(Device& device, uint8_t* physical, uint32_t mirror_bytes, uint32_t resolution_scale = 1);
  uint32_t resolution_scale() const { return scale_; }
  // Pipelines kept in the file at `path` across runs (read by Initialize, written as new ones are made and at the
  // end). Before Initialize; empty: none kept.
  void SetPipelineCache(std::string path) { pipeline_cache_path_ = std::move(path); }
  // Host MSAA: render targets drawn with this many samples a host pixel (1, 2, 4, 8; the most the device has up to
  // it), averaged when anything reads them (depth: sample 0). Apart from the resolution scale (supersampling) and
  // from the title's own MSAA (drawn as pixels). Before Initialize.
  void SetMsaa(uint32_t samples) { msaa_ = samples; }
  uint32_t msaa() const { return uint32_t(samples_); }
  // Pipelines compiled on threads of their own: a draw waits a millisecond at most for its pipeline, then is not
  // drawn until it is ready (a frame or so without it rather than a stall of tens of milliseconds). Before Initialize.
  void SetAsyncPipelines(bool on) { async_pipelines_ = on; }
  // Pipeline warm-up. Every pipeline a title draws with is kept as a recipe beside the pipeline cache
  // (<cache>.recipes: the shaders' microcode, their modifications and the render state, and whether the pass was
  // multisampled, but not the host MSAA or resolution of that run). The driver's cache only holds pipelines for the
  // settings they were made with; the recipes are compiled again for this run's settings (another MSAA, a new driver)
  // before the title draws with them, so its objects are not left out while their pipelines compile. With
  // asynchronous pipelines only: each swap queues what `budget` allows (translating the shaders is the cost on this
  // thread; the compiler threads make the pipelines); a title may call it with more in its loading screens.
  // Returns the recipes left.
  size_t WarmUpPipelines(std::chrono::microseconds budget);
  size_t recipes_loaded() const { return recipes_loaded_; }
  // Waits until every pipeline queued (by draws or the warm-up) is compiled and ready for the draws.
  void FinishPipelines();
  ~VulkanRenderer() override;

  bool Initialize();
  // Threads that record draws (into secondary command buffers, a chunk of a render pass's draws each) while this
  // thread prepares the next ones; 0 records them here. Set before the first draw.
  void SetRecordThreads(uint32_t count);
  // What follows a batch (the command processor's fences) once the batch no longer reads guest memory, instead of
  // once the GPU has run it (as the console's GPU passes a fence once it has fetched what precedes it); resolves read
  // back into guest memory land when it has run. Off: once it has run.
  void SetFencesWhenCaptured(bool on) { fences_when_captured_ = on; }
  // A texture read from guest memory over a resolve kept on the GPU (one it cannot take from there: a mip chain
  // built from resolves of each level) reads what the resolve wrote a frame or so before, its write-back queued in
  // the batch, instead of the batch flushed and waited for there. Off by default: a title mixing CPU-written and
  // resolved texels in one texture would see them a frame apart.
  void SetLateResolveReads(bool on) { late_resolve_reads_ = on; }
  // A count for this machine: what its cores leave after the title's threads and the command processor's.
  static uint32_t AutoRecordThreads();
  uint32_t record_threads() const { return record_threads_; }
  // With recording threads, a thread of its own submits the batches (it waits for their chunks, records their render
  // passes around them): wait until everything flushed is on the queue, before using the queue otherwise (presenting).
  void WaitSubmitted();

  // The host image of a surface, created on first use (width = pitch, height = the EDRAM rows the surface spans).
  // Surfaces over the same EDRAM (SyncAliases): by tile row, the version written last and the surface that wrote it.
  struct Surface;
  struct EdramGroup {
    std::vector<uint64_t> version;  // by tile row
    std::vector<Surface*> writer;   // by tile row (null: none yet)
    uint32_t members = 0;
  };
  struct Surface {
    SurfaceKey key;
    Image image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView depth_view = VK_NULL_HANDLE;  // a depth surface's depth aspect, sampled by depth resolves
    // With host MSAA (SetMsaa): the multisampled image draws render into, resolved into `image` before anything
    // reads it; `drawn`: the host pixels drawn since (left, top, right, bottom; empty: nothing to resolve).
    Image ms;
    VkImageLayout ms_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Views of `ms` and `image` as unsigned integers of the same size, for resolving one sample instead of the
    // average (only integer formats may); null when the images cannot take them.
    VkImageView ms_bits = VK_NULL_HANDLE, image_bits = VK_NULL_HANDLE;
    bool bits_alias = false;  // `image` made to take its integer view
    int32_t drawn[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    bool paired = false;               // drawn with colour and depth targets together this frame
    bool unpaired_this_frame = false;  // a depth target drawn without colour targets this frame
    int32_t written[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};  // what it holds: drawn or cleared since
    // EDRAM is one memory: surfaces of the same pitch, format and kind whose bases are a whole number of tile rows
    // apart hold the same samples (Call of Duty: Black Ops draws its shadow atlas at two bases 1024 rows apart and
    // resolves it whole from the first). Such surfaces share an EdramGroup that knows, for each tile row of EDRAM (16
    // sample rows), which surface wrote it last and the version it wrote; `row0` is this surface's first tile row
    // there, `have` the version of each of its tile rows it holds (SyncAliases).
    EdramGroup* group = nullptr;
    uint32_t row0 = 0;
    std::vector<uint64_t> have;
    // A colour surface: a depth surface wrote over its EDRAM since it last looked (SyncDepthIntoColour).
    bool depth_over = false;
    // Guest 2x MSAA drawn at the console's sample positions (UseViews): two layers of a pixel each (sample 0, then 1,
    // the pass's views), each drawn with its sample's offset. The image keeps its rows (a pixel's samples one under the
    // other, as EDRAM holds them) for everything else: the layers are copied into them before they are read
    // (ResolveSamples: drawn is what the layers hold newer), and loaded from them when they hold newer content
    // (views_stale: written while the surface was not drawn as views).
    Image views;
    VkImageLayout views_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView views_depth[2] = {};  // a depth surface's layers' depth aspect, one each (depth resolves read them)
    bool use_views = false;    // draws render into views
    // Host rows of the image holding what the layers do not (written outside the layers since they last matched; [0] >= [1]:
    // none): loaded into the layers before the next 2x pass.
    int32_t views_stale[2] = {0, INT32_MAX};
    bool StaleViews() const { return views_stale[0] < views_stale[1]; }
    void StaleRows(int32_t top, int32_t bottom) {
      views_stale[0] = std::min(views_stale[0], top), views_stale[1] = std::max(views_stale[1], bottom);
    }
    void FreshViews() { views_stale[0] = INT32_MAX, views_stale[1] = INT32_MIN; }
    Image& target() { return use_views ? views : ms.image ? ms : image; }  // what draws render into
    VkImageLayout& target_layout() { return use_views ? views_layout : ms.image ? ms_layout : layout; }
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
  uint64_t CpuWriteMark() const override { return textures_ ? textures_->CpuWriteMark() : 0; }
  void CpuWritesVisible(uint64_t mark) override {
    if (textures_) textures_->CpuWritesVisible(mark);
  }
  void GammaRamp(const uint32_t* table, const uint32_t* pwl) override;
  bool CountsSamples() const override { return query_pool_ != VK_NULL_HANDLE && occlusion_counts_; }
  // Occlusion queries counted on the GPU (the default), or answered at once with a fixed count by the command
  // processor, as the runtime's own GPU does (every object visible). Before the first query.
  void SetOcclusionCounts(bool counted) { occlusion_counts_ = counted; }
  void OcclusionQuery(bool end, uint32_t address) override;

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
  // Texels read back from an image of `format` made RGBA8 (red in the low byte) in place: BGRA8 swapped, the 10-bit
  // A2B10G10R10 / A2R10G10B10 ones rounded to 8 bits a channel (alpha 0-3 to 0-255). RGBA8 and other formats as they
  // are.
  static void TexelsToRGBA8(VkFormat format, std::vector<uint32_t>& pixels);
  // The 8-bit output of each 8-bit input through the gamma ramp's table, per channel (red, green, blue).
  const std::array<std::array<uint8_t, 256>, 3>& gamma_lut() const { return gamma_lut_; }
  // Records the GPU frame's blit onto `target` (in TRANSFER_DST layout) over [dst0, dst1), through the gamma ramp (a
  // compute pass into an intermediate image, then the blit).
  void RecordFrameBlit(VkCommandBuffer cb, VkImage target, VkOffset3D dst0, VkOffset3D dst1);

  const RendererStats& stats() const { return stats_; }
  // With ANY360_DRAW_TIMING set: the GPU's time by pixel shader (its hash; 0: draws without one), from a time stamp
  // before each draw, nanoseconds and draws. A diagnostic: the time stamps cost a little and serialize nothing, so a
  // draw's interval may hold some of its neighbours' work.
  struct ShaderTime {
    uint64_t ns = 0, draws = 0, vertex_hash = 0;
    uint64_t indices = 0;  // the draws' vertices (indices), to tell vertex-bound draws from small ones
  };
  std::map<uint64_t, ShaderTime> shader_times() const {
    std::lock_guard lock(shader_times_mutex_);
    return shader_times_;
  }
  const std::string& last_error() const { return last_error_; }
  // Every failure so far, by reason (last_error's text).
  const std::map<std::string, uint32_t>& failures() const { return failures_; }
  bool guest_memory_in_place() const { return shared_memory_imported_; }
  // For diagnostics: the textures uploaded so far by (base, format, width, height), and whether a base is the
  // destination of a resolve (with its destination format, or UINT32_MAX).
  TextureCache* texture_cache() { return textures_.get(); }
  // What the renderer holds, to see it does not grow without bound: render targets, resolves kept, pipelines.
  size_t surface_count() const { return surfaces_.size(); }
  size_t resolved_count() const { return resolved_.size(); }
  // The resolution scale a resolve kept on the GPU holds at `dest_base` (0: none there), for tests.
  uint32_t resolve_scale(uint32_t dest_base) const {
    const auto it = resolved_.find(dest_base & 0x1FFFFFFF);
    return it == resolved_.end() ? 0 : it->second.scale;
  }
  // Writes a resolve kept on the GPU at `dest_base` into guest memory now, for tools that read the destination
  // (gfx_replay --dump-resolves): false when none is kept there.
  bool WriteBackForTools(uint32_t dest_base) {
    const auto it = resolved_.find(dest_base & 0x1FFFFFFF);
    return it != resolved_.end() && it->second.image.image && WriteBackResolved(it->first, it->second);
  }
  size_t pipeline_count() const { return pipelines_.size(); }
  // A host that watches guest memory for CPU writes (as for textures): guest memory copied into video memory is then
  // copied again only after a write (GuestMemoryWritten), not at every batch. `watch(begin, end)` arms a one-shot watch.
  void SetMemoryWatch(std::function<void(uint32_t, uint32_t)> watch);
  // [begin, end) of guest memory was written (by the CPU, a resolve's write-back, a replay): its copy in video memory
  // is stale. Any thread.
  void GuestMemoryWritten(uint32_t begin, uint32_t end);
  // The CPU wrote [begin, end) of guest memory (a watched write; any thread): a resolve kept on the GPU over it is
  // superseded, never written back over what the title put there since (a title reuses a resolve's memory for
  // streamed textures and vertices; written back late, the old copy turned them to noise). Writes the renderer makes
  // itself (WritingGuestMemory on the writing thread) are not the CPU's.
  void CpuWrote(uint32_t begin, uint32_t end);
  static bool WritingGuestMemory();
  // Diagnostics: each write the GPU side makes into guest memory checked against the textures in the cache; one that
  // lands on a compressed texture (DXT, DXN, 5:6:5: never a render target's copy) is noted with where it came from
  // (TakeAuditLog, any thread). `what`: the writing path.
  void SetAuditWrites(bool on) { audit_writes_ = on; }
  // Diagnostics: told where the renderer's threads are when a wait for a batch lasts more than 5 s (once a wait).
  void SetStallLog(std::function<void(const std::string&)> log) { stall_log_ = std::move(log); }
  void AuditWrite(const char* what, uint32_t begin, uint32_t end);
  std::vector<std::string> TakeAuditLog();
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
    uint32_t primitive_restart; // strips cut where the index buffer holds the reset index
    uint32_t samples;           // host MSAA samples of the pass's targets
    uint32_t view_mask;         // the pass's views (0b11: a 2x MSAA pass drawn one layer a sample; 0: none)
    bool operator<(const PipelineKey& other) const { return std::memcmp(this, &other, sizeof(*this)) < 0; }
    bool operator==(const PipelineKey& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
  };
  // The analyzed shader (no translation yet), and its translation for a modification.
  TranslatedShader* GetShader(const gfx::Shader& shader);
  VkShaderModule GetModule(TranslatedShader& shader, uint64_t modification);
  // The pipeline of a key; asynchronously, VK_NULL_HANDLE with `pending` set while it compiles.
  VkPipeline GetPipeline(const PipelineKey& key, bool& pending);
  VkPipeline CreatePipeline(const PipelineKey& key);  // any thread
  // ANY360_PIPELINE_STATS=<file>: each pipeline's shaders as the driver compiled them, a line a pipeline (registers,
  // instructions, spills...), and with ANY360_PIPELINE_ISA=<dir> their internal representations (the GPU's assembly).
  void PipelineStats(const PipelineKey& key, VkPipeline pipeline);
  std::mutex module_names_mutex_;
  std::unordered_map<uint64_t, std::string> module_names_;  // "ps <hash>_<modification>", while the stats are on
  void CompilePipelines();                            // a compiler thread
  void TakeCompiledPipelines();                       // compile_mutex_ held
  void SavePipelineCache();                           // any thread
  // Recipes (WarmUpPipelines): plain dwords, compared as bytes.
  struct PipelineRecipe {
    uint64_t vs_hash, vs_modification, ps_hash, ps_modification;  // ps_hash 0: no pixel shader
    uint32_t vs_type, ps_type;
    uint32_t layout[4];  // the pipeline layout's counts: vertex textures and samplers, pixel textures and samplers
    uint32_t topology, cull_mode, front_face_clockwise, polygon_mode, depth_control, depth_format;
    uint32_t color_formats[4], blend_controls[4], color_masks, primitive_restart;
    uint32_t samples;  // 0 one sample with host MSAA on, 1 the host MSAA, 2 not known (recorded without host MSAA)
    uint32_t view_mask;  // PipelineKey::view_mask
    bool operator==(const PipelineRecipe& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
  };
  void RecordRecipe(const PipelineKey& key);  // the renderer's thread
  void LoadRecipes();                         // Initialize
  void SaveRecipes();                         // any thread
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
  uint32_t msaa_ = 1;   // host MSAA asked for
  bool occlusion_counts_ = true;  // SetOcclusionCounts
  VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;  // host MSAA the device has
  // The samples drawn into a surface's multisampled image since its last resolve, within the host pixels x0-x1,
  // y0-y1 (what is about to be read), into its image (outside a pass).
  // `one_sample`: sample 0 of each pixel instead of the average, the samples kept to resolve again (a copy a title
  // reads as data); averaged when the surface cannot.
  void ResolveSamples(VkCommandBuffer cb, Surface& surface, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      bool one_sample = false);
  bool SampleBitsViews(Surface& surface);
  // A surface back to one sample: its samples resolved, its multisampled image pooled once the batch has run (nothing
  // of it lost; the next surface of its size and format needing samples takes it, else freed after 2 s).
  // top, bottom: the host rows about to be written in its image (a surface drawn as views loads them again).
  void DropSamples(Surface& surface, int32_t top = 0, int32_t bottom = INT32_MAX);
  // A surface's multisampled image (not yet drawn into).
  bool CreateSamples(Surface& surface);
  // A surface drawn back at one sample given its samples again, when a clear over `area` is about to cover all it
  // holds (the multisampled image needs nothing of the single-sample one).
  void GiveSamplesBack(Surface* surface, const int32_t area[4]);
  // A surface about to be drawn by a pass of guest 2x MSAA (views true: into its layers, made and loaded first when
  // needed) or of anything else (false: its layers copied into its rows first; they are loaded again before the next
  // 2x pass). Outside a pass. False when the layers cannot be made.
  bool UseViews(Surface& surface, bool views);
  // Copies between a surface's layers and its rows over host pixels [left, right) x [top, bottom) of the rows (whole
  // pixel rows: both samples), outside a pass.
  void CopyViews(VkCommandBuffer cb, Surface& surface, int32_t left, int32_t top, int32_t right, int32_t bottom,
                 bool into_views);
  std::vector<VkImageCopy> view_regions_;  // CopyViews' regions, reused
  // Guest 2x MSAA drawn as views (the default; ANY360_2X_AS_ROWS=1: as rows, both samples at the pixel's x).
  bool views_2x_ = true;
  // Colour resolves of a surface drawn as views read its layers (ANY360_VIEWS_RESOLVE_ROWS=1: through its rows, A/B).
  bool views_resolve_ = true;
  // Before a surface is read or drawn: what the surfaces over the same EDRAM wrote since it last did, copied into it
  // at their rows (their samples first resolved and dropped, its own too). True when something was copied.
  // Rows within [skip_top, skip_bottom) (host rows) are left: the caller overwrites them whole.
  // `need_bottom`: the host rows the caller uses (depth under the rows below it is converted later, when needed).
  bool SyncAliases(Surface& surface, int32_t skip_top = 0, int32_t skip_bottom = 0, int32_t need_bottom = INT32_MAX);
  // Whether a surface over shared EDRAM has rows another one wrote since it last held them.
  bool AliasesNewer(const Surface* surface) const {
    if (surface && surface->depth_over) return true;
    if (!surface || !surface->group || surface->group->members < 2) return false;
    const EdramGroup& g = *surface->group;
    for (size_t i = 0; i < surface->have.size(); ++i) {
      if (g.version[surface->row0 + i] > surface->have[i] && g.writer[surface->row0 + i] != surface) return true;
    }
    return false;
  }
  // The tile rows of `surface` host rows [top, bottom) cover, written by it now.
  void MarkWritten(Surface* surface, int32_t top, int32_t bottom);
  std::map<std::array<uint32_t, 4>, std::unique_ptr<EdramGroup>> edram_groups_;
  uint64_t edram_version_ = 0;
  uint32_t viewport_extent_ = 4096;  // the host viewport's size for every draw (a power of two: Draw)
  // What a surface's samples or layers hold newer, less a rectangle its single-sample image now holds the same: by
  // the band rule of ResolveSamples (only a band across what was drawn takes it in).
  static void ShrinkDrawn(Surface& s, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t* d = s.drawn;
    if (x0 <= d[0] && x1 >= d[2]) {
      if (y0 <= d[1]) d[1] = std::max(d[1], y1);
      if (y1 >= d[3]) d[3] = std::min(d[3], y0);
    } else if (y0 <= d[1] && y1 >= d[3]) {
      if (x0 <= d[0]) d[0] = std::max(d[0], x1);
      if (x1 >= d[2]) d[2] = std::min(d[2], x0);
    }
    if (d[2] <= d[0] || d[3] <= d[1]) d[0] = d[1] = INT32_MAX, d[2] = d[3] = INT32_MIN;
  }
  // `writes`: the rows change (false: a depth target only tested).
  void GrowDrawn(Surface* surface, int32_t left, int32_t top, int32_t right, int32_t bottom, bool writes = true) {
    if (!surface) return;
    if (surface->group) MarkWritten(surface, top, bottom);
    int32_t* w = surface->written;
    w[0] = std::min(w[0], left), w[1] = std::min(w[1], top), w[2] = std::max(w[2], right), w[3] = std::max(w[3], bottom);
    if (writes && surface->views.image && !surface->use_views) {  // its rows drawn, not its layers
      surface->StaleRows(top, bottom);
    }
    if (!surface->ms.image && !surface->use_views) return;
    surface->drawn[0] = std::min(surface->drawn[0], left), surface->drawn[1] = std::min(surface->drawn[1], top);
    surface->drawn[2] = std::max(surface->drawn[2], right), surface->drawn[3] = std::max(surface->drawn[3], bottom);
  }
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
  // Recipes: those recorded and loaded (in order, once each), the shaders they name (microcode by type and hash), and
  // what a pipeline key's handles were made from. recipes_mutex_ guards what SaveRecipes reads from another thread.
  std::mutex recipes_mutex_;
  std::vector<PipelineRecipe> recipe_list_;
  std::unordered_set<PipelineRecipe, BytesHash> recipe_set_;
  std::map<std::pair<uint32_t, uint64_t>, std::vector<uint32_t>> recipe_shaders_;
  bool recipes_dirty_ = false;
  std::unordered_map<uint64_t, std::pair<const TranslatedShader*, uint64_t>> module_sources_;  // module: shader, modification
  std::unordered_map<uint64_t, std::array<uint32_t, 4>> layout_counts_;  // pipeline layout: its counts
  size_t recipes_loaded_ = 0, warm_next_ = 0;  // recipe_list_[warm_next_ ...] are still to be queued
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
  // Memory exports (memexport): the guest ranges a draw's shaders wrote, each at an offset of the readback buffer
  // (guest memory in video memory: copied there at the flush, then into guest memory once the batch is done).
  struct MemExport {
    uint32_t base, bytes, readback;
  };
  std::vector<MemExport> memexports_;
  Buffer memexport_readback_;
  void MemExported(std::vector<MemExport> exports);
  std::vector<MemExport> MemExportRanges(const uint32_t* regs, const TranslatedShader* vs, const TranslatedShader* ps) const;
  // A draw nothing of which reaches a target (EDRAM mode other than 4 or 5, no target, an empty scissor): a vertex
  // shader exporting to memory still runs, as a compute dispatch of its translation for memexport (one invocation a
  // guest vertex). False when the vertex shader exports nothing.
  bool MemExportWithoutRasterization(const DrawCall& draw);
  std::map<VkShaderModule, VkPipeline> memexport_pipelines_;
  uint32_t memexport_dispatches_ = 0;
 public:
  uint32_t memexport_dispatches() const { return memexport_dispatches_; }
 private:
  bool vram_guest_on_ = false;
  // Pages (4 KB) of guest memory whose copy in video memory is current (with a watch set; else none are kept).
  static constexpr uint32_t kGuestPageShift = 12;
  std::unique_ptr<std::atomic<uint64_t>[]> vram_valid_;
  // By page: how often the CPU wrote it after it was copied (each a fault on the watch), halved every few hundred
  // frames. A page written that often (a vertex buffer the title fills every frame) is copied again by every batch
  // that reads it instead, never watched: a copy of 4 KB costs far less than the fault.
  std::unique_ptr<std::atomic<uint8_t>[]> vram_writes_;
  static constexpr uint8_t kHotPageWrites = 3;
  uint32_t hot_decay_frames_ = 0;
  std::vector<VkBufferCopy> watch_runs_;
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
    // What guest memory holds of this destination: the Signature of the last write-back that landed there (0: none).
    // A late read (late_resolve_reads) is only right when it is this resolve's: a destination two resolves share in
    // a frame (Call of Duty: Black Ops's shadow depth, then its colour grading table) holds the other one's texels.
    std::atomic<uint64_t> landed{0};
    uint64_t Signature() const {
      return (uint64_t(image.format) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(pitch) << 48 | uint64_t(width) << 32 |
                                                                  uint64_t(height) << 16 | (x ^ y << 8));
    }
    bool float24 = false;  // a depth resolve from a D24FS8 surface (written back as float24 words)
    uint32_t scale = 1;    // host texels a guest texel along each axis (colour resolves: the resolution scale)
    Image small;           // a scaled resolve brought to one texel a guest texel for its write-back
    VkImageLayout small_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The images of the other formats last resolved here (titles alternate colour and depth resolves to one
    // destination, Crackdown three formats at one address in its cut-scenes): swapped back in instead of an image
    // made and freed at each resolve. The oldest beyond kSpares is freed.
    struct Spare {
      Image image;
      VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
      uint32_t scale = 1;
      Image small;  // its own small image, swapped in with it (else made again at each write-back)
      VkImageLayout small_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    static constexpr size_t kSpares = 4;
    std::vector<Spare> spares;  // oldest first
  };
  // Writes a resolve kept on the GPU into guest memory: now (WriteBackResolved: the batch runs), or when the batch
  // has run (QueueWriteBack: its copy recorded in the batch; the CPU reads guest memory only after a fence, which runs
  // the batch first).
  bool WriteBackResolved(uint32_t base, Resolved& r);
  // The resolve image for `base` in `format`, at least `pitch` x `rows`: made, or grown keeping what earlier
  // resolves wrote there (copied in the batch; the old image freed once the batch has run).
  bool EnsureResolveImage(Resolved& r, VkFormat format, uint32_t pitch, uint32_t rows, VkImageUsageFlags usage);
  bool QueueWriteBack(uint32_t base, Resolved& r);
  // Textures sampling a resolve's image in place (TextureCache::SetSourceView): the destination's Resolved while
  // `image` is still its image, its views by format and components, and the image put in a layout for the next use.
  Resolved* SourceResolved(uint32_t base, VkImage image);
  VkImageView SourceView(uint32_t base, VkImage image, VkFormat format, VkComponentMapping components);
  bool SourceLayout(uint32_t base, VkImage image, VkImageLayout layout);
  // A resolve image no batch recorded from now on uses: freed with its sampled views once the batch has run.
  void RetireResolveImage(Image& image);
  struct PendingWriteBack {
    Buffer buffer;
    Resolved* resolved;
    uint32_t base, x, y, width, height, pitch, endian, element;
    bool depth, float24;
    uint64_t signature;  // the resolve's Signature when queued
  };
  // Batches in flight: each submission keeps what its commands use until the GPU has run it. A completion thread
  // waits for them in order, then does what was to follow each (guest memory writes of resolves, the command
  // processor's fences and interrupts) and frees what it used (staging buffers, retired images).
  static constexpr uint32_t kGpuMarks = 1024;  // time stamps a batch
  // A surface's samples dropped (DropSamples): kept once the batch that last drew them has run, for the next surface of
  // the same size and format that needs samples (a title that draws a target with and without its pair every frame
  // would otherwise create and free a multisampled image a frame, 160 MB at 1440p with 4 samples).
  struct SpareSamples {
    Image ms;
    VkImageView bits = VK_NULL_HANDLE;  // its integer view, when it had one
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::chrono::steady_clock::time_point since{};  // in the pool since
  };
  struct Batch {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    std::atomic<uint32_t> marks{0};          // GpuMark time stamps written (recording and submitting threads)
    std::vector<std::pair<uint64_t, uint64_t>> timing_keys;  // ANY360_DRAW_TIMING: each draw's pixel / vertex hash
    std::vector<uint32_t> timing_indices;                     // and its index count
    std::vector<std::pair<uint64_t, uint64_t>> shader_pairs;  // the vertex / pixel shaders it draws with (stall report)
    uint32_t memexport_dispatches = 0;                        // compute dispatches for memexport it records
    std::vector<std::string> checkpoints;  // its draws marked for the GPU, described (ANY360_GPU_CHECKPOINTS)
    std::array<uint8_t, kGpuMarks> mark_kinds{};
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
    std::vector<SpareSamples> spare_samples;  // dropped samples, pooled once it has run
    std::vector<VkImageView> retired_views;  // of textures dropped from the cache
    // Occlusion queries ended in it: their counts written into guest memory once it has run.
    struct Query {
      uint32_t address;
      std::vector<uint32_t> parts;  // host queries (a draw each), maybe of earlier batches
    };
    std::vector<Query> queries;
    std::vector<PendingWriteBack> write_backs;
    std::vector<std::function<void()>> actions;
    bool done = true;  // run, and what follows it done (guarded by done_mutex_)
    // Its draws read no guest memory once submitted (what they fetch was copied on the CPU at the flush): what follows
    // it (fences) needs not wait for the GPU to run it (resolves written back land later, as the CPU rarely reads them).
    bool captured = false;
    bool submitted = false, released = false;  // on the queue; what follows it done then (guarded by done_mutex_)
    Buffer guest_staging;  // the guest memory its draws fetch, copied at the flush
    bool uploaded = false;  // its upload command buffer ran too (time stamped)
  };
  // A time stamp in `cb` (a batch's, run in submission order) where work of `kind` starts.
  void GpuMark(Batch& b, VkCommandBuffer cb, GpuKind kind);
  static constexpr uint32_t kBatches = 8;
  // Occlusion queries: a host query around each draw of an open guest query (so none spans command buffers or render
  // passes), a range of the pool by batch, reset when the batch begins; the guest query's count is their sum.
  static constexpr uint32_t kQueriesPerBatch = 1024;
  VkQueryPool query_pool_ = VK_NULL_HANDLE;
  uint32_t queries_used_ = 0;  // of the open batch's range
  bool query_open_ = false;
  std::vector<uint32_t> query_parts_;
  void WriteQuery(const Batch::Query& query);
  int32_t last_submitted_ = -1;  // the batch submitted last (its actions take what follows it)
  std::array<Batch, kBatches> batches_;
  uint32_t current_ = 0;  // the batch being recorded (not in flight)
  Batch& batch() { return batches_[current_]; }
  void FinishBatch(Batch& b);  // on the completion thread
  void FinishWriteBacks(Batch& b);
  // The batches submitted after every batch before them was done or released, and captured, released: what follows
  // them is taken into `actions`, in order (under done_mutex_).
  void TakeReleased(std::vector<std::function<void()>>& actions);
  bool fences_when_captured_ = true;
  bool late_resolve_reads_ = false;
  // CpuWrote's ranges, applied on the renderer's thread before resolves are written back or read (ApplyCpuWrites).
  std::mutex cpu_writes_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> cpu_writes_;
  std::atomic<bool> any_cpu_writes_{false};
  void ApplyCpuWrites();
  bool audit_writes_ = false;
  std::function<void(const std::string&)> stall_log_;
  // Where the completion and submitter threads are (WaitBatch's stall report): phase codes and the batch.
  std::atomic<uint32_t> complete_phase_{0}, complete_batch_{0}, submit_phase_{0}, submit_batch_{0};
  std::mutex audit_mutex_;
  struct AuditedWrite {
    const char* what;
    uint32_t begin, end;
  };
  std::vector<AuditedWrite> audit_pending_;  // checked on the renderer's thread at each swap (CheckAuditedWrites)
  std::vector<std::string> audit_log_;
  uint32_t audit_logged_ = 0;
  void CheckAuditedWrites();

 public:
  // Audited writes that landed on a compressed texture, and among them on one used this frame or the last.
  std::atomic<uint64_t> audit_hits_{0}, audit_hits_in_use_{0};

 private:
  int32_t submitted_unlisted_ = -1;  // the batch handed to the submission thread before Flush listed it in flight
  std::vector<std::function<void()>> released_actions_;  // the submission thread's
  void WaitBatch(uint32_t index);
  void Complete();  // the completion thread
  std::mutex done_mutex_;
  // Dropped samples of finished batches (FinishBatch, the completion thread), taken by CreateSamples; freed after 2 s
  // unused (FreeSpareSamples at each frame's end) or all at the end.
  std::mutex sample_pool_mutex_;
  std::vector<SpareSamples> sample_pool_;
  void FreeSpareSamples(bool all);
  std::condition_variable done_cv_, work_cv_;
  std::deque<uint32_t> in_flight_;
  bool stopping_ = false;
  std::thread completer_;
  std::set<uint32_t> front_on_gpu_;  // front buffers presented from their resolve image (not read back)
  // Reads `image` (RGBA8, BGRA8 or a 10-bit A2B10G10R10 / A2R10G10B10 one) over a rectangle as RGBA8 pixels.
  bool ReadImageRGBA8(Image& image, VkImageLayout& layout, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      std::vector<uint32_t>& pixels);
  // A depth resolve kept on the GPU: the depth of one sample a pixel into an R32F image (DepthPass, depth_shader.cpp).
  bool ResolveDepthOnGpu(Surface& surface, int32_t x0, int32_t y0, uint32_t width, uint32_t height, uint32_t sample_x,
                         uint32_t sample_y, uint32_t sample, uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian);
  bool DepthPass();
  // EDRAM a depth surface wrote last, under a colour surface of the same pitch whose host texels are the EDRAM word
  // (k_8_8_8_8, k_2_10_10_10): the depth words (depth << 8 | stencil, as the console lays them out) copied into it.
  // Titles clear a colour buffer by drawing depth over its EDRAM (Mirror's Edge's light buffer): without it the
  // colour surface kept the frame before, and the lighting added it to characters (seen through, half transparent).
  bool SyncDepthIntoColour(Surface& s, int32_t need_bottom, int32_t skip_top, int32_t skip_bottom);
  void TrimResolvesBelow(uint32_t dest_base);
  bool CopyDepthWords(Surface& depth, Surface& colour, uint32_t depth_row, uint32_t colour_row, uint32_t rows);
  bool DepthWordPass();
  VkDescriptorSetLayout depth_word_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_word_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_word_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_word_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet depth_word_set_ = VK_NULL_HANDLE;
  Buffer depth_word_buffer_;  // the depth and stencil copies and the words, grown as needed
  // CopyViews within a batch: the rows through a buffer, reordered by a compute pass (view_rows_shader.cpp).
  // Its pipeline, and its buffer grown to need bytes (a flush and a wait when it grows: outside any command buffer
  // a caller holds).
  bool ViewRowsPass(VkDeviceSize need);
  uint32_t ViewTexelBytes(const Surface& surface) const;  // a texel's bytes in that buffer (depth: its depth aspect)
  VkDescriptorSetLayout view_rows_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout view_rows_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline view_rows_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool view_rows_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet view_rows_set_ = VK_NULL_HANDLE;
  Buffer view_rows_buffer_;  // both orders of the rows copied, grown as needed
  VkDescriptorSetLayout depth_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  // A resolve's exponent bias on the GPU: an RGBA16F resolve image's texels over a rectangle times 2^bias, in place
  // (scale_shader.cpp; its sets from the batch's depth pool).
  bool ScalePass();
  bool ScaleOnGpu(VkCommandBuffer cb, Image& image, VkImageLayout& layout, int32_t x, int32_t y, uint32_t width,
                  uint32_t height, int32_t bias);
  VkDescriptorSetLayout scale_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout scale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline scale_pipeline_ = VK_NULL_HANDLE;
  // k_16_16_16_16 targets resolved to k_16_16_16_16 textures on the GPU (fixed16_shader.cpp): the depth copy's set
  // layout (a sampled image, a storage image), when the device stores R16G16B16A16_UNORM in storage images.
  bool Fixed16Pass();
  // The compute pass averaging a 2x surface's two layers into a half-float resolve image (the depth copy's set
  // and pipeline layouts).
  bool ViewsResolvePass();
  VkPipeline views_resolve_pipeline_ = VK_NULL_HANDLE;
  bool fixed16_storage_ = false;
  VkPipelineLayout fixed16_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline fixed16_pipeline_ = VK_NULL_HANDLE;
  static constexpr uint32_t kDepthSets = 64;
  uint32_t depth_sets_ = 0;
  std::map<uint32_t, Resolved> resolved_;
  // The views textures sample resolve images through, by image: (format << 32 | components, view).
  std::unordered_map<VkImage, std::vector<std::pair<uint64_t, VkImageView>>> source_views_;
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
                    uint32_t sample_y, uint32_t dest_base, uint32_t dest_pitch, uint32_t dest_endian, VkFormat format,
                    int32_t dest_dx = 0, int32_t dest_dy = 0, bool fill_half_pixel = false, int32_t bias = 0);
  std::set<uint32_t> front_buffers_;  // bases swapped as front buffers: resolves there are always read back
  void ReadsGuestMemory(uint32_t address, uint32_t bytes);
  VkQueryPool timestamps_ = VK_NULL_HANDLE;  // four a batch: start, end, its upload's start, end
  VkQueryPool gpu_marks_ = VK_NULL_HANDLE;   // kGpuMarks a batch (GpuMark)
  static constexpr uint32_t kDrawTimings = 16384;  // a batch's draws timed (ANY360_DRAW_TIMING)
  VkQueryPool draw_timing_ = VK_NULL_HANDLE;
  mutable std::mutex shader_times_mutex_;
  std::map<uint64_t, ShaderTime> shader_times_;
  std::array<std::chrono::steady_clock::time_point, 8> submitted_at_{};
  uint32_t constant_head_ = 0, index_head_ = 0;  // ring allocation of the batch's constants and indices
  // The size every batch's rings are given when it is next free: small at first, doubled whenever a draw finds no room
  // (a flush the frame did not ask for), up to the ceilings.
  static constexpr VkDeviceSize kConstantRingFirst = 2u << 20, kConstantRingMost = 32u << 20;
  static constexpr VkDeviceSize kIndexRingFirst = 1u << 20, kIndexRingMost = 16u << 20;
  VkDeviceSize constants_capacity_ = kConstantRingFirst, indices_capacity_ = kIndexRingFirst;
  void GrowRing(VkDeviceSize& capacity, VkDeviceSize need, VkDeviceSize most);
  bool SizeBatchRings(Batch& b);  // the batch's rings brought to the current capacities (the batch free)
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
    enum class Indices : uint8_t { kNone, kRectangles, kQuads, kLineLoop, kDma } indices;
    bool index_32, restart;
    Endian endian;
    DrawSource source;
    VkIndexType index_type;
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    uint8_t* index_dst;
    const uint8_t* index_src;
    uint32_t index_bytes, primitives, reset_index, host_index_count;
    uint32_t query;  // the host occlusion query around the draw (UINT32_MAX: none)
    uint32_t timing;  // its time stamp in draw_timing_ (ANY360_DRAW_TIMING; UINT32_MAX: none)
    uint64_t checkpoint;  // its GPU marker (ANY360_GPU_CHECKPOINTS; 0: none): batch + 1 above, draw + 1 below
    // The texture sets a recording thread makes (sets[2 + stage] then), by layout: images, then samplers.
    static constexpr uint32_t kMaxBindings = 16;
    VkDescriptorSetLayout texture_layouts[2];
    uint8_t view_count[2], sampler_count[2];
    VkImageView views[2][kMaxBindings];
    VkSampler samplers[2][kMaxBindings];
  };
  void RecordJob(VkCommandBuffer cb, const DrawJob& job, RecordedState& rs, std::vector<uint16_t>& scratch) const;
  // After a lost device: the draw each pipeline stage last passed (ANY360_GPU_CHECKPOINTS), to the stall log.
  void ReportCheckpoints() const;
  // Recording threads: chunks of the open render pass's draws, each into a secondary command buffer; the pass's
  // chunks run in order when it ends (EndRendering waits for them).
  struct RecordTask {
    std::vector<DrawJob> jobs;
    uint32_t batch = 0;
    VkFormat formats[4] = {};  // the render pass's attachments
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t view_mask = 0;
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
    uint32_t view_mask = 0;
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
  uint32_t submitting_index_ = 0;  // the batch being submitted, while submitting_
  bool segment_secondary_ = false;  // the open pass takes secondary command buffers
  VkFormat segment_formats_[4] = {};
  VkFormat segment_depth_ = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits segment_samples_ = VK_SAMPLE_COUNT_1_BIT;
  uint32_t segment_view_mask_ = 0;
  static constexpr uint32_t kChunkDraws = 32;
  // By texture fetch constant (32), view type (3) and signedness: the last view and swizzled signs for its six words,
  // reused while the texture cache says views stay valid (no search by the fetch constant's words then).
  struct TextureMemo {
    uint32_t fetch[6] = {};
    uint64_t version = 0, epoch = 0;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t* used = nullptr;  // the texture's last-use frame, marked at each reuse (TextureCache::last_used)
    uint32_t signs = 0;
    bool scaled = false;  // a resolution-scaled texture (copied from a scaled resolve)
    bool patched = false;  // a patched texture (TextureCache::last_view_patched)
  };
  std::array<TextureMemo, 32 * 3 * 2> texture_memo_;
  uint32_t evict_frames_ = 0;  // frames since textures were last evicted
  // By texture fetch constant: the last sampler for its words and the binding's filters.
  struct SamplerMemo {
    uint32_t key[10] = {};
    uint64_t patches = 0;  // the patches' generation it was chosen under (a patched texture samples its mips)
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
  // One set, LUT and output image a presented frame, used in turn: the presenter keeps frames in flight, and a frame
  // still running on the GPU must not see its set rewritten (a lost device) or its image overwritten.
  struct GammaFrame {
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buffer lut;
    Image image;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  static constexpr uint32_t kGammaFrames = 4;  // more than the presenter's frames in flight
  std::array<GammaFrame, kGammaFrames> gamma_frames_;
  uint32_t gamma_next_ = 0;
  std::array<std::array<uint8_t, 256>, 3> gamma_lut_ = [] {
    std::array<std::array<uint8_t, 256>, 3> lut{};
    for (auto& channel : lut) {
      for (uint32_t i = 0; i < 256; ++i) channel[i] = uint8_t(i);
    }
    return lut;
  }();
};

}  // namespace any360::gfx::vk
