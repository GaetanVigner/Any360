#include "any360/gfx/vulkan/textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <bit>
#include <cstring>

#include <immintrin.h>

namespace any360::gfx::vk {

namespace {
// Xenos swizzle codes.
constexpr uint32_t Swizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 3 | z << 6 | w << 9; }
constexpr uint32_t kRGBA = Swizzle(0, 1, 2, 3), kRRRR = Swizzle(0, 0, 0, 0), kRGGG = Swizzle(0, 1, 1, 1),
                   kRGBB = Swizzle(0, 1, 2, 2);

// Byte orders as 16-byte shuffles (each 32-bit word): none, 8 in 16, 8 in 32, 16 in 32.
alignas(16) constexpr uint8_t kWordShuffles[4][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
    {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12}, {2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13}};
// SSSE3's byte shuffle (every x86-64 CPU this runs on has it; the build targets the baseline).
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("ssse3")))
#endif
__m128i ShuffleBytes(__m128i v, __m128i mask) {
  return _mm_shuffle_epi8(v, mask);
}

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

// A content hash of guest memory, 32 bytes a step over four independent lanes (a texture is hashed whenever it may
// have changed: byte-wise hashing cost milliseconds a texture).
uint64_t HashMemory(const uint8_t* data, size_t size) {
  constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
  uint64_t lanes[4] = {k, k * 3, k * 5, k * 7};
  size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    for (int lane = 0; lane < 4; ++lane) {
      uint64_t v;
      std::memcpy(&v, data + i + 8 * lane, 8);
      lanes[lane] = (lanes[lane] ^ v) * 0xFF51AFD7ED558CCDull;
      lanes[lane] ^= lanes[lane] >> 29;
    }
  }
  uint64_t hash = size;
  for (uint64_t lane : lanes) hash = (hash ^ lane) * 0xC4CEB9FE1A85EC53ull;
  for (; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
  return hash ^ (hash >> 33);
}
}  // namespace

