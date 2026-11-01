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
  // How each 32-bit texel is converted for the host format (kNone: copied as it is).
  enum Convert : uint32_t { kNone, kDepthUnorm24, kDepthFloat24 } convert = kNone;
};

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
  static TextureFetch Parse(const uint32_t dwords[6]);
};

class TextureCache {
 public:
  TextureCache(Device& device, const uint8_t* physical, uint32_t mirror_bytes);
  ~TextureCache();

  // A 2D-array view of the texture `fetch` describes, with its swizzle, uploaded and ready for sampling; null with
  // `error` when it is not handled yet.
  VkImageView View(const uint32_t fetch[6], std::string& error);
  // The sampler for `fetch` with a fetch instruction's filters (3 / 7: take the fetch constant's).
  VkSampler Sampler(const uint32_t fetch[6], uint32_t mag, uint32_t min, uint32_t mip, uint32_t aniso);

  // The texture `fetch` describes as RGBA8 pixels, swizzled as the guest asks (a front buffer to present; k_8_8_8_8
  // only for now).
  bool ReadRGBA8(const uint32_t fetch[6], uint32_t& width, uint32_t& height, std::vector<uint32_t>& pixels,
                 std::string& error) const;

  uint32_t uploads() const { return uploads_; }
  // Called before an upload replaces a texture's content (the renderer runs its pending draws first).
  void SetBeforeUpload(std::function<void()> callback) { before_upload_ = std::move(callback); }

 private:
  // The guest bytes the base level spans, and its blocks endian-swapped and untiled into rows.
  // The guest memory a texture spans (all its stacked slices), and the distance between slices: the slice's blocks
  // with rows and columns aligned to 32 blocks, aligned to 4 KB (as the reference lays them out).
  static uint32_t GuestExtent(const TextureFetch& t, const TextureFormatInfo& info);
  static uint32_t SliceStride(const TextureFetch& t, const TextureFormatInfo& info);
  // The texture's blocks in host order, slice after slice.
  bool LoadBlocks(const uint32_t fetch[6], std::vector<uint8_t>& blocks, std::string& error) const;

  struct Texture {
    Image image;  // the view member is unused: views are per swizzle
    uint64_t content_hash = 0;
    std::map<uint32_t, VkImageView> views;  // by host swizzle
  };

  Device& device_;
  const uint8_t* physical_;
  uint32_t mirror_bytes_;
  std::map<std::array<uint32_t, 4>, std::unique_ptr<Texture>> textures_;
  std::map<std::array<uint32_t, 8>, VkSampler> samplers_;
  std::vector<uint8_t> scratch_;
  uint32_t uploads_ = 0;
  std::function<void()> before_upload_;
};

}  // namespace any360::gfx::vk
