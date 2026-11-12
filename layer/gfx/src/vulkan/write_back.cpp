#include "any360/gfx/vulkan/write_back.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <emmintrin.h>
#include <tmmintrin.h>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/textures.h"

namespace any360::gfx::vk {

namespace {

// Byte orders as 16-byte shuffles: none, 8 in 16, 8 in 32, 16 in 32 (each 32-bit word), 8 in 64 (8-byte elements).
alignas(16) constexpr uint8_t kShuffles[5][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
    {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12}, {2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13},
    {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8}};

uint32_t ShuffleIndex(const WriteBack& w) {
  return w.depth || w.endian > 4 || (w.endian == 4 && w.element != 8) ? 0 : w.endian;
}

// The guest's depth word (depth << 8, stencil not kept: 0), as the CPU depth resolve writes it.
uint32_t DepthWord(const WriteBack& w, const uint8_t* at) {
  float value;
  std::memcpy(&value, at, 4);
  const uint32_t guest =
      w.float24 ? FloatTo20e4(value) : uint32_t(std::lround(std::clamp(value, 0.0f, 1.0f) * float(0xFFFFFF)));
  return GpuSwap(guest << 8, Endian(w.endian <= 3 ? w.endian : 0));
}

// The same, without a call a pixel: float24 as FloatTo20e4 does it (textures.cpp), inlined; unorm rounded half
// away from zero in double, as lround does on these non-negative values (a float above 2^23 is a whole number,
// which adding 0.5 in float would round to even); a NaN as lround leaves it.
inline uint32_t FastFloatTo20e4(float value) {
  uint32_t f32 = std::bit_cast<uint32_t>(value);
  if (int32_t(f32) <= 0 || f32 > 0x7F800000u) return 0;  // negative, zero, NaN
  f32 = std::min(f32, 0x3FFFFFF8u);
  if (f32 < 0x38800000u) {
    const uint32_t shift = std::min(113u - (f32 >> 23), 24u);
    f32 = (0x800000u | (f32 & 0x7FFFFFu)) >> shift;
  } else {
    f32 += 0xC8000000u;
  }
  f32 += 3 + ((f32 >> 3) & 1);
  return (f32 >> 3) & 0xFFFFFF;
}
inline uint32_t FastDepthWord(const WriteBack& w, const uint8_t* at) {
  float value;
  std::memcpy(&value, at, 4);
  uint32_t guest;
  if (w.float24) {
    guest = FastFloatTo20e4(value);
  } else {
    const float scaled = std::clamp(value, 0.0f, 1.0f) * float(0xFFFFFF);
    guest = scaled == scaled ? uint32_t(double(scaled) + 0.5) : uint32_t(std::lround(scaled));
  }
  return GpuSwap(guest << 8, Endian(w.endian <= 3 ? w.endian : 0));
}

// One element in the destination's byte order (Endian128 on a 32- or 64-bit element).
void SwapElement(uint8_t* bytes, uint32_t size, uint32_t endian) {
  if (endian >= 1 && endian <= 3) {
    for (uint32_t i = 0; i < size; i += 4) {
      uint32_t v;
      std::memcpy(&v, bytes + i, 4);
      v = GpuSwap(v, Endian(endian));
      std::memcpy(bytes + i, &v, 4);
    }
  } else if (endian == 4 && size == 8) {  // 8 in 64
    std::reverse(bytes, bytes + 8);
  }
}

// SSSE3's byte shuffle (every x86-64 CPU this runs on has it; the build targets the baseline).
#if defined(__clang__) || defined(__GNUC__)
#define ANY360_SSSE3 __attribute__((target("ssse3")))
#else
#define ANY360_SSSE3
#endif

ANY360_SSSE3 __m128i Shuffle(__m128i v, __m128i mask) { return _mm_shuffle_epi8(v, mask); }

// An element that does not start a 16-byte group, or ends a row short of one: alone.
template <bool kFast = false>
void WriteOne(uint8_t* physical, uint32_t mirror_bytes, const uint8_t* from, uint32_t address, const WriteBack& w) {
  uint8_t bytes[8];
  std::memcpy(bytes, from, w.element);
  if (w.depth) {
    const uint32_t word = kFast ? FastDepthWord(w, bytes) : DepthWord(w, bytes);
    std::memcpy(bytes, &word, 4);
  } else {
    SwapElement(bytes, w.element, w.endian);
  }
  if (uint64_t(address) + w.element <= mirror_bytes) std::memcpy(physical + address, bytes, w.element);
}

}  // namespace

void WriteBackTiledReference(uint8_t* physical, uint32_t mirror_bytes, const uint8_t* src, const WriteBack& w) {
  const uint32_t element = w.element, bpp_log2 = element == 8 ? 3 : 2;
  // 16 bytes of a tiled row are contiguous from an x aligned to their element count (4 of 4 bytes, 2 of 8): one
  // tiled offset and one shuffle for them.
  const uint32_t group = 16 / element;
  const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(kShuffles[ShuffleIndex(w)]));
  for (uint32_t y = 0; y < w.height; ++y) {
    const uint8_t* row = src + size_t(y) * w.width * element;
    for (uint32_t x = 0; x < w.width;) {
      const uint32_t tx = w.x + x;
      const uint32_t address = w.base + uint32_t(TiledOffset2D(int32_t(tx), int32_t(w.y + y), w.pitch, bpp_log2));
      if (tx % group == 0 && x + group <= w.width) {
        alignas(16) uint8_t bytes[16];
        if (w.depth) {
          for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t word = DepthWord(w, row + size_t(x + i) * 4);
            std::memcpy(bytes + 4 * i, &word, 4);
          }
        } else {
          const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + size_t(x) * element));
          _mm_store_si128(reinterpret_cast<__m128i*>(bytes), Shuffle(v, mask));
        }
        if (uint64_t(address) + 16 <= mirror_bytes) std::memcpy(physical + address, bytes, 16);
        x += group;
        continue;
      }
      WriteOne(physical, mirror_bytes, row + size_t(x) * element, address, w);
      ++x;
    }
  }
}

