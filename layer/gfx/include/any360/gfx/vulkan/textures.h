// Textures of any360::gfx's Vulkan renderer: what a texture fetch constant (6 dwords of the register file) describes,
// read from guest memory, endian-swapped, untiled and uploaded into a host image, viewed with the guest swizzle; and
// the samplers fetch constants and fetch instructions ask for.
//
// First step, deliberately narrow: 2D textures, base level only (no mips, no packed mip tail: textures of 16 texels or
// less a side are refused), unsigned components, the formats a host image can hold as they are (8, 8_8, 8_8_8_8,
// 2_10_10_10, DXT1/3/5, 16-bit and 32-bit float); untiling and conversion on the CPU, a texture uploaded again only
// when its memory changed (hashed per use). Cube, 3D, stacked, signed and mip-mapped textures are later steps.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "any360/gfx/diagnostics.h"
#include "device.h"

namespace any360::gfx::vk {

struct TextureFormatInfo {
  VkFormat format;
  uint32_t block_bytes;  // bytes a block (a texel, or a 4x4 block for DXT) takes
  uint32_t block_size;   // texels a block spans in x and y (1, or 4 for DXT)
  uint32_t swizzle;      // the format's own component swizzle (Xenos 3-bit codes: 0-3 RGBA, 4 zero, 5 one)
  // How each texel is converted for the host format (kNone: copied as it is): 32-bit depth words to float, or
  // 16-bit 5:6:5 and 1:5:5:5 with red and blue exchanged (the guest's red is in the low bits, the host's in the
  // high bits); 32-bit 10:11:11 or 11:11:10 (red in the low bits) unpacked to 16-bit normalized RGBA (8 bytes).
  enum Convert : uint32_t {
    kNone, kDepthUnorm24, kDepthFloat24, kSwapRedBlue565, kSwapRedBlue1555, kUnpack10_11_11, kUnpack11_11_10
  } convert = kNone;
  // The format a view reads signed components with (the same bits as signed normalized; float formats are signed
  // already), VK_FORMAT_UNDEFINED when there is none yet.
  VkFormat signed_format = VK_FORMAT_UNDEFINED;};

// Xenos float24 depth (20e4: 4 exponent bits, bias 15, 20 mantissa bits, no sign) to and from float, as the
// reference converts it (round to nearest even; negative and NaN give 0, too large the largest value).
float Float20e4ToFloat(uint32_t f24);
uint32_t FloatTo20e4(float value);
// The host format of a Xenos texture format, or nullptr when it is not handled yet.
const TextureFormatInfo* GuestTextureFormat(uint32_t xenos_format);

// The byte offset of block (x, y) in a tiled 2D surface `pitch` blocks wide (Xenos 32x32-block tiling).
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2);
// The byte offset of block (x, y, z) in a tiled 3D texture `pitch` blocks wide and `height` blocks high (32x32x4
// tiles, as XGRAPHICS::TileVolume lays them).
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t bytes_per_block_log2);

// A texture fetch constant's fields this module uses.
struct TextureFetch {
  uint32_t type = 0, signs = 0, clamp_x = 0, clamp_y = 0, clamp_z = 0, pitch = 0, tiled = 0;
  uint32_t format = 0, endian = 0, base = 0;  // base in bytes
  uint32_t width = 0, height = 0, depth = 0;
  uint32_t swizzle = 0, mag_filter = 0, min_filter = 0, mip_filter = 0, aniso = 0;
  uint32_t border_color = 0, dimension = 0, packed_mips = 0;
  uint32_t mip_min = 0, mip_max = 0, mip_address = 0;  // the mip levels' range, their storage (0: none)
  static TextureFetch Parse(const uint32_t dwords[6]);
  // The levels the texture has, as the reference reads them from the fetch constant: [first, last], level 0 from
  // the base address, levels 1 and up from the mip address.
  void Levels(uint32_t& first, uint32_t& last) const;
  // Slices of a 2D array or cube (1 for others).
  uint32_t slices() const { return dimension != 0 ? depth : 1; }  // stacked layers, cube faces or 3D z slices
};

class TextureCache {
 public:
  TextureCache(Device& device, const uint8_t* physical, uint32_t mirror_bytes);
  ~TextureCache();