const TextureFormatInfo* GuestTextureFormat(uint32_t xenos_format) {
  // As the reference maps them (VulkanTextureCache::kBestHostFormats), for the formats a host image holds unchanged.
  static const TextureFormatInfo k8 = {VK_FORMAT_R8_UNORM, 1, 1, kRRRR, TextureFormatInfo::kNone, VK_FORMAT_R8_SNORM};
  static const TextureFormatInfo k8_8 = {VK_FORMAT_R8G8_UNORM, 2, 1, kRGGG, TextureFormatInfo::kNone, VK_FORMAT_R8G8_SNORM};
  static const TextureFormatInfo k8_8_8_8 = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, kRGBA, TextureFormatInfo::kNone,
                                             VK_FORMAT_R8G8B8A8_SNORM};
  static const TextureFormatInfo k5_6_5 = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, kRGBB, TextureFormatInfo::kSwapRedBlue565};
  static const TextureFormatInfo k2_10_10_10 = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, kRGBA};
  // 1:5:5:5 (red in the low bits, alpha the top bit) as A1R5G5B5 with red and blue exchanged; 4:4:4:4 (red in the
  // low nibble) as A4B4G4R4, the same bits.
  static const TextureFormatInfo k1_5_5_5 = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, kRGBA, TextureFormatInfo::kSwapRedBlue1555};
  static const TextureFormatInfo k4_4_4_4 = {VK_FORMAT_A4B4G4R4_UNORM_PACK16, 2, 1, kRGBA};
  // 10:11:11 (red 11 bits, green 11, blue 10, from the low bits) and 11:11:10 (10, 11, 11) as 16-bit normalized
  // RGBA, as the reference converts them.
  // As the reference converts them: 6:5:5 (x 5 bits, y 5, z 6 from the low bits) into 5:6:5 with a red, blue,
  // green swizzle; DXT3A (a DXT3 alpha block) to 8-bit texels; CTX1 (two 8:8 end points and 2-bit indices) to 8:8
  // texels; the EXPAND formats' bits as half floats.
  static const TextureFormatInfo k6_5_5 = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, Swizzle(0, 2, 1, 1), TextureFormatInfo::kRotate655};
  static const TextureFormatInfo kDxt3a = {VK_FORMAT_R8_UNORM, 8, 4, kRRRR, TextureFormatInfo::kDecodeDxt3a};
  static const TextureFormatInfo kCtx1 = {VK_FORMAT_R8G8_UNORM, 8, 4, kRGGG, TextureFormatInfo::kDecodeCtx1};
  // 4:2:2 YUV, two texels a 32-bit block sharing their chroma: Cr_Y1_Cb_Y0 (bytes Y0, Cb, Y1, Cr, as Vulkan's
  // G8B8G8R8_422) and Y1_Cr_Y0_Cb (Cb, Y0, Cr, Y1, as B8G8R8G8_422), decoded to RGBA8 texels (Cr, Y, Cb, 1), as the
  // reference does where the 4:2:2 formats cannot be sampled.
  static const TextureFormatInfo kGbgr = {VK_FORMAT_R8G8B8A8_UNORM, 4, 2, kRGBB, TextureFormatInfo::kDecodeGbgr,
                                          VK_FORMAT_UNDEFINED, 1};
  static const TextureFormatInfo kBgrg = {VK_FORMAT_R8G8B8A8_UNORM, 4, 2, kRGBB, TextureFormatInfo::kDecodeBgrg,
                                          VK_FORMAT_UNDEFINED, 1};
  static const TextureFormatInfo k10_11_11 = {VK_FORMAT_R16G16B16A16_UNORM, 4, 1, kRGBA, TextureFormatInfo::kUnpack10_11_11};
  static const TextureFormatInfo k11_11_10 = {VK_FORMAT_R16G16B16A16_UNORM, 4, 1, kRGBA, TextureFormatInfo::kUnpack11_11_10};
  // 16-bit normalized components (textures read them as 0...1 or -1...1, unlike render targets' -32...32).
  static const TextureFormatInfo k16 = {VK_FORMAT_R16_UNORM, 2, 1, kRRRR, TextureFormatInfo::kNone, VK_FORMAT_R16_SNORM};
  static const TextureFormatInfo k16_16 = {VK_FORMAT_R16G16_UNORM, 4, 1, kRGGG, TextureFormatInfo::kNone,
                                           VK_FORMAT_R16G16_SNORM};
  static const TextureFormatInfo k16_16_16_16 = {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, kRGBA, TextureFormatInfo::kNone,
                                                 VK_FORMAT_R16G16B16A16_SNORM};
  // DXN: two DXT5 alpha blocks (red, green), as BC5; DXT5A: one, as BC4.
  static const TextureFormatInfo kDxn = {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, kRGGG, TextureFormatInfo::kNone,
                                         VK_FORMAT_BC5_SNORM_BLOCK};
  static const TextureFormatInfo kDxt5a = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, kRRRR, TextureFormatInfo::kNone,
                                           VK_FORMAT_BC4_SNORM_BLOCK};
  static const TextureFormatInfo kDxt1 = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, kRGBA};
  static const TextureFormatInfo kDxt3 = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, kRGBA};
  static const TextureFormatInfo kDxt5 = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, kRGBA};
  static const TextureFormatInfo k16Float = {VK_FORMAT_R16_SFLOAT, 2, 1, kRRRR, TextureFormatInfo::kNone, VK_FORMAT_R16_SFLOAT};
  static const TextureFormatInfo k16_16Float = {VK_FORMAT_R16G16_SFLOAT, 4, 1, kRGGG, TextureFormatInfo::kNone,
                                                VK_FORMAT_R16G16_SFLOAT};
  static const TextureFormatInfo k16_16_16_16Float = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, kRGBA, TextureFormatInfo::kNone,
                                                      VK_FORMAT_R16G16B16A16_SFLOAT};
  static const TextureFormatInfo k32Float = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR, TextureFormatInfo::kNone, VK_FORMAT_R32_SFLOAT};
  static const TextureFormatInfo k32_32Float = {VK_FORMAT_R32G32_SFLOAT, 8, 1, kRGGG, TextureFormatInfo::kNone,
                                                VK_FORMAT_R32G32_SFLOAT};
  static const TextureFormatInfo k32_32_32_32Float = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, kRGBA, TextureFormatInfo::kNone,
                                                      VK_FORMAT_R32G32B32A32_SFLOAT};
  // Depth as the resolves write it (depth << 8 | stencil): sampled as float depth (VK_FORMAT_R32_SFLOAT, RRRR).
  static const TextureFormatInfo k24_8 = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR, TextureFormatInfo::kDepthUnorm24};
  static const TextureFormatInfo k24_8Float = {VK_FORMAT_R32_SFLOAT, 4, 1, kRRRR, TextureFormatInfo::kDepthFloat24};
  switch (xenos_format) {
    // The _A and _AS_16_16_16_16 forms read the same bits (the latter only ask for more filtering precision), as
    // the reference maps them.
    case 2: case 8: return &k8;  // k_8, k_8_A
    case 3: return &k1_5_5_5;
    case 5: return &k6_5_5;
    case 10: return &k8_8;
    case 4: return &k5_6_5;
    case 6: case 50: return &k8_8_8_8;  // k_8_8_8_8, _AS_16_16_16_16
    case 7: case 54: return &k2_10_10_10;  // and k_2_10_10_10_AS_16_16_16_16 (only its filtering differs)
    case 15: return &k4_4_4_4;
    case 16: case 55: return &k10_11_11;  // and _AS_16_16_16_16
    case 17: case 56: return &k11_11_10;
    case 18: case 51: return &kDxt1;
    case 19: case 52: return &kDxt3;
    case 20: case 53: return &kDxt5;
    case 24: return &k16;
    case 25: return &k16_16;
    case 26: return &k16_16_16_16;
    case 27: return &k16Float;            // k_16_EXPAND
    case 28: return &k16_16Float;         // k_16_16_EXPAND
    case 29: return &k16_16_16_16Float;   // k_16_16_16_16_EXPAND
    case 58: return &kDxt3a;
    case 11: return &kGbgr;  // k_Cr_Y1_Cb_Y0_REP
    case 12: return &kBgrg;  // k_Y1_Cr_Y0_Cb_REP
    case 60: return &kCtx1;
    case 49: return &kDxn;
    case 59: return &kDxt5a;
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

int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t bytes_per_block_log2) {
  // As the reference computes it (texture_util::GetTiledOffset3D, from XGRAPHICS::TileVolume).
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
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
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t block_size, uint32_t block_rows, uint32_t level, uint32_t& x,
                     uint32_t& y) {
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
  x /= block_size, y /= block_rows;
}

