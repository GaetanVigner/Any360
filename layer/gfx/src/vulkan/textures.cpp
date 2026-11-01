#include "any360/gfx/vulkan/textures.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>

namespace any360::gfx::vk {

namespace {
// Xenos swizzle codes.
constexpr uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 3 | z << 6 | w << 9; }
constexpr uint32_t kRGBA = Swizzle(0, 1, 2, 3), kRRRR = Swizzle(0, 0, 0, 0), kRGGG = Swizzle(0, 1, 1, 1),
                   kRGBB = Swizzle(0, 1, 2, 2);

uint32_t Swap(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);  // 8 in 16
    case 2: return std::byteswap(v);                                     // 8 in 32
    case 3: return (v >> 16) | (v << 16);                                // 16 in 32
    default: return v;
  }
}

VkComponentSwizzle Component(uint32_t code) {
  switch (code) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

VkSamplerAddressMode AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // halfway: edge until the halfway rule is done
    case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;  // the mirror-once modes (core only from Vulkan 1.2 features)
  }
}

uint64_t Fnv1a(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
  return hash;
}
}  // namespace

const TextureFormatInfo* GuestTextureFormat(uint32_t xenos_format) {
  // As the reference maps them (VulkanTextureCache::kBestHostFormats), for the formats a host image holds unchanged.
  static const TextureFormatInfo k8 = {VK_FORMAT_R8_UNORM, 1, 1, kRRRR};
  static const TextureFormatInfo k8_8 = {VK_FORMAT_R8G8_UNORM, 2, 1, kRGGG};
  static const TextureFormatInfo k8_8_8_8 = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, kRGBA};
  static const TextureFormatInfo k5_6_5 = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, kRGBB, TextureFormatInfo::kSwapRedBlue565};
  static const TextureFormatInfo k2_10_10_10 = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, kRGBA};
  static const TextureFormatInfo kDxt1 = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, kRGBA};
  static const TextureFormatInfo kDxt3 = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, kRGBA};
  static const TextureFormatInfo kDxt5 = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, kRGBA};
  static const TextureFormatInfo k16Float = {VK_FORMAT_R16_SFLOAT, 2, 1, kRRRR};
  static const TextureFormatInfo k16_16Float = {VK_FORMAT_R16G16_SFLOAT, 4, 1, kRGGG};
  static const TextureFormatInfo k16_16_16_16Float = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, kRGBA};
  static const TextureFormatInfo k32Float = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR};
  static const TextureFormatInfo k32_32Float = {VK_FORMAT_R32G32_SFLOAT, 8, 1, kRGGG};
  static const TextureFormatInfo k32_32_32_32Float = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, kRGBA};
  // Depth as the resolves write it (depth << 8 | stencil): sampled as float depth (VK_FORMAT_R32_SFLOAT, RRRR).
  static const TextureFormatInfo k24_8 = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR, TextureFormatInfo::kDepthUnorm24};
  static const TextureFormatInfo k24_8Float = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR, TextureFormatInfo::kDepthFloat24};
  switch (xenos_format) {
    case 2: case 8: return &k8;  // k_8, k_8_A
    case 10: return &k8_8;
    case 4: return &k5_6_5;
    case 6: return &k8_8_8_8;
    case 7: return &k2_10_10_10;
    case 18: return &kDxt1;
    case 19: return &kDxt3;
    case 20: return &kDxt5;
    case 22: return &k24_8;
    case 23: return &k24_8Float;
    case 30: return &k16Float;
    case 31: return &k16_16Float;
    case 32: return &k16_16_16_16Float;
    case 36: return &k32Float;
    case 37: return &k32_32Float;
    case 38: return &k32_32_32_32Float;
    default: return nullptr;
  }
}

float Float20e4ToFloat(uint32_t f24) {
  const uint32_t mantissa = f24 & 0xFFFFF, exponent = (f24 >> 20) & 0xF;
  return exponent ? std::ldexp(1.0f + float(mantissa) / float(1u << 20), int(exponent) - 15)
                  : std::ldexp(float(mantissa) / float(1u << 20), -14);
}

