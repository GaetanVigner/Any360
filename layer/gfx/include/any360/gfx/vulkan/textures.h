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
#include <string>
#include <vector>

#include "device.h"

namespace any360::gfx::vk {

struct TextureFormatInfo {
  VkFormat format;
  uint32_t block_bytes;  // bytes a block (a texel, or a 4x4 block for DXT) takes
  uint32_t block_size;   // texels a block spans in x and y (1, or 4 for DXT)
  uint32_t swizzle;      // the format's own component swizzle (Xenos 3-bit codes: 0-3 RGBA, 4 zero, 5 one)
  // How each texel is converted for the host format (kNone: copied as it is): 32-bit depth words to float, or
  // 16-bit 5:6:5 with red and blue exchanged (the guest's red is in the low bits, the host's in the high bits).
  enum Convert : uint32_t { kNone, kDepthUnorm24, kDepthFloat24, kSwapRedBlue565 } convert = kNone;
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
  uint32_t slices() const { return dimension == 1 || dimension == 3 ? depth : 1; }
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
  // When guest memory may have changed, so a texture's content is hashed again before its next use (at most once
  // between two such events): a new frame (the CPU may have written anywhere), or [begin, end) written by the GPU
  // (a resolve) or reported by the title (coherency).
  void NewFrame() { ++epoch_; }
  void MemoryChanged(uint32_t begin, uint32_t end);
  // Called before an upload replaces a texture's content (the renderer runs its pending draws first).
  void SetBeforeUpload(std::function<void()> callback) { before_upload_ = std::move(callback); }

 private:
  // A level's blocks in host order, slice after slice (level 0: the base).
  bool LoadLevel(const TextureFetch& t, const TextureFormatInfo& info, uint32_t level, std::vector<uint8_t>& blocks,
                 std::string& error) const;
  bool LoadBlocks(const uint32_t fetch[6], std::vector<uint8_t>& blocks, std::string& error) const;

  struct Texture {
    Image image;  // the view member is unused: views are per swizzle
    uint64_t content_hash = 0;
    uint64_t seed = 0;            // the fetch constant's part of the hash
    uint64_t validated = 0;       // the epoch its content was last hashed in
    uint32_t begin = 0, end = 0;  // the guest memory its levels span
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
  uint64_t epoch_ = 1;
  std::function<void()> before_upload_;
};

}  // namespace any360::gfx::vk