// Where a level's texels are in guest memory (texture_util::GetGuestTextureLayout): the base level at the base
// address with the fetch constant's pitch; levels 1 and up one after the other at the mip address, each sized from
// the base rounded to powers of two; rows and columns aligned to 32 blocks, slices to 4 KB; the levels whose shorter
// side is 16 texels or less packed into the tail level's tile at their own offsets.
struct Placement {
  uint32_t address = 0, pitch_blocks = 0, row_pitch = 0, slice_stride = 0, x = 0, y = 0;
};

Placement Place(const TextureFetch& t, const TextureFormatInfo& info, uint32_t level) {
  const uint32_t bs = info.block_size, bsh = info.block_rows(), bpb = info.block_bytes;
  const uint32_t packed_level = t.packed_mips ? PackedMipLevel(t.width, t.height) : UINT32_MAX;
  auto layout = [&](Placement& p, uint32_t stored, bool base) {
    const uint32_t row_texels = base ? std::max(t.pitch, t.width) : std::max((1u << Log2Ceil(t.width)) >> stored, 1u);
    const uint32_t rows = base ? t.height : std::max((1u << Log2Ceil(t.height)) >> stored, 1u);
    p.pitch_blocks = (row_texels + bs - 1) / bs;
    p.row_pitch = t.tiled ? ((p.pitch_blocks + 31) & ~31u) * bpb : (p.pitch_blocks * bpb + 255) & ~255u;
    const uint32_t stride_rows = ((rows + bsh - 1) / bsh + 31) & ~31u;
    p.slice_stride = (((p.pitch_blocks + 31) & ~31u) * bpb * stride_rows + 4095) & ~4095u;
    if (!t.tiled) p.slice_stride = std::max(p.slice_stride, (p.row_pitch * stride_rows + 4095) & ~4095u);
    // A 3D texture's z slices are packed at the row pitch times the tile-aligned rows (the whole volume, its depth
    // aligned to 4, is what aligns to 4 KB); stacked layers and cube faces each align.
    if (t.dimension == 2) p.slice_stride = (t.tiled ? ((p.pitch_blocks + 31) & ~31u) * bpb : p.row_pitch) * stride_rows;
  };
  Placement p;
  if (level == 0) {
    p.address = t.base;
    layout(p, 0, true);
    if (packed_level == 0) PackedMipOffset(t.width, t.height, bs, bsh, 0, p.x, p.y);
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
  if (level >= packed_level) PackedMipOffset(t.width, t.height, bs, bsh, level, p.x, p.y);
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
  const uint32_t bs = info->block_size, bsh = info->block_rows(), bpb = info->block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(bpb));
  const uint32_t bw = (std::max(t.width >> level, 1u) + bs - 1) / bs, bh = (std::max(t.height >> level, 1u) + bsh - 1) / bsh;
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
  if (t.dimension != 2 && t.tiled && bpb >= 4 && t.endian <= 3) {
    // Tiled 2D levels of 4, 8 or 16-byte blocks: 16 bytes of a tiled row are contiguous from a block x aligned to their
    // count (as for resolves written back): one tiled offset and one byte shuffle for them.
    const uint32_t group = 16 / bpb;
    const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(kWordShuffles[t.endian]));
    for (uint32_t s = 0; s < slices; ++s) {
      for (uint32_t y = 0; y < bh; ++y) {
        uint8_t* row = &blocks[s * slice_bytes + size_t(y) * bw * bpb];
        for (uint32_t x = 0; x < bw;) {
          const uint32_t tx = x + p.x;
          const uint32_t address = p.address + s * p.slice_stride +
                                   uint32_t(TiledOffset2D(int32_t(tx), int32_t(y + p.y), p.pitch_blocks, bpb_log2));
          if (tx % group == 0 && x + group <= bw && uint64_t(address) + 16 <= mirror_bytes_) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(physical_ + address));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(row + size_t(x) * bpb), ShuffleBytes(v, mask));
            x += group;
            continue;
          }
          read(address, row + size_t(x) * bpb);
          ++x;
        }
      }
    }
  }
  for (uint32_t s = 0; s < slices && !(t.dimension != 2 && t.tiled && bpb >= 4 && t.endian <= 3); ++s) {
    for (uint32_t y = 0; y < bh; ++y) {
      for (uint32_t x = 0; x < bw; ++x) {
        uint32_t offset;
        if (t.dimension == 2 && t.tiled) {
          offset = uint32_t(TiledOffset3D(int32_t(x + p.x), int32_t(y + p.y), int32_t(s), p.pitch_blocks,
                                          (t.height + bsh - 1) / bsh, bpb_log2));
        } else {
          offset = s * p.slice_stride + (t.tiled ? uint32_t(TiledOffset2D(int32_t(x + p.x), int32_t(y + p.y), p.pitch_blocks, bpb_log2))
                                                 : (y + p.y) * p.row_pitch + (x + p.x) * bpb);
        }
        read(p.address + offset, &blocks[s * slice_bytes + (size_t(y) * bw + x) * bpb]);
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
  } else if (info->convert == TextureFormatInfo::kUnpack10_11_11 || info->convert == TextureFormatInfo::kUnpack11_11_10) {
    // Each component scaled to 16 bits, alpha one; the texels grow from 4 bytes to 8.
    const bool red11 = info->convert == TextureFormatInfo::kUnpack10_11_11;
    const uint32_t widths[3] = {red11 ? 11u : 10u, 11, red11 ? 10u : 11u};
    std::vector<uint8_t> packed;
    packed.swap(blocks);
    blocks.resize(packed.size() * 2);
    for (size_t i = 0; i + 4 <= packed.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &packed[i], 4);
      uint16_t out[4] = {0, 0, 0, 0xFFFF};
      for (uint32_t c = 0, shift = 0; c < 3; shift += widths[c++]) {
        const uint32_t max = (1u << widths[c]) - 1;
        out[c] = uint16_t((((v >> shift) & max) * 65535u + max / 2) / max);
      }
      std::memcpy(&blocks[2 * i], out, 8);
    }
  } else if (info->convert == TextureFormatInfo::kRotate655) {
    for (size_t i = 0; i + 2 <= blocks.size(); i += 2) {
      uint16_t v;
      std::memcpy(&v, &blocks[i], 2);
      v = uint16_t((v << 11 & 0xF800) | (v >> 5 & 0x07FF));
      std::memcpy(&blocks[i], &v, 2);
    }
  } else if (info->convert == TextureFormatInfo::kDecodeDxt3a || info->convert == TextureFormatInfo::kDecodeCtx1) {
    // Blocks to texels, rows of the level's width, as the reference decodes them.
    const bool ctx1 = info->convert == TextureFormatInfo::kDecodeCtx1;
    const uint32_t texel_bytes = ctx1 ? 2 : 1;
    const uint32_t w = std::max(t.width >> level, 1u), h = std::max(t.height >> level, 1u);
    std::vector<uint8_t> packed;
    packed.swap(blocks);
    blocks.assign(size_t(w) * h * texel_bytes * slices, 0);
    for (uint32_t s = 0; s < slices; ++s) {
      for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
          const uint8_t* block = &packed[s * slice_bytes + (size_t(by) * bw + bx) * 8];
          uint8_t* out = &blocks[size_t(s) * w * h * texel_bytes];
          if (ctx1) {
            // g0, r0, g1, r1, then 2 bits a texel; the middle colours a third and two thirds of the way.
            const uint8_t g0 = block[0], r0 = block[1], g1 = block[2], r1 = block[3];
            uint32_t indices;
            std::memcpy(&indices, block + 4, 4);
            const uint8_t cr[4] = {r0, r1, uint8_t(2.f / 3.f * r0 + 1.f / 3.f * r1), uint8_t(1.f / 3.f * r0 + 2.f / 3.f * r1)};
            const uint8_t cg[4] = {g0, g1, uint8_t(2.f / 3.f * g0 + 1.f / 3.f * g1), uint8_t(1.f / 3.f * g0 + 2.f / 3.f * g1)};
            for (uint32_t oy = 0; oy < 4 && by * 4 + oy < h; ++oy) {
              for (uint32_t ox = 0; ox < 4 && bx * 4 + ox < w; ++ox) {
                const uint32_t index = (indices >> (2 * (ox + 4 * oy))) & 3;
                uint8_t* texel = out + (size_t(by * 4 + oy) * w + bx * 4 + ox) * 2;
                texel[0] = cr[index], texel[1] = cg[index];
              }
            }
          } else {
            uint64_t alpha;
            std::memcpy(&alpha, block, 8);
            for (uint32_t oy = 0; oy < 4 && by * 4 + oy < h; ++oy) {
              for (uint32_t ox = 0; ox < 4 && bx * 4 + ox < w; ++ox) {
                out[size_t(by * 4 + oy) * w + bx * 4 + ox] = uint8_t(((alpha >> (4 * (ox + 4 * oy))) & 0xF) * 17);
              }
            }
          }
        }
      }
    }
  } else if (info->convert == TextureFormatInfo::kDecodeGbgr || info->convert == TextureFormatInfo::kDecodeBgrg) {
    // Each block's two texels: the luma of its own, the block's chroma.
    const bool gbgr = info->convert == TextureFormatInfo::kDecodeGbgr;
    const uint32_t w = std::max(t.width >> level, 1u), h = std::max(t.height >> level, 1u);
    std::vector<uint8_t> packed;
    packed.swap(blocks);
    blocks.assign(size_t(w) * h * 4 * slices, 0);
    for (uint32_t s = 0; s < slices; ++s) {
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* b = &packed[s * slice_bytes + (size_t(y) * bw + x / 2) * 4];
          const uint8_t luma = gbgr ? b[(x & 1) * 2] : b[1 + (x & 1) * 2];
          uint8_t* out = &blocks[((size_t(s) * h + y) * w + x) * 4];
          out[0] = gbgr ? b[3] : b[2], out[1] = luma, out[2] = gbgr ? b[1] : b[0], out[3] = 0xFF;
        }
      }
    }
  } else if (info->convert == TextureFormatInfo::kSwapRedBlue1555) {
    for (size_t i = 0; i + 2 <= blocks.size(); i += 2) {
      uint16_t v;
      std::memcpy(&v, &blocks[i], 2);
      v = uint16_t((v & 0x83E0) | (v & 0x1F) << 10 | (v >> 10 & 0x1F));
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
  for (Buffer& staging : pending_staging_) DestroyBuffer(device_, staging);
  pending_staging_.clear();
  for (Buffer& staging : free_staging_) DestroyBuffer(device_, staging);
  free_staging_.clear();
  for (auto& [key, t] : textures_) {
    if (!t) continue;
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

void TextureCache::Evict(uint64_t frames, std::vector<Image>& images, std::vector<VkImageView>& views) {
  bool any = false;
  for (auto it = textures_.begin(); it != textures_.end();) {
    Texture& t = *it->second;
    if (epoch_ - t.used <= frames) {
      ++it;
      continue;
    }
    for (auto& [swizzle, view] : t.views) views.push_back(view);
    images.push_back(t.image);
    it = textures_.erase(it);
    any = true;
  }
  if (any) {
    fast_.clear();
    last_used_ = nullptr;
    ++version_;
  }
}

VkImageView TextureCache::View(const uint32_t fetch[6], ViewType type, std::string& error, bool is_signed) {
  last_view_scaled_ = false;
  last_used_ = nullptr;
  if (any_changed_.load(std::memory_order_acquire)) {
    // Ranges reported from other threads (CPU writes watched, resolves written into guest memory).
    {
      std::lock_guard lock(changed_mutex_);
      changed_taken_.swap(changed_);
      any_changed_.store(false, std::memory_order_relaxed);
    }
    MemoryChanged(changed_taken_);  // a watched write is reported a page at a time: many ranges
    changed_taken_.clear();
  }
  const std::array<uint32_t, 8> fast_key = {fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], uint32_t(type),
                                            uint32_t(is_signed)};
  if (const auto hit = fast_.find(fast_key);
      hit != fast_.end() && (watch_ || hit->second.epoch == epoch_) && hit->second.version == version_) {
    last_view_scaled_ = hit->second.scaled;
    last_used_ = hit->second.used;
    *last_used_ = epoch_;
    return hit->second.view;
  }
  const TextureFetch t = TextureFetch::Parse(fetch);
  // Not a texture (a slot the title left unset): an empty texture, as the reference binds a null one, rather than
  // the draw dropped.
  if (t.type != 2) return Empty(type, error);
  // The data dimension (fetch constant word 5): 0 1D, 1 2D or stacked, 2 3D, 3 cube.
  const ViewType matching = t.dimension == 3 ? ViewType::kCube : t.dimension == 2 ? ViewType::k3D : ViewType::k2DArray;
  if (type != matching) return Empty(type, error);
  if (t.dimension == 0) return error = "1D textures (not yet)", VK_NULL_HANDLE;
  const bool cube = t.dimension == 3, volume = t.dimension == 2;
  // The layers of the host image (stacked slices, cube faces) and its depth (a 3D texture's z slices).
  const uint32_t layers = volume ? 1 : t.depth, image_depth = volume ? t.depth : 1;
  const TextureFormatInfo* info = GuestTextureFormat(t.format);
  if (!info) return error = "texture format " + std::to_string(t.format) + " not handled yet", VK_NULL_HANDLE;
  if (is_signed && info->signed_format == VK_FORMAT_UNDEFINED) {
    return error = "signed texture format " + std::to_string(t.format) + " (not yet)", VK_NULL_HANDLE;
  }
  // Signed views of the same bits need a mutable-format image.
  const bool mutable_format = info->signed_format != VK_FORMAT_UNDEFINED && info->signed_format != info->format;

  // The levels, and a hash of the guest memory each spans (a level's slices at its stride, the packed tail's tile).
  uint32_t first_level, last_level;
  t.Levels(first_level, last_level);
  if (volume && last_level) return error = "3D texture mip levels (not yet)", VK_NULL_HANDLE;
  uint32_t span_begin = UINT32_MAX, span_end = 0;
  for (uint32_t level = first_level; level <= last_level; ++level) {
    const Placement p = Place(t, *info, level);
    const uint64_t bytes = uint64_t(p.slice_stride) * t.slices();
    if (!p.address || p.address + bytes > mirror_bytes_) return error = "texture outside the mirrored memory", VK_NULL_HANDLE;
    span_begin = std::min(span_begin, p.address);
    span_end = std::max(span_end, uint32_t(p.address + bytes));
  }
  const uint32_t levels = last_level + 1;

  // A resolve's destination left on the GPU, read as it is: copied from there (no memory read, no hash).
  // Each slice of a 2D or stacked texture (one level) that a resolve wrote there: all of them, or none.
  std::vector<const GpuSource*> sources;
  uint64_t source_generation = 0;
  if (t.tiled && t.dimension == 1 && first_level == 0 && last_level == 0 &&
      (info->convert == TextureFormatInfo::kNone || info->convert == TextureFormatInfo::kDepthUnorm24 ||
       info->convert == TextureFormatInfo::kDepthFloat24)) {
    const bool depth_texture = info->convert == TextureFormatInfo::kDepthUnorm24 ||
                               info->convert == TextureFormatInfo::kDepthFloat24;
    const uint32_t slice_stride = Place(t, *info, 0).slice_stride;
    for (uint32_t slice = 0; slice < t.depth; ++slice) {
      const auto it = gpu_sources_.find((t.base & 0x1FFFFFFF) + slice * slice_stride);
      const bool same_bytes =
          it != gpu_sources_.end() &&
          (it->second.format == info->format ||
           (it->second.format == VK_FORMAT_B8G8R8A8_UNORM && info->format == VK_FORMAT_R8G8B8A8_UNORM) ||
           (it->second.format == VK_FORMAT_R16G16_SNORM && info->format == VK_FORMAT_R16G16_UNORM) ||
           (it->second.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 &&
            info->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32));
      if (!same_bytes || it->second.depth != depth_texture || it->second.pitch != t.pitch ||
          it->second.endian != t.endian || (!sources.empty() && it->second.scale != sources[0]->scale) ||
          t.width * it->second.scale > it->second.width || t.height * it->second.scale > it->second.height) {
        sources.clear();
        break;
      }
      sources.push_back(&it->second);
      source_generation = source_generation * 0x9E3779B97F4A7C15ull + it->second.generation;
    }
  }
  const bool source = !sources.empty();
  // Texels a guest texel along each axis: a scaled source's (its texture is a slot of its own, never one read from
  // memory at the guest resolution).
  const uint32_t scale = source ? sources[0]->scale : 1;
  const std::array<uint32_t, 5> key = {fetch[0] & 0xFFC00003, fetch[1], fetch[2], fetch[5] & 0xFFFFFE00,
                                       (fetch[4] & 0x3FC) | (scale > 1 ? 1u : 0u)};
  auto& slot = textures_[key];
  if (!source && Diagnosing()) {
    // For diagnostics: a GPU source within this texture's memory it could not use, and why.
    const auto near = gpu_sources_.lower_bound(span_begin);
    if (near != gpu_sources_.end() && near->first < span_end) {
      const GpuSource& s = near->second;
      char why[200];
      std::snprintf(why, sizeof(why), "fmt %u %ux%u depth %u dim %u tiled %u levels %u-%u pitch %u/%u at +0x%X (source %s %ux%u)",
                    t.format, t.width, t.height, t.depth, t.dimension, t.tiled, first_level, last_level, t.pitch, s.pitch,
                    near->first - (t.base & 0x1FFFFFFF), s.depth ? "depth" : "colour", s.width, s.height);
      if (Diagnosing()) ++source_misses_[why];
    }
  }
  // The content is hashed again only when memory may have changed since it last was (MemoryChanged, NewFrame).
  uint64_t hash = slot ? slot->content_hash : 0;
  // Memory only: the key holds what lays the data out, and sampling fields (clamp, filters) the title changes
  // between uses of one texture must not make it look new.
  // Watched memory: hashed again only once a write was reported (validated reset to 0); otherwise once a frame.
  const bool stale = !slot || (watch_ ? slot->validated == 0 : slot->validated != epoch_) || slot->gpu_generation;
  if (!source && stale) {
    if (before_read_) before_read_(span_begin, span_end);
    if (watch_) watch_(span_begin, span_end);  // armed before reading: a write from now on is reported
    hash = 0xCBF29CE484222325ull;
    const auto hash_start = std::chrono::steady_clock::now();
    for (uint32_t level = first_level; level <= last_level; ++level) {
      const Placement p = Place(t, *info, level);
      hash = hash * 0x100000001B3ull ^ HashMemory(physical_ + p.address, size_t(uint64_t(p.slice_stride) * t.slices()));
    }
    ns_hash_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hash_start).count());
  }
  const Functions& f = device_.fn();
  if (!slot) {
    slot = std::make_unique<Texture>();
    VkImageCreateInfo info_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info_ci.flags = (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    info_ci.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info_ci.format = info->format;
    info_ci.extent = {t.width * scale, t.height * scale, image_depth};
    info_ci.mipLevels = levels;
    info_ci.arrayLayers = layers;  // stacked textures: one layer a slice; cubes: a face
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
    slot->image.width = t.width * scale;
    slot->image.height = t.height * scale;
  }

  slot->validated = epoch_;
  slot->begin = span_begin, slot->end = span_end;
  if (source) {
    if (slot->gpu_generation != source_generation) {
      Image& image = slot->image;
      auto copy = [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        f.vkCmdPipelineBarrier2(cb, &dependency);
        // The destination's bytes as they are (a B8G8R8A8 or A2R10G10B10 source is a resolve with the red/blue swap).
        for (uint32_t slice = 0; slice < uint32_t(sources.size()); ++slice) {
          VkImageCopy region{};
          region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, slice, 1};
          region.extent = {t.width * scale, t.height * scale, 1};
          f.vkCmdCopyImage(cb, sources[slice]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        f.vkCmdPipelineBarrier2(cb, &dependency);
      };
      if (recorder_) {
        copy(recorder_());
      } else if (!device_.Submit(copy)) {
        return error = "texture copy from a resolve", VK_NULL_HANDLE;
      }
      slot->gpu_generation = source_generation;
      slot->content_hash = 0;  // read from memory again once the source is gone
      ++gpu_copies_;
    }
  } else if (slot->content_hash != hash) {
    slot->gpu_generation = 0;
    // Every level's blocks, one after the other (levels below the first are never sampled: left as they are).
    if (Diagnosing()) ++upload_log_[{t.base & 0x1FFFFFFF, t.format, t.width, t.height}];
    const auto load_start = std::chrono::steady_clock::now();
    std::vector<uint8_t>& blocks = upload_blocks_;
    std::vector<VkBufferImageCopy>& regions = upload_regions_;
    blocks.clear();
    regions.clear();
    for (uint32_t level = first_level; level <= last_level; ++level) {
      std::vector<uint8_t>& level_blocks = upload_level_;
      if (!LoadLevel(t, *info, level, level_blocks, error)) return VK_NULL_HANDLE;
      VkBufferImageCopy region{};
      region.bufferOffset = blocks.size();
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
      region.imageExtent = {std::max(t.width >> level, 1u), std::max(t.height >> level, 1u), image_depth};
      regions.push_back(region);
      blocks.insert(blocks.end(), level_blocks.begin(), level_blocks.end());
      blocks.resize((blocks.size() + 15) & ~size_t(15));  // offsets aligned for any texel size
    }
    CountSwapped(g_swapped.textures, blocks.size());
    ns_load_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - load_start).count());
    Buffer staging;
    if (!AcquireStaging(blocks.size(), staging)) {
      return error = "texture staging", VK_NULL_HANDLE;
    }
    std::memcpy(staging.mapped, blocks.data(), blocks.size());
    Image& image = slot->image;
    auto record = [&](VkCommandBuffer cb) {
      VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
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
    };
    if (recorder_) {
      record(recorder_());
      pending_staging_.push_back(staging);  // freed when the batch has run
    } else {
      const bool ok = device_.Submit(record);
      std::vector<Buffer> done{staging};
      RecycleStaging(done);
      if (!ok) return error = "texture upload", VK_NULL_HANDLE;
    }
    slot->content_hash = hash;
    ++uploads_;
  }

  // The view: the guest swizzle over the format's own (codes 6 and 7 read as 0 and 1, as the reference does).
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t guest = (t.swizzle >> (3 * i)) & 7;
    host_swizzle |= (guest >= 4 ? (guest & 5) : (info->swizzle >> (3 * guest)) & 7) << (3 * i);
  }
  VkImageView& view = slot->views[host_swizzle | (is_signed ? 1u << 16 : 0u)];
  if (!view) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = slot->image.image;
    vi.viewType = cube     ? VK_IMAGE_VIEW_TYPE_CUBE
                  : volume ? VK_IMAGE_VIEW_TYPE_3D
                           : VK_IMAGE_VIEW_TYPE_2D_ARRAY;  // 2D textures are declared arrayed
    vi.format = is_signed ? info->signed_format : info->format;
    vi.components = {Component(host_swizzle & 7), Component((host_swizzle >> 3) & 7), Component((host_swizzle >> 6) & 7),
                     Component((host_swizzle >> 9) & 7)};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    if (f.vkCreateImageView(device_.device(), &vi, nullptr, &view) != VK_SUCCESS) {
      view = VK_NULL_HANDLE;
      return error = "texture view", VK_NULL_HANDLE;
    }
  }
  if (fast_.size() > 65536) fast_.clear();
  fast_[fast_key] = {view, epoch_, version_, scale > 1, &slot->used};
  last_view_scaled_ = scale > 1;
  slot->used = epoch_;
  last_used_ = &slot->used;
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