uint32_t FloatTo20e4(float value) {
  uint32_t f32 = std::bit_cast<uint32_t>(value);
  if (int32_t(f32) <= 0 || f32 > 0x7F800000u) return 0;  // negative, zero, NaN
  f32 = std::min(f32, 0x3FFFFFF8u);  // the largest float24 (just under 2)
  if (f32 < 0x38800000u) {
    // Below 2^-14: a denormal float24.
    const uint32_t shift = std::min(113u - (f32 >> 23), 24u);
    f32 = (0x800000u | (f32 & 0x7FFFFFu)) >> shift;
  } else {
    f32 += 0xC8000000u;  // the exponent bias from 127 to 15
  }
  f32 += 3 + ((f32 >> 3) & 1);  // to nearest even at the 3 bits dropped
  return (f32 >> 3) & 0xFFFFFF;
}

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  // As the reference computes it (texture_util::GetTiledOffset2D, after XGRAPHICS' tiling).
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) +
         (offset & 0x3F);
}

TextureFetch TextureFetch::Parse(const uint32_t d[6]) {
  TextureFetch f;
  f.type = d[0] & 3;
  f.signs = (d[0] >> 2) & 0xFF;
  f.clamp_x = (d[0] >> 10) & 7;
  f.clamp_y = (d[0] >> 13) & 7;
  f.clamp_z = (d[0] >> 16) & 7;
  f.pitch = ((d[0] >> 22) & 0x1FF) * 32;  // texels
  f.tiled = d[0] >> 31;
  f.format = d[1] & 0x3F;
  f.endian = (d[1] >> 6) & 3;
  f.base = d[1] & 0xFFFFF000;
  f.dimension = (d[5] >> 9) & 3;
  if (f.dimension == 2) {  // 3D
    f.width = (d[2] & 0x7FF) + 1, f.height = ((d[2] >> 11) & 0x7FF) + 1, f.depth = (d[2] >> 22) + 1;
  } else if (f.dimension == 0) {  // 1D
    f.width = (d[2] & 0xFFFFFF) + 1, f.height = 1, f.depth = 1;
  } else {
    f.width = (d[2] & 0x1FFF) + 1, f.height = ((d[2] >> 13) & 0x1FFF) + 1, f.depth = (d[2] >> 26) + 1;
  }
  if (f.dimension == 3) f.depth = 6;  // cube: six faces, laid out as stacked slices
  f.swizzle = (d[3] >> 1) & 0xFFF;
  f.mag_filter = (d[3] >> 19) & 3;
  f.min_filter = (d[3] >> 21) & 3;
  f.mip_filter = (d[3] >> 23) & 3;
  f.aniso = (d[3] >> 25) & 7;
  f.border_color = d[5] & 3;
  f.packed_mips = (d[5] >> 11) & 1;
  f.mip_min = (d[4] >> 2) & 0xF;
  f.mip_max = (d[4] >> 6) & 0xF;
  f.mip_address = d[5] & 0xFFFFF000;
  return f;
}

namespace {

uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
uint32_t Log2Floor(uint32_t v) { return 31 - uint32_t(std::countl_zero(v | 1)); }

// The first level stored in the packed mip tail: the one whose shorter side is 16 texels or less.
uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  const uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Where a packed level lies in the mip tail's 32x32 tile, in blocks (texture_util::GetPackedMipOffset for 2D).
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t block_size, uint32_t level, uint32_t& x, uint32_t& y) {
  const uint32_t log2_width = Log2Ceil(width), log2_height = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  const uint32_t tail_base = log2_size > 4 ? log2_size - 4 : 0, packed = level - tail_base;
  if (packed < 3) {
    x = log2_width > log2_height ? 0 : 16u >> packed;
    y = log2_width > log2_height ? 16u >> packed : 0;
  } else if (log2_width > log2_height) {
    x = (1u << (log2_width - tail_base)) >> (packed - 2), y = 0;
  } else {
    x = 0, y = (1u << (log2_height - tail_base)) >> (packed - 2);
  }
  x /= block_size, y /= block_size;
}

// Where a level's texels are in guest memory (texture_util::GetGuestTextureLayout): the base level at the base
// address with the fetch constant's pitch; levels 1 and up one after the other at the mip address, each sized from
// the base rounded to powers of two; rows and columns aligned to 32 blocks, slices to 4 KB; the levels whose shorter
// side is 16 texels or less packed into the tail level's tile at their own offsets.
struct Placement {
  uint32_t address = 0, pitch_blocks = 0, row_pitch = 0, slice_stride = 0, x = 0, y = 0;
};