// The tiled offset of (x, y) is that of (x mod 32, y) plus (x / 32) << (log2 bytes + 10): the 32-element step of the
// macro tile lands in the offset's bits from 9 up (log2 bytes 2 or 3), which the tiling moves as a whole (shifted
// left by 3), and nothing else depends on x / 32. So a row's offsets are one table of the 16-byte groups of the
// first 32 elements (8 groups of 4-byte elements, 16 of 8-byte ones) and a step a span; the byte shuffle runs in
// the loop instead of a call each group.
ANY360_SSSE3 void WriteBackTiled(uint8_t* physical, uint32_t mirror_bytes, const uint8_t* src, const WriteBack& w) {
  const uint32_t element = w.element, bpp_log2 = element == 8 ? 3 : 2;
  const uint32_t group = 16 / element, groups_a_span = 32 / group;
  const uint32_t span_step = 1u << (bpp_log2 + 10);
  const __m128i mask = _mm_load_si128(reinterpret_cast<const __m128i*>(kShuffles[ShuffleIndex(w)]));
  uint32_t table[16];
  for (uint32_t y = 0; y < w.height; ++y) {
    const uint32_t ty = w.y + y;
    for (uint32_t g = 0; g < groups_a_span; ++g) table[g] = uint32_t(TiledOffset2D(int32_t(g * group), int32_t(ty), w.pitch, bpp_log2));
    const uint8_t* row = src + size_t(y) * w.width * element;
    uint32_t x = 0;
    // The elements before the first whole group, alone.
    while (x < w.width && (w.x + x) % group != 0) {
      const uint32_t tx = w.x + x;
      WriteOne<true>(physical, mirror_bytes, row + size_t(x) * element,
               w.base + uint32_t(TiledOffset2D(int32_t(tx), int32_t(ty), w.pitch, bpp_log2)), w);
      ++x;
    }
    // Whole groups.
    for (; x + group <= w.width; x += group) {
      const uint32_t tx = w.x + x;
      const uint32_t address = w.base + table[(tx & 31) / group] + (tx >> 5) * span_step;
      if (uint64_t(address) + 16 > mirror_bytes) continue;
      if (w.depth) {
        uint32_t words[4];
        for (uint32_t i = 0; i < 4; ++i) words[i] = FastDepthWord(w, row + size_t(x + i) * 4);
        std::memcpy(physical + address, words, 16);
      } else {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + size_t(x) * element));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(physical + address), _mm_shuffle_epi8(v, mask));
      }
    }
    // The rest of the row, alone.
    for (; x < w.width; ++x) {
      const uint32_t tx = w.x + x;
      WriteOne<true>(physical, mirror_bytes, row + size_t(x) * element,
               w.base + uint32_t(TiledOffset2D(int32_t(tx), int32_t(ty), w.pitch, bpp_log2)), w);
    }
  }
}

}  // namespace any360::gfx::vk
