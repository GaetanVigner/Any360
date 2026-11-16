// libpng 1.0.5's png_read_filter_row, native: the undoing of a PNG row's filter, every row of every PNG image a title
// reads (where each title has it: db/xdk_seed.json). Its model, the function's own PowerPC step by step, is
// any360/xdk/libpng_reference.h; the registers and the filters are described there.
//
// It declines (returns false, nothing written; the function's own code runs) a filter above 4 (the function warns
// through png_warning), a pixel depth of 0, a row (and for Up, Average and Paeth the previous row) that is not plain
// memory, a previous row that overlaps the row, Average and Paeth with bpp above rowbytes (the function's loop does
// not end), and a row_info that is not plain memory. Writes the row's rowbytes bytes and nothing else: the register
// save and the frame the function leaves below r1 are the binding's to write (any360/host/xdk_natives.h).
// Allocates nothing.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define ANY360_XDK_PAETH_SSE2 1
#endif

#include "any360/xdk/crt.h"

namespace any360::xdk {

namespace detail {

// Paeth from byte `from` on (the bytes before it done), one byte at a time. (Choosing the predictor with masks instead
// of branches measured slower on 8-bit pixels, 0.93x: not kept.)
inline void PaethBytes(uint8_t* r, const uint8_t* p, uint32_t bpp, uint32_t from, uint32_t rowbytes) {
  for (uint32_t i = from; i < rowbytes; ++i) {
    const int a = r[i - bpp], b = p[i], c = p[i - bpp];
    const int pa = b - c < 0 ? c - b : b - c, pb = a - c < 0 ? c - a : a - c;
    const int pc = a + b - 2 * c < 0 ? 2 * c - a - b : a + b - 2 * c;
    r[i] = uint8_t(r[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
  }
}

#if ANY360_XDK_PAETH_SSE2
// Paeth a pixel of kBpp bytes at a time (its bytes are independent; a pixel needs the one before it done), in 16-bit
// lanes; the bytes left over (a row that is not whole pixels) one at a time.
template <uint32_t kBpp>
inline void PaethPixels(uint8_t* r, const uint8_t* p, uint32_t rowbytes) {
  const __m128i zero = _mm_setzero_si128(), low = _mm_set1_epi16(0xFF);
  const auto load = [&](const uint8_t* s) {
    uint64_t v = 0;
    std::memcpy(&v, s, kBpp);
    return _mm_unpacklo_epi8(_mm_cvtsi64_si128(int64_t(v)), zero);
  };
  const auto abs16 = [&](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
  __m128i a = load(r), c = load(p);
  uint32_t i = kBpp;
  for (; i + kBpp <= rowbytes; i += kBpp) {
    const __m128i b = load(p + i), x = load(r + i);
    const __m128i d1 = _mm_sub_epi16(b, c), d2 = _mm_sub_epi16(a, c);
    const __m128i pa = abs16(d1), pb = abs16(d2), pc = abs16(_mm_add_epi16(d1, d2));
    // pa <= pb && pa <= pc: a; else pb <= pc: b; else c.
    const __m128i not_a = _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
    const __m128i c_not_b = _mm_cmpgt_epi16(pb, pc);
    const __m128i bc = _mm_or_si128(_mm_andnot_si128(c_not_b, b), _mm_and_si128(c_not_b, c));
    const __m128i predictor = _mm_or_si128(_mm_andnot_si128(not_a, a), _mm_and_si128(not_a, bc));
    const __m128i out = _mm_and_si128(_mm_add_epi16(x, predictor), low);
    const uint64_t bytes = uint64_t(_mm_cvtsi128_si64(_mm_packus_epi16(out, zero)));
    std::memcpy(r + i, &bytes, kBpp);
    a = out, c = b;
  }
  PaethBytes(r, p, kBpp, i, rowbytes);
}
#endif

}  // namespace detail

inline bool PngReadFilterRow(const Memory& memory, uint32_t row_info, uint32_t row, uint32_t prev, uint32_t filter) {
  if (filter > 4 || !IsPlainMemory(row_info, 12)) return false;
  if (filter == 0) return true;
  uint32_t rowbytes;
  std::memcpy(&rowbytes, memory.Host(row_info + 4), 4);
  rowbytes = std::byteswap(rowbytes);
  const uint32_t bpp = (uint32_t(*memory.Host(row_info + 11)) + 7) >> 3;
  if (bpp == 0 || !IsPlainMemory(row, rowbytes)) return false;
  uint8_t* const r = memory.Host(row);
  if (filter == 1) {  // Sub: each byte gets the one bpp before it, done
    for (uint32_t i = bpp; i < rowbytes; ++i) r[i] = uint8_t(r[i] + r[i - bpp]);
    return true;
  }
  if (!IsPlainMemory(prev, rowbytes) || (prev < row + rowbytes && row < prev + rowbytes)) return false;
  const uint8_t* const p = memory.Host(prev);
  if (filter == 2) {  // Up
    for (uint32_t i = 0; i < rowbytes; ++i) r[i] = uint8_t(r[i] + p[i]);
    return true;
  }
  if (bpp > rowbytes) return false;
  if (filter == 3) {  // Average
    for (uint32_t i = 0; i < bpp; ++i) r[i] = uint8_t(r[i] + (p[i] >> 1));
    for (uint32_t i = bpp; i < rowbytes; ++i) r[i] = uint8_t(r[i] + ((r[i - bpp] + p[i]) >> 1));
    return true;
  }
  for (uint32_t i = 0; i < bpp; ++i) r[i] = uint8_t(r[i] + p[i]);  // Paeth
#if ANY360_XDK_PAETH_SSE2
  switch (bpp) {
    case 3: detail::PaethPixels<3>(r, p, rowbytes); return true;
    case 4: detail::PaethPixels<4>(r, p, rowbytes); return true;
    case 6: detail::PaethPixels<6>(r, p, rowbytes); return true;
    case 8: detail::PaethPixels<8>(r, p, rowbytes); return true;
    default: break;
  }
#endif
  detail::PaethBytes(r, p, bpp, bpp, rowbytes);
  return true;
}

}  // namespace any360::xdk