Placement Place(const TextureFetch& t, const TextureFormatInfo& info, uint32_t level) {
  const uint32_t bs = info.block_size, bpb = info.block_bytes;
  const uint32_t packed_level = t.packed_mips ? PackedMipLevel(t.width, t.height) : UINT32_MAX;
  auto layout = [&](Placement& p, uint32_t stored, bool base) {
    const uint32_t row_texels = base ? std::max(t.pitch, t.width) : std::max((1u << Log2Ceil(t.width)) >> stored, 1u);
    const uint32_t rows = base ? t.height : std::max((1u << Log2Ceil(t.height)) >> stored, 1u);
    p.pitch_blocks = (row_texels + bs - 1) / bs;
    p.row_pitch = t.tiled ? ((p.pitch_blocks + 31) & ~31u) * bpb : (p.pitch_blocks * bpb + 255) & ~255u;
    const uint32_t stride_rows = ((rows + bs - 1) / bs + 31) & ~31u;
    p.slice_stride = (((p.pitch_blocks + 31) & ~31u) * bpb * stride_rows + 4095) & ~4095u;
    if (!t.tiled) p.slice_stride = std::max(p.slice_stride, (p.row_pitch * stride_rows + 4095) & ~4095u);
  };
  Placement p;
  if (level == 0) {
    p.address = t.base;
    layout(p, 0, true);
    if (packed_level == 0) PackedMipOffset(t.width, t.height, bs, 0, p.x, p.y);
    return p;
  }
  p.address = t.mip_address;
  const uint32_t stored = std::min(level, packed_level);
  if (stored > 0) {
    for (uint32_t i = 1; i < stored; ++i) {
      Placement before;
      layout(before, i, false);
      p.address += before.slice_stride * t.slices();
    }
  }
  layout(p, stored, false);
  if (level >= packed_level) PackedMipOffset(t.width, t.height, bs, level, p.x, p.y);
  return p;
}

}  // namespace

void TextureFetch::Levels(uint32_t& first, uint32_t& last) const {
  const uint32_t size_levels = Log2Floor(std::max(width, height));
  first = 0, last = 0;
  if (mip_address) {
    first = std::min(mip_min, size_levels);
    last = std::max(std::min(mip_max, size_levels), first);
  }
  if (last && !base) first = std::max(first, 1u);
}

bool TextureCache::LoadLevel(const TextureFetch& t, const TextureFormatInfo& info_ref, uint32_t level,
                             std::vector<uint8_t>& blocks, std::string& error) const {
  const TextureFormatInfo* info = &info_ref;
  const Placement p = Place(t, *info, level);
  const uint32_t bs = info->block_size, bpb = info->block_bytes, bpb_log2 = uint32_t(std::countr_zero(bpb));
  const uint32_t bw = (std::max(t.width >> level, 1u) + bs - 1) / bs, bh = (std::max(t.height >> level, 1u) + bs - 1) / bs;
  const uint32_t slices = t.slices();
  const size_t slice_bytes = size_t(bw) * bh * bpb;
  blocks.assign(slice_bytes * slices, 0);
  if (!p.address) return error = "texture level " + std::to_string(level) + " without an address", false;
  // Block by block: the 32-bit words holding it, endian-swapped as the GPU reads memory.
  auto read = [&](uint32_t address, uint8_t* out) {
    if (bpb >= 4) {
      if (uint64_t(address) + bpb > mirror_bytes_) return;
      std::memcpy(out, physical_ + address, bpb);
      for (uint32_t i = 0; i < bpb; i += 4) {
        uint32_t v;
        std::memcpy(&v, out + i, 4);
        v = Swap(v, t.endian);
        std::memcpy(out + i, &v, 4);
      }
    } else {
      const uint32_t word_address = address & ~3u;
      if (uint64_t(word_address) + 4 > mirror_bytes_) return;
      uint32_t v;
      std::memcpy(&v, physical_ + word_address, 4);
      v = Swap(v, t.endian);
      std::memcpy(out, reinterpret_cast<const uint8_t*>(&v) + (address & 3), bpb);
    }
  };
  for (uint32_t s = 0; s < slices; ++s) {
    for (uint32_t y = 0; y < bh; ++y) {
      for (uint32_t x = 0; x < bw; ++x) {
        const uint32_t offset = t.tiled ? uint32_t(TiledOffset2D(int32_t(x + p.x), int32_t(y + p.y), p.pitch_blocks, bpb_log2))
                                        : (y + p.y) * p.row_pitch + (x + p.x) * bpb;
        read(p.address + s * p.slice_stride + offset, &blocks[s * slice_bytes + (size_t(y) * bw + x) * bpb]);
      }
    }
  }
  if (info->convert == TextureFormatInfo::kSwapRedBlue565) {
    for (size_t i = 0; i + 2 <= blocks.size(); i += 2) {
      uint16_t v;
      std::memcpy(&v, &blocks[i], 2);
      v = uint16_t((v & 0x07E0) | (v & 0x1F) << 11 | v >> 11);
      std::memcpy(&blocks[i], &v, 2);
    }
  } else if (info->convert != TextureFormatInfo::kNone) {
    for (size_t i = 0; i + 4 <= blocks.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &blocks[i], 4);
      const float depth = info->convert == TextureFormatInfo::kDepthFloat24 ? Float20e4ToFloat(v >> 8)
                                                                           : float(v >> 8) / float(0xFFFFFF);
      std::memcpy(&blocks[i], &depth, 4);
    }
  }
  return true;
}

