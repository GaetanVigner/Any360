// Unit test and timings of the resolve write-back (any360/gfx/vulkan/write_back.h): WriteBackTiled against
// WriteBackTiledReference (its first version) on random rectangles, both element sizes, every byte order, depth
// unorm and float24, rectangles at any offset and width, and one past the end of guest memory. Then both timed on
// a 1280x720 colour resolve and a depth one.
// Run: gfx_write_back_tests [--bench]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "any360/gfx/vulkan/write_back.h"

namespace {

using any360::gfx::vk::WriteBack;
int g_failures = 0;
uint64_t g_checks = 0;
#define CHECK(condition)                                                                     \
  do {                                                                                       \
    ++g_checks;                                                                              \
    if (!(condition) && ++g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  } while (0)

constexpr uint32_t kMemory = 8u << 20;

void Random() {
  std::mt19937 rng(41);
  std::vector<uint8_t> a(kMemory), b(kMemory), src;
  for (int round = 0; round < 3000; ++round) {
    WriteBack w;
    w.element = rng() % 3 == 0 ? 8 : 4;
    w.depth = w.element == 4 && rng() % 4 == 0;
    w.float24 = rng() % 2;
    w.endian = rng() % 6;
    w.pitch = 32 * (1 + rng() % 12) + (rng() % 3 == 0 ? rng() % 32 : 0);
    w.x = rng() % 3 == 0 ? rng() % 70 : 32 * (rng() % 3);
    w.y = rng() % 50;
    w.width = 1 + rng() % std::min<uint32_t>(w.pitch, 200);
    w.height = 1 + rng() % 40;
    w.base = (rng() % 64) << 12;
    if (round % 100 == 0) w.base = kMemory - 4096 * (1 + rng() % 4);  // its last rows past the end
    src.resize(size_t(w.width) * w.height * w.element);
    for (uint8_t& v : src) v = uint8_t(rng());
    if (w.depth && rng() % 2) {  // depth values as a GPU leaves them, now and then
      for (size_t i = 0; i + 4 <= src.size(); i += 4) {
        const float f = float(rng() % 100001) / 100000.0f;
        std::memcpy(&src[i], &f, 4);
      }
    }
    const uint8_t fill = uint8_t(rng());
    std::memset(a.data(), fill, a.size());
    std::memset(b.data(), fill, b.size());
    any360::gfx::vk::WriteBackTiled(a.data(), kMemory, src.data(), w);
    any360::gfx::vk::WriteBackTiledReference(b.data(), kMemory, src.data(), w);
    CHECK(a == b);
  }
}

void Bench() {
  std::vector<uint8_t> memory(kMemory * 2), src(1280 * 720 * 4);
  std::mt19937 rng(43);
  for (uint8_t& v : src) v = uint8_t(rng());
  for (bool depth : {false, true}) {
    if (depth) {  // depth as a GPU leaves it: floats in [0, 1]
      for (size_t i = 0; i + 4 <= src.size(); i += 4) {
        const float f = float(rng() % 1000001) / 1000000.0f;
        std::memcpy(&src[i], &f, 4);
      }
    }
    WriteBack w;
    w.width = 1280, w.height = 720, w.pitch = 1280, w.endian = 2, w.element = 4, w.depth = depth, w.float24 = depth;
    for (int repeat = 0; repeat < 3; ++repeat) {
      const auto time = [&](auto&& f) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) f(memory.data(), uint32_t(memory.size()), src.data(), w);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
      };
      const double reference = time(any360::gfx::vk::WriteBackTiledReference);
      const double tiled = time(any360::gfx::vk::WriteBackTiled);
      std::printf("write-back 1280x720 %s: first version %.3f ms, now %.3f ms (%.1fx)\n", depth ? "depth" : "colour",
                  reference, tiled, reference / tiled);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Random();
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
  std::printf("gfx_write_back_tests: %llu checks, %d failing\n", static_cast<unsigned long long>(g_checks), g_failures);
  return g_failures ? 1 : 0;
}
