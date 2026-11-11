// Timings of the D3D replacements (any360/xdk/d3d.h) against their models: nanoseconds a call by mask. Not a test.
// The figures that decide a binding are the title's own (--any360_xdk_bench): docs/NATIVE_FUNCTIONS.md.
// Run: xdk_d3d_bench [repeats]
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "any360/xdk/d3d.h"
#include "any360/xdk/d3d_reference.h"

namespace {

namespace xdk = any360::xdk;

template <class F>
double NsPerCall(F&& f, int calls) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < calls; ++i) f();
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  std::vector<uint8_t> arena(4u << 20, 0x11);
  xdk::Memory memory;
  memory.base = arena.data();
  constexpr uint32_t kDevice = 0x300000, kOffset = 48, kBuffer = 0x100000, kSource = 0x200000;
  const auto reset = [&] {
    const uint32_t cursor = std::byteswap(kBuffer), end = std::byteswap(kBuffer + 0x80000);
    std::memcpy(&arena[kDevice + kOffset], &cursor, 4);
    std::memcpy(&arena[kDevice + kOffset + 4], &end, 4);
  };
  for (int r = 0; r < repeats; ++r) {
    for (uint64_t mask : {0x8000000000000000ull, 0xF000000000000000ull, 0xFFFF000000000000ull, 0x7FFFFFFFFFFFFFFFull,
                          0xFFFFFFFFFFFFFFFEull, ~0ull, 0x5555555555555555ull}) {
      uint64_t index = 0, cursor = 0;
      const double native = NsPerCall([&] {
        reset();
        index = 0;
        xdk::D3dDirtyConstants(memory, kDevice, kOffset, mask, index, kSource, cursor);
      }, 200000);
      const double model = NsPerCall([&] {
        reset();
        uint64_t r4 = mask, r5 = 0;
        xdk::reference::D3dDirtyConstants(memory, kOffset, kDevice, r4, r5, kSource);
      }, 20000);
      std::printf("dirty constants mask %016llX (%2d constants): replacement %7.1f ns, model %8.1f ns\n",
                  static_cast<unsigned long long>(mask), std::popcount(mask), native, model);
    }
  }
  return 0;
}