bool TextureCache::LoadBlocks(const uint32_t fetch[6], std::vector<uint8_t>& blocks, std::string& error) const {
  const TextureFetch t = TextureFetch::Parse(fetch);
  const TextureFormatInfo* info = GuestTextureFormat(t.format);
  if (!info || (t.dimension != 1 && t.dimension != 3)) return error = "texture format or dimension not handled yet", false;
  return LoadLevel(t, *info, 0, blocks, error);
}

bool TextureCache::ReadRGBA8(const uint32_t fetch[6], uint32_t& width, uint32_t& height, std::vector<uint32_t>& pixels,
                             std::string& error) const {
  const TextureFetch t = TextureFetch::Parse(fetch);
  if (t.type != 2 || t.format != 6) return error = "front buffer not a k_8_8_8_8 texture (not yet)", false;
  std::vector<uint8_t> blocks;
  if (!LoadBlocks(fetch, blocks, error)) return false;
  width = t.width, height = t.height;
  pixels.resize(size_t(width) * height);
  // The guest swizzle over the texel's components (codes 4 and 5: zero and one; 6 and 7 read as 4 and 5).
  for (size_t i = 0; i < pixels.size(); ++i) {
    const uint8_t* c = &blocks[i * 4];
    uint32_t out = 0;
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t code = (t.swizzle >> (3 * k)) & 7;
      const uint32_t value = code < 4 ? c[code] : (code & 1) ? 0xFF : 0;
      out |= value << (8 * k);
    }
    pixels[i] = out;
  }
  return true;
}

TextureCache::TextureCache(Device& device, const uint8_t* physical, uint32_t mirror_bytes)
    : device_(device), physical_(physical), mirror_bytes_(mirror_bytes) {}

TextureCache::~TextureCache() {
  const Functions& f = device_.fn();
  VkDevice d = device_.device();
  if (!d) return;
  f.vkDeviceWaitIdle(d);
  for (auto& [key, t] : textures_) {
    for (auto& [swizzle, view] : t->views) f.vkDestroyImageView(d, view, nullptr);
    DestroyImage(device_, t->image);
  }
  for (auto& [key, sampler] : samplers_) f.vkDestroySampler(d, sampler, nullptr);
  for (Image& image : empty_) DestroyImage(device_, image);
}