// Whether a cached texture spans guest memory in [begin, end): only then may views cached by fetch constant be stale
// (a resolve every few draws otherwise sent every texture of the frame through the slow path again).
bool TextureCache::CachedOver(uint64_t begin, uint64_t end) const {
  for (const auto& [key, t] : textures_) {
    if (t->begin < end && begin < t->end) return true;
  }
  return false;
}

void TextureCache::SetGpuSource(const GpuSource& source) {
  gpu_sources_[source.base] = source;
  // The rows it covers, at most 8 bytes a texel (the format is the texture's to say).
  const uint64_t end = uint64_t(source.base) + uint64_t(source.pitch) * ((source.height + 31) & ~31u) * 8;
  if (CachedOver(source.base, end)) ++version_;
}

void TextureCache::DropGpuSources(uint32_t begin, uint32_t end) {
  if (CachedOver(begin, end)) ++version_;
  for (auto it = gpu_sources_.begin(); it != gpu_sources_.end();) {
    it = it->first >= begin && it->first < end ? gpu_sources_.erase(it) : std::next(it);
  }
}

bool TextureCache::AcquireStaging(VkDeviceSize size, Buffer& out) {
  {
    // The smallest free one that fits.
    std::lock_guard<std::mutex> lock(staging_mutex_);
    size_t best = free_staging_.size();
    for (size_t i = 0; i < free_staging_.size(); ++i) {
      if (free_staging_[i].size >= size && (best == free_staging_.size() || free_staging_[i].size < free_staging_[best].size)) best = i;
    }
    if (best != free_staging_.size()) {
      out = free_staging_[best];
      free_staging_[best] = free_staging_.back();
      free_staging_.pop_back();
      free_staging_bytes_ -= out.size;
      return true;
    }
  }
  // None: a new one, rounded up to a power of two (64 KB at least) so it serves other sizes later.
  return CreateBuffer(device_, std::max<VkDeviceSize>(VkDeviceSize(1) << (64 - std::countl_zero(uint64_t(size - 1) | 0xFFFF)), size),
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, out);
}