  // The view types the translated shaders declare: 2D and stacked textures as 2D arrays, 3D, cube.
  enum class ViewType { k2DArray, k3D, kCube };
  // A view of `type` of the texture `fetch` describes, with its swizzle, uploaded and ready for sampling; null with
  // `error` when it is not handled yet. A shader declares a 2D-array and a 3D binding for a fetch that can be either
  // and picks one at run time: the binding the texture's dimension does not match gets an empty texture.
  // `is_signed`: the view the shader's signed binding reads (the texture's signed format).
  VkImageView View(const uint32_t fetch[6], ViewType type, std::string& error, bool is_signed = false);
  // The sampler for `fetch` with a fetch instruction's filters (3 / 7: take the fetch constant's).
  VkSampler Sampler(const uint32_t fetch[6], uint32_t mag, uint32_t min, uint32_t mip, uint32_t aniso);

  // The texture `fetch` describes as RGBA8 pixels, swizzled as the guest asks (a front buffer to present; k_8_8_8_8
  // only for now).
  bool ReadRGBA8(const uint32_t fetch[6], uint32_t& width, uint32_t& height, std::vector<uint32_t>& pixels,
                 std::string& error) const;

  uint32_t uploads() const { return uploads_; }
  // Time spent hashing texture memory and loading levels for uploads (untiling, conversion), in nanoseconds.
  uint64_t ns_hash() const { return ns_hash_; }
  uint64_t ns_load() const { return ns_load_; }
  // The textures uploaded, by (base, format, width, height): count, for finding what is uploaded again and again.
  std::map<std::array<uint32_t, 4>, uint32_t>& upload_log() { return upload_log_; }
  // When guest memory may have changed, so a texture's content is hashed again before its next use (at most once
  // between two such events): a new frame (the CPU may have written anywhere), or [begin, end) written by the GPU
  // (a resolve) or reported by the title (coherency).
  void NewFrame() { ++epoch_; }
  // What a view returned earlier stays valid for (the renderer remembers views by fetch constant): the same epoch
  // (unless memory is watched), the same version, and no change queued.
  uint64_t epoch() const { return epoch_; }
  uint64_t version() const { return version_; }
  bool changes_queued() const { return any_changed_.load(std::memory_order_acquire); }
  // A host that can watch guest memory for CPU writes (the reference runtime's physical memory access callbacks):
  // `watch(begin, end)` arms a one-shot watch, and the host reports writes with QueueMemoryChanged. Watched textures
  // are hashed again only after such a write, not at every frame.
  void SetMemoryWatch(std::function<void(uint32_t, uint32_t)> watch) { watch_ = std::move(watch); }
  bool watching() const { return bool(watch_); }
  size_t texture_count() const { return textures_.size(); }
  // The frame the texture of the last view View returned was last used in, for a caller that reuses the view without
  // asking again to keep it marked (valid while version() is unchanged); null for an empty texture.
  uint64_t* last_used() const { return last_used_; }
  // The textures unused for more than `frames` frames, dropped: their images and views go to the caller, to destroy
  // once no batch reads them. Views returned before are invalid then (version() changes).
  void Evict(uint64_t frames, std::vector<Image>& images, std::vector<VkImageView>& views);
  void MemoryChanged(uint32_t begin, uint32_t end);
  // Several ranges at once (sorted and merged in place): one pass over the textures, however many there are.
  void MemoryChanged(std::vector<std::pair<uint32_t, uint32_t>>& ranges);
  // Where uploads are recorded: the renderer's open batch (outside rendering), so an upload is one more command in it
  // rather than a submission waited for; draws recorded before it still read the old content, in GPU order. Without
  // one, each upload is submitted and waited for. BatchDone: the batch ran, its staging buffers are free.
  void SetRecorder(std::function<VkCommandBuffer()> recorder) { recorder_ = std::move(recorder); }
  // The staging buffers of the uploads recorded since the last call, for the batch that reads them to free.
  void TakeStaging(std::vector<Buffer>& out);
  // Staging buffers back from a batch that has run (any thread): kept for later uploads instead of freed.
  void RecycleStaging(std::vector<Buffer>& buffers);
  // MemoryChanged from another thread (the renderer's completion thread): applied before the next texture use.
  void QueueMemoryChanged(uint32_t begin, uint32_t end);