VkImageView TextureCache::Empty(ViewType type, std::string& error) {
  Image& image = empty_[int(type)];
  if (image.view) return image.view;
  const Functions& f = device_.fn();
  const uint32_t layers = type == ViewType::kCube ? 6 : 1;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = type == ViewType::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ci.imageType = type == ViewType::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {1, 1, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (f.vkCreateImage(device_.device(), &ci, nullptr, &image.image) != VK_SUCCESS) return error = "empty texture", VK_NULL_HANDLE;
  VkMemoryRequirements req;
  f.vkGetImageMemoryRequirements(device_.device(), image.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = device_.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (alloc.memoryTypeIndex == UINT32_MAX || f.vkAllocateMemory(device_.device(), &alloc, nullptr, &image.memory) != VK_SUCCESS) {
    return error = "empty texture memory", VK_NULL_HANDLE;
  }
  f.vkBindImageMemory(device_.device(), image.image, image.memory, 0);
  image.format = ci.format;
  image.width = image.height = 1;
  const bool ok = device_.Submit([&](VkCommandBuffer cb) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    f.vkCmdPipelineBarrier2(cb, &dependency);
    const VkClearColorValue black{};
    f.vkCmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &barrier.subresourceRange);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    f.vkCmdPipelineBarrier2(cb, &dependency);
  });
  if (!ok) return error = "empty texture upload", VK_NULL_HANDLE;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image.image;
  vi.viewType = type == ViewType::kCube ? VK_IMAGE_VIEW_TYPE_CUBE
                : type == ViewType::k3D ? VK_IMAGE_VIEW_TYPE_3D
                                        : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = image.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  if (f.vkCreateImageView(device_.device(), &vi, nullptr, &image.view) != VK_SUCCESS) {
    image.view = VK_NULL_HANDLE;
    return error = "empty texture view", VK_NULL_HANDLE;
  }
  return image.view;
}

VkImageView TextureCache::View(const uint32_t fetch[6], ViewType type, std::string& error) {
  const TextureFetch t = TextureFetch::Parse(fetch);
  if (t.type != 2) return error = "fetch constant is not a texture", VK_NULL_HANDLE;
  // The data dimension (fetch constant word 5): 0 1D, 1 2D or stacked, 2 3D, 3 cube.
  const ViewType matching = t.dimension == 3 ? ViewType::kCube : t.dimension == 2 ? ViewType::k3D : ViewType::k2DArray;
  if (type != matching) return Empty(type, error);
  if (t.dimension == 0 || t.dimension == 2) {
    return error = std::string(t.dimension ? "3D" : "1D") + " textures (not yet)", VK_NULL_HANDLE;
  }
  const bool cube = t.dimension == 3;
  const TextureFormatInfo* info = GuestTextureFormat(t.format);
  if (!info) return error = "texture format " + std::to_string(t.format) + " not handled yet", VK_NULL_HANDLE;

  // The levels, and a hash of the guest memory each spans (a level's slices at its stride, the packed tail's tile).
  uint32_t first_level, last_level;
  t.Levels(first_level, last_level);
  uint64_t hash = (uint64_t(fetch[0]) << 32 | fetch[1]) ^ (uint64_t(fetch[2]) << 7) ^ (uint64_t(fetch[4]) << 21) ^ fetch[5];
  for (uint32_t level = first_level; level <= last_level; ++level) {
    const Placement p = Place(t, *info, level);
    const uint64_t bytes = uint64_t(p.slice_stride) * t.slices();
    if (!p.address || p.address + bytes > mirror_bytes_) return error = "texture outside the mirrored memory", VK_NULL_HANDLE;
    hash = hash * 0x100000001B3ull ^ Fnv1a(physical_ + p.address, size_t(bytes));
  }
  const uint32_t levels = last_level + 1;

  const std::array<uint32_t, 5> key = {fetch[0] & 0xFFC00003, fetch[1], fetch[2], fetch[5] & 0xFFFFFE00, fetch[4] & 0x3FC};
  auto& slot = textures_[key];
  const Functions& f = device_.fn();
  if (!slot) {
    slot = std::make_unique<Texture>();
    VkImageCreateInfo info_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info_ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info_ci.imageType = VK_IMAGE_TYPE_2D;
    info_ci.format = info->format;
    info_ci.extent = {t.width, t.height, 1};
    info_ci.mipLevels = levels;
    info_ci.arrayLayers = t.depth;  // stacked textures: one layer a slice; cubes: a face
    info_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    info_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    info_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (f.vkCreateImage(device_.device(), &info_ci, nullptr, &slot->image.image) != VK_SUCCESS) {
      return error = "texture image", VK_NULL_HANDLE;
    }
    VkMemoryRequirements req;
    f.vkGetImageMemoryRequirements(device_.device(), slot->image.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device_.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || f.vkAllocateMemory(device_.device(), &alloc, nullptr, &slot->image.memory) != VK_SUCCESS) {
      return error = "texture memory", VK_NULL_HANDLE;
    }
    f.vkBindImageMemory(device_.device(), slot->image.image, slot->image.memory, 0);
    slot->image.format = info->format;
    slot->image.width = t.width;
    slot->image.height = t.height;
  }

  if (slot->content_hash != hash) {
    if (before_upload_) before_upload_();  // draws recorded earlier must sample the old content
    // Every level's blocks, one after the other (levels below the first are never sampled: left as they are).
    std::vector<uint8_t> blocks;
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t level = first_level; level <= last_level; ++level) {
      std::vector<uint8_t> level_blocks;
      if (!LoadLevel(t, *info, level, level_blocks, error)) return VK_NULL_HANDLE;
      VkBufferImageCopy region{};
      region.bufferOffset = blocks.size();
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t.depth};
      region.imageExtent = {std::max(t.width >> level, 1u), std::max(t.height >> level, 1u), 1};
      regions.push_back(region);
      blocks.insert(blocks.end(), level_blocks.begin(), level_blocks.end());
      blocks.resize((blocks.size() + 15) & ~size_t(15));  // offsets aligned for any texel size
    }
    Buffer staging;
    if (!CreateBuffer(device_, blocks.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) {
      return error = "texture staging", VK_NULL_HANDLE;
    }
    std::memcpy(staging.mapped, blocks.data(), blocks.size());
    Image& image = slot->image;
    const bool ok = device_.Submit([&](VkCommandBuffer cb) {
      VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, t.depth};
      VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dependency.imageMemoryBarrierCount = 1;
      dependency.pImageMemoryBarriers = &barrier;
      f.vkCmdPipelineBarrier2(cb, &dependency);
      f.vkCmdCopyBufferToImage(cb, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                               regions.data());
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      f.vkCmdPipelineBarrier2(cb, &dependency);
    });
    DestroyBuffer(device_, staging);
    if (!ok) return error = "texture upload", VK_NULL_HANDLE;
    slot->content_hash = hash;
    ++uploads_;
  }

  // The view: the guest swizzle over the format's own (codes 6 and 7 read as 0 and 1, as the reference does).
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t guest = (t.swizzle >> (3 * i)) & 7;
    host_swizzle |= (guest >= 4 ? (guest & 5) : (info->swizzle >> (3 * guest)) & 7) << (3 * i);
  }
  VkImageView& view = slot->views[host_swizzle];
  if (!view) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = slot->image.image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;  // 2D textures are declared arrayed
    vi.format = info->format;
    vi.components = {Component(host_swizzle & 7), Component((host_swizzle >> 3) & 7), Component((host_swizzle >> 6) & 7),
                     Component((host_swizzle >> 9) & 7)};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, t.depth};
    if (f.vkCreateImageView(device_.device(), &vi, nullptr, &view) != VK_SUCCESS) {
      view = VK_NULL_HANDLE;
      return error = "texture view", VK_NULL_HANDLE;
    }
  }
  return view;
}