void TextureCache::RecycleStaging(std::vector<Buffer>& buffers) {
  constexpr VkDeviceSize kMaxFreeBytes = 64ull << 20;
  std::lock_guard<std::mutex> lock(staging_mutex_);
  for (Buffer& b : buffers) {
    if (free_staging_bytes_ + b.size > kMaxFreeBytes) {
      DestroyBuffer(device_, b);
    } else {
      free_staging_bytes_ += b.size;
      free_staging_.push_back(b);
    }
  }
  buffers.clear();
}

void TextureCache::TakeStaging(std::vector<Buffer>& out) {
  out.insert(out.end(), pending_staging_.begin(), pending_staging_.end());
  pending_staging_.clear();
}

void TextureCache::QueueMemoryChanged(uint32_t begin, uint32_t end) {
  std::lock_guard lock(changed_mutex_);
  changed_.emplace_back(begin, end);
  any_changed_.store(true, std::memory_order_release);
}

void TextureCache::MemoryChanged(uint32_t begin, uint32_t end) {
  bool changed = false;
  for (auto& [key, texture] : textures_) {
    // (A texture being created right now has no entry yet: View reads memory after this.)
    if (texture && texture->begin < end && begin < texture->end) texture->validated = 0, changed = true;
  }
  if (changed) ++version_;  // the views cached by fetch constant are checked again
}

void TextureCache::MemoryChanged(std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (ranges.empty()) return;
  std::sort(ranges.begin(), ranges.end());
  size_t merged = 0;
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first <= ranges[merged].second) {
      ranges[merged].second = std::max(ranges[merged].second, ranges[i].second);
    } else {
      ranges[++merged] = ranges[i];
    }
  }
  ranges.resize(merged + 1);
  bool changed = false;
  for (auto& [key, texture] : textures_) {
    if (!texture) continue;
    // The first range ending after the texture's start (the merged ranges' ends are sorted too).
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), texture->begin,
                                     [](uint32_t at, const std::pair<uint32_t, uint32_t>& r) { return at < r.second; });
    if (it != ranges.end() && it->first < texture->end) texture->validated = 0, changed = true;
  }
  if (changed) ++version_;
}

}  // namespace any360::gfx::vk