  // Guest memory a resolve left on the GPU (its texels at their coordinates in `image`, kept in TRANSFER_SRC_OPTIMAL
  // layout): a texture that reads exactly that destination (same base, pitch, host format and endianness; tiled 2D,
  // one level) is copied from the image on the GPU instead of being read from memory, which a large resolve does not
  // write. A new resolve to the base replaces it (with a new generation); DropGpuSources forgets those in a range.
  struct GpuSource {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t base = 0, pitch = 0, endian = 0, width = 0, height = 0;
    uint64_t generation = 0;
    bool depth = false;  // a depth resolve (R32F depth values): read by the depth formats (k_24_8, k_24_8_FLOAT)
    uint32_t scale = 1;  // host texels a guest texel along each axis (a resolution-scaled resolve)
  };
  void SetGpuSource(const GpuSource& source) {
    gpu_sources_[source.base] = source;
    ++version_;
  }
  // Called before a texture's memory [begin, end) is read on the CPU: the renderer writes there what a resolve left
  // on the GPU only, when the texture does not read it as it is (another layout, stacked slices...).
  void SetBeforeMemoryRead(std::function<void(uint32_t, uint32_t)> callback) { before_read_ = std::move(callback); }
  void DropGpuSources(uint32_t begin, uint32_t end);
  // Whether the last view View returned is of a resolution-scaled texture (copied from a scaled GPU source: scale x
  // scale texels a guest texel), which the shaders sample with the scale taken into account.
  bool last_view_scaled() const { return last_view_scaled_; }
  uint32_t gpu_copies() const { return gpu_copies_; }
  // For diagnostics: textures over a GPU source that could not use it, by reason.
  std::map<std::string, uint32_t>& source_misses() { return source_misses_; }

 private:
  // A level's blocks in host order, slice after slice (level 0: the base).
  bool LoadLevel(const TextureFetch& t, const TextureFormatInfo& info, uint32_t level, std::vector<uint8_t>& blocks,
                 std::string& error) const;
  bool LoadBlocks(const uint32_t fetch[6], std::vector<uint8_t>& blocks, std::string& error) const;

  struct Texture {
    Image image;  // the view member is unused: views are per swizzle
    uint64_t content_hash = 0;
    uint64_t validated = 0;       // the epoch its content was last hashed in
    uint32_t begin = 0, end = 0;  // the guest memory its levels span
    uint64_t gpu_generation = 0;  // the GPU source generation it was copied from (0: from memory)
    uint64_t used = 0;            // the epoch (frame) it was last used in
    std::map<uint32_t, VkImageView> views;  // by host swizzle, bit 16: signed
  };

  Device& device_;
  const uint8_t* physical_;
  uint32_t mirror_bytes_;
  std::map<std::array<uint32_t, 5>, std::unique_ptr<Texture>> textures_;
  std::map<std::array<uint32_t, 10>, VkSampler> samplers_;
  std::vector<uint8_t> scratch_;
  Image empty_[3];  // 1x1 black textures by ViewType, made on first use
  VkImageView Empty(ViewType type, std::string& error);
  uint32_t uploads_ = 0;
  uint64_t ns_hash_ = 0, ns_load_ = 0;
  std::map<std::array<uint32_t, 4>, uint32_t> upload_log_;
  uint64_t epoch_ = 1;
  std::function<void(uint32_t, uint32_t)> watch_;
  // Views already checked, by fetch constant, view type and signedness: valid until the next frame or the next change
  // of guest memory or GPU sources (version_), so most draws skip parsing, placing and hashing.
  uint64_t version_ = 1;
  struct FastKeyHash {
    size_t operator()(const std::array<uint32_t, 8>& key) const {
      uint64_t h = 0xCBF29CE484222325ull;
      for (uint32_t v : key) h = (h ^ v) * 0x100000001B3ull;
      return size_t(h ^ (h >> 31));
    }
  };
  struct FastView {
    VkImageView view;
    uint64_t epoch, version;
    bool scaled;
    uint64_t* used;
  };
  bool last_view_scaled_ = false;
  uint64_t* last_used_ = nullptr;
  std::unordered_map<std::array<uint32_t, 8>, FastView, FastKeyHash> fast_;
  std::function<VkCommandBuffer()> recorder_;
  std::vector<Buffer> pending_staging_;  // staging buffers of uploads recorded in the open batch
  // Staging buffers free for uploads (a video plane each frame would otherwise allocate device memory each frame).
  bool AcquireStaging(VkDeviceSize size, Buffer& out);
  std::mutex staging_mutex_;
  std::vector<Buffer> free_staging_;
  VkDeviceSize free_staging_bytes_ = 0;
  // Reused by each upload: the levels' blocks and copy regions.
  std::vector<uint8_t> upload_blocks_, upload_level_;
  std::vector<VkBufferImageCopy> upload_regions_;
  std::mutex changed_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> changed_;  // queued MemoryChanged ranges
  std::vector<std::pair<uint32_t, uint32_t>> changed_taken_;  // reused to apply them
  std::atomic<bool> any_changed_{false};  // checked by each View without the lock
  std::map<uint32_t, GpuSource> gpu_sources_;  // by base
  std::function<void(uint32_t, uint32_t)> before_read_;
  uint32_t gpu_copies_ = 0;
  std::map<std::string, uint32_t> source_misses_;
};

}  // namespace any360::gfx::vk
