// What the unit tests of any360::xdk share: the checks' counters and two copies of the same guest memory, one for
// the replacement and one for its reference model.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "any360/xdk/crt.h"

namespace xdk_test {

inline int g_failures = 0;
inline uint64_t g_checks = 0;
#define CHECK(condition)                                                                  \
  do {                                                                                    \
    ++xdk_test::g_checks;                                                                 \
    if (!(condition)) {                                                                   \
      if (++xdk_test::g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                                     \
  } while (0)

// Two copies of the same guest memory: the replacement works on one, the model on the other.
constexpr uint32_t kArena = 1u << 20;      // guest addresses [0, kArena)
constexpr uint32_t kStack = kArena - 256;  // the stack pointer of the calls: its slot sits at kStack - 8
struct Pair {
  std::vector<uint8_t> native, model;
  any360::xdk::Memory a, b;
  explicit Pair(uint32_t seed) : native(kArena), model(kArena) {
    std::mt19937 rng(seed);
    for (uint32_t i = 0; i < kArena; i += 4) {
      const uint32_t v = rng();
      std::memcpy(&native[i], &v, 4);
    }
    model = native;
    a.base = native.data();
    b.base = model.data();
  }
  // The two memories agree over [begin, end), a margin around it and at the stack slot.
  bool Same(uint32_t begin, uint32_t end, uint32_t margin = 64) const {
    begin = begin > margin ? begin - margin : 0;
    end = end + margin < kArena ? end + margin : kArena;
    return std::memcmp(&native[begin], &model[begin], end - begin) == 0 &&
           std::memcmp(&native[kStack - 16], &model[kStack - 16], 32) == 0;
  }
  bool SameEverywhere() const { return native == model; }
};

inline int Finish(const char* name) {
  std::printf("%s: %llu checks, %d failing\n", name, static_cast<unsigned long long>(g_checks), g_failures);
  return g_failures ? 1 : 0;
}

}  // namespace xdk_test
