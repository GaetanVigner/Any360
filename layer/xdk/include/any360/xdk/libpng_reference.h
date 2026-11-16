// The reference model of libpng 1.0.5's png_read_filter_row (any360/xdk/libpng.h has the native one): the function's
// own PowerPC written out step by step, the plainest way (the same instructions in every copy read so far:
// db/xdk_seed.json). The unit tests hold the native one to it.
//
// r3 png_ptr, r4 row_info (rowbytes +4, pixel_depth +11), r5 row, r6 prev_row, r7 the filter. Each byte of the row
// gets its predictor added, in order, byte by byte (a byte reads the row's bytes before it once they are done):
// 1 Sub (the byte bpp before), 2 Up (the previous row's), 3 Average (half their sum), 4 Paeth. bpp is
// (pixel_depth + 7) >> 3. 0 leaves the row. Above 4 the function calls png_warning and writes 0 at row[0]: the model
// does not, and returns false. Neither does it follow the two loops that do not end (Average and Paeth with bpp
// above rowbytes: their second loop counts down from below 0): it returns false there too, with nothing written.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "any360/xdk/crt.h"

namespace any360::xdk::reference {

inline bool PngReadFilterRow(const Memory& memory, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  const auto l8 = [&](uint32_t a) -> uint32_t { return *memory.Host(a); };
  const auto s8 = [&](uint32_t a, uint32_t v) { *memory.Host(a) = uint8_t(v); };
  const auto l32 = [&](uint32_t a) -> uint32_t {
    uint32_t v;
    std::memcpy(&v, memory.Host(a), 4);
    return std::byteswap(v);
  };
  const uint32_t r31 = r5, r10_prev = r6;
  if (r7 >= 5) return false;  // png_warning, then row[0] = 0
  if (r7 == 0) return true;
  const uint32_t rowbytes = l32(r4 + 4);
  const int32_t bpp = int32_t(l8(r4 + 11) + 7) >> 3;  // srawi
  if (r7 == 1) {  // Sub (loc_82A7F7A8)
    if (uint32_t(bpp) >= rowbytes) return true;
    uint32_t r11 = r31 + uint32_t(bpp), r9 = r31, r10 = rowbytes - uint32_t(bpp);
    do s8(r11, l8(r9) + l8(r11)), r9 += 1, r11 += 1;
    while (--r10);
    return true;
  }
  if (r7 == 2) {  // Up (loc_82A7F774)
    for (uint32_t i = 0; i < rowbytes; ++i) s8(r31 + i, l8(r10_prev + i) + l8(r31 + i));
    return true;
  }
  if ((bpp > 0 && uint32_t(bpp) > rowbytes) || bpp < 0) return false;  // the second loop would not end
  uint32_t rp = r31, pp = r10_prev;
  for (int32_t i = 0; i < bpp; ++i) {  // the first bpp bytes: half the byte above (Average), the byte above (Paeth)
    s8(rp, (r7 == 3 ? l8(pp) >> 1 : l8(pp)) + l8(rp));
    rp += 1, pp += 1;
  }
  uint32_t count = rowbytes - uint32_t(bpp), lp = r31, cp = r10_prev;
  if (count == 0) return true;
  if (r7 == 3) {  // Average (loc_82A7F73C)
    do s8(rp, ((l8(lp) + l8(pp)) >> 1) + l8(rp)), lp += 1, pp += 1, rp += 1;
    while (--count);
    return true;
  }
  do {  // Paeth (loc_82A7F664)
    const int32_t c = int32_t(l8(cp)), b = int32_t(l8(pp)), a = int32_t(l8(lp));
    cp += 1, pp += 1, lp += 1;
    const int32_t p = b - c, pc0 = a - c;
    const int32_t pa = p < 0 ? -p : p, pb = pc0 < 0 ? -pc0 : pc0, pc = (p + pc0) < 0 ? -(p + pc0) : p + pc0;
    int32_t predictor;
    if (pa > pb || pa > pc) {
      predictor = pb <= pc ? b : c;
    } else {
      predictor = a;
    }
    s8(rp, l8(rp) + uint32_t(predictor));
    rp += 1;
  } while (--count);
  return true;
}

}  // namespace any360::xdk::reference
