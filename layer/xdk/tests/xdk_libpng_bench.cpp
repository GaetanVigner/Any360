// Timings of libpng 1.0.5's png_read_filter_row (any360/xdk/libpng.h) against its model and the version first written
// (kept here as the fixed point, never edited): nanoseconds a row and a byte, by filter and pixel size, on smooth rows
// (what images give). Not a test. The figures that decide a binding are the title's own, with --any360_xdk_bench:
// docs/NATIVE_FUNCTIONS.md.
// Run: xdk_libpng_bench [repeats]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "xdk_bench.h"

#include "any360/xdk/libpng.h"
#include "any360/xdk/libpng_reference.h"

namespace first {
using any360::xdk::IsPlainMemory;
using any360::xdk::Memory;

NOINLINE bool PngReadFilterRow(const Memory& memory, uint32_t row_info, uint32_t row, uint32_t prev, uint32_t filter) {
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
  for (uint32_t i = bpp; i < rowbytes; ++i) {
    const int a = r[i - bpp], b = p[i], c = p[i - bpp];
    const int pa = b - c < 0 ? c - b : b - c, pb = a - c < 0 ? c - a : a - c;
    const int pc = a + b - 2 * c < 0 ? 2 * c - a - b : a + b - 2 * c;
    r[i] = uint8_t(r[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
  }
  return true;
}

}  // namespace first

namespace {

namespace xdk = any360::xdk;

template <class F>
double NsPerCall(F&& f, int calls) {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < calls; ++i) f();
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  xdk_bench::Allocated& a = xdk_bench::g_allocated;
  a.calls += uint64_t(calls), a.bytes += spent.bytes, a.count += spent.count;
  a.max_bytes_a_call = std::max(a.max_bytes_a_call, double(spent.bytes) / double(calls));
  return ns;
}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  std::vector<uint8_t> arena(1u << 20, 0);
  xdk::Memory memory;
  memory.base = arena.data();
  constexpr uint32_t kInfo = 0x1000, kRow = 0x10000, kPrev = 0x40000;
  std::mt19937 rng(7);
  uint32_t v = 0;
  for (uint32_t i = kRow; i < kPrev + 0x10000; ++i) arena[i] = uint8_t(v = (v + rng() % 7 - 3) & 0xFF);
  const char* names[] = {"", "Sub", "Up", "Average", "Paeth"};
  for (int r = 0; r < repeats; ++r) {
    for (uint32_t depth : {8u, 24u, 32u}) {
      const uint32_t rowbytes = 1024 * depth / 8;
      arena[kInfo + 4] = uint8_t(rowbytes >> 24), arena[kInfo + 5] = uint8_t(rowbytes >> 16);
      arena[kInfo + 6] = uint8_t(rowbytes >> 8), arena[kInfo + 7] = uint8_t(rowbytes);
      arena[kInfo + 11] = uint8_t(depth);
      for (uint32_t filter = 1; filter <= 4; ++filter) {
        const int calls = 2000;
        const double current = NsPerCall([&] { xdk_bench::g_sink = xdk::PngReadFilterRow(memory, kInfo, kRow, kPrev, filter); }, calls);
        const double original = NsPerCall([&] { xdk_bench::g_sink = first::PngReadFilterRow(memory, kInfo, kRow, kPrev, filter); }, calls);
        const double model = NsPerCall([&] { xdk_bench::g_sink = xdk::reference::PngReadFilterRow(memory, kInfo, kRow, kPrev, filter); }, calls / 4);
        std::printf("png_read_filter_row %-7s %2u-bit pixels, %4u bytes: current %7.0f ns (%.2f ns a byte), first %7.0f ns "
                    "(%.2fx), model %7.0f ns (%.2fx)\n",
                    names[filter], depth, rowbytes, current, current / rowbytes, original, original / current, model,
                    model / current);
      }
    }
  }
  xdk_bench::PrintAllocated("xdk_libpng_bench");
  return 0;
}