VkSampler TextureCache::Sampler(const uint32_t fetch[6], uint32_t mag, uint32_t min, uint32_t mip, uint32_t aniso) {
  const TextureFetch t = TextureFetch::Parse(fetch);
  if (mag == 3) mag = t.mag_filter;
  if (min == 3) min = t.min_filter;
  if (mip == 3) mip = t.mip_filter;
  if (aniso == 7) aniso = t.aniso;
  uint32_t first_level, last_level;
  t.Levels(first_level, last_level);
  const std::array<uint32_t, 10> key = {mag, min, mip, aniso, t.clamp_x, t.clamp_y, t.clamp_z, t.border_color, first_level,
                                        last_level};
  VkSampler& sampler = samplers_[key];
  if (sampler) return sampler;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.minFilter = min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = AddressMode(t.clamp_x);
  si.addressModeV = AddressMode(t.clamp_y);
  si.addressModeW = AddressMode(t.clamp_z);
  if (aniso >= 2 && aniso <= 5) {
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = std::min(float(1u << (aniso - 1)), device_.properties().limits.maxSamplerAnisotropy);
  }
  // The fetch constant's level range; the base map filter (2) samples its first level only.
  si.minLod = float(first_level);
  si.maxLod = mip == 2 ? float(first_level) : float(last_level);
  si.borderColor = t.border_color == 0   ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : t.border_color == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  if (device_.fn().vkCreateSampler(device_.device(), &si, nullptr, &sampler) != VK_SUCCESS) sampler = VK_NULL_HANDLE;
  return sampler;
}

}  // namespace any360::gfx::vk
